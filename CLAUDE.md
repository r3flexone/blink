# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 SBB (Swiss Federal Railways) Departure Monitor. The device:
- Wakes from deep sleep on a schedule (active time window, default 06:45–07:00) or via button press (GPIO 0).
- Falls back to AP mode (SSID `SBB-Monitor`, IP `192.168.4.1`) when no WiFi is reachable, for first-time setup or after a router change.
- Fetches the next departures for a configured station from `transport.opendata.ch`.
- Displays them on an SSD1306 128×64 I2C OLED and signals overall status via a WS2812 NeoPixel (worst-of-4 rule).
- Returns to deep sleep to minimise battery usage.
- Serves a web configuration panel at `http://sbb-monitor.local` while active.

## Build / flash / monitor

Standard ESP-IDF v6.x project (also builds on v5.3+, where `driver` was split into the `esp_driver_*` components). Target is `esp32s3`.

```
# one-time: create WiFi credentials (gitignored)
cp main/secrets.h.example main/secrets.h   # edit WIFI_SSID / WIFI_PASS

idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor               # Ctrl-] to exit monitor
```

On first flash or after partition table changes: `idf.py fullclean` before build.

There is no linter or test harness. If ESP-IDF isn't available in the environment, `gcc -fsyntax-only -Wall -Wextra` against hand-written stub headers for the IDF APIs still catches typos, wrong types and missing includes across all of `main/*.c` — worth doing before claiming a change compiles, and it's the only build verification available in that case. It does **not** substitute for a real build or a hardware test; say so explicitly when that's all that was run.

## Git workflow

Develop on the active feature branch, not `main`. User-facing configuration values (station, filters, time windows, etc.) are managed via NVS and the web panel — do not hardcode them in source. If editing `main.c` defaults in `nvs_config.c`, avoid overwriting values the user may have customised.

## Architecture

All application code lives in `main/`. Source files:

- **`main/main.c`** — hardware drivers and the active-window main loop.
- **`main/sbb.c` / `sbb.h`** — WiFi, HTTP, JSON parsing, filter logic. Public API: `sbb_wifi_init()` and `sbb_get_departures()`.
- **`main/nvs_config.c` / `nvs_config.h`** — all configuration in NVS. `blink_config_t` is the central struct. Defaults in `nvs_config_defaults()`.
- **`main/http_server.c` / `http_server.h`** — web panel (SPIFFS + `/api/config` GET/POST + `/api/status` GET + `/api/departures` GET + `/api/restart` POST). Sets `g_cfg_dirty = true` after successful save so the main loop reloads cfg. Optional HTTP Basic Auth via `panelPass` (empty = no auth, the default). `/api/departures` serves `g_last_deps[]`, written by the main loop. Request bodies are read in a loop up to `req->content_len` — a single `httpd_req_recv()` only returns what is currently in the socket, so a config body spanning more than one TCP segment used to arrive truncated and fail as "Invalid JSON".
- **`main/spiffs/index.html`** — web panel UI, flashed to SPIFFS. See "Web panel" below.
- **`main/cJSON.c` / `cJSON.h`** — vendored JSON library, do not modify.

### Configuration

All tunables live in `blink_config_t` (`nvs_config.h`). They are:
- Loaded from NVS at startup via `nvs_config_load()`.
- Editable at runtime via `http://sbb-monitor.local` while the device is active.
- Persisted to NVS on save; survive deep sleep and reboots.
- `secrets.h` (`WIFI_SSID` / `WIFI_PASS`) is a compile-time fallback if NVS has no credentials.

When adding a new tunable: add the field to `blink_config_t`, set a default in `nvs_config_defaults()`, add a range check to `nvs_config_sanitize()`, add NVS load/save with a key ≤ 15 chars, and add the field to `handler_config_get()` and `handler_config_post()` in `http_server.c`.

`nvs_config_sanitize()` clamps every field to a plausible range. It runs at the end of `nvs_config_load()` and again in `handler_config_post()` before the save, so neither a stale NVS entry nor a hand-crafted `POST /api/config` can store a value that bricks the device (e.g. `apiRetryCount = 0` → the retry loop never runs, `refresh*Sec = 0` → the API is hammered without pause).

### Wake-up and sleep flow (`app_main` in `main.c`)

1. Check wake cause. `ESP_SLEEP_WAKEUP_EXT1` = button on GPIO 0.
2. Init LED and OLED, set TZ (`CET-1CEST,M3.5.0,M10.5.0/3`).
3. On a button wake: measure the hold to pick short/long active duration, then **wait for release** before continuing (see "Button handling").
4. `time()` + `localtime_r()` — the ESP32 RTC keeps time across deep sleep, so a wake with `tm_year >= 100` skips the cold-start WiFi+NTP screen.
5. Compute `in_window = time_valid && !weekend_skip && inside_active_time`.
6. **If not in window and not woken by button and `sleepEnabled = true` → sleep.** If `weekendSleepEnabled` and inside the weekend window: sleep directly until `weekendEnd` (no `sleepMaxMin` cap). Otherwise: sleep until next window start, capped at `sleepMaxMin`. Either way the result is floored at 1 min — a wake landing exactly on a window boundary would otherwise compute a 0 s sleep and wake straight back up. With no valid time this branch instead sleeps exactly `sleepFallbackS` **seconds** (not rounded down to whole minutes).
7. **Bring WiFi up if the cold-start branch in step 4 didn't** — see "WiFi must come up on every active path" below. Then run `ntp_sync()` once, which restarts SNTP and corrects RTC drift in the background.
8. **If `sleepEnabled = false` and not woken by button:** set `run_forever = true` — the active loop runs indefinitely, in and outside a time window alike. The OLED countdown bar stays full. When sleep is re-enabled via the web panel, a fresh `buttonActiveMin`-timer starts from the save moment. A button wake always keeps its timer, so the button still ends in deep sleep.
9. Otherwise run the active loop until `active_end` (end of time window, or button active duration).
10. Button pressed during active loop → `force_sleep = true`, exits immediately.
11. After the loop, `go_to_sleep(sleepAfterS)` (default 300 s = 5 min).

#### WiFi must come up on every active path

`app_main` reaches the active loop from three directions: cold boot (no valid time), a timer/button wake from deep sleep, and `esp_restart()` (web panel "Neustart", or the AP-mode config save). The third one is the trap: after `esp_restart()` the wake cause is `ESP_SLEEP_WAKEUP_UNDEFINED` (== 0, same as a cold boot) **but the RTC time is still valid**, so a guard like `if (wakeup != 0)` skips WiFi init entirely and the device runs netless until the next sleep cycle. WiFi is therefore gated on a local `wifi_started` flag, not on the wake cause. Keep it that way.

#### Button handling

The button (GPIO 0 by default) is both the deep-sleep wake source (`ESP_EXT1_WAKEUP_ANY_LOW`) and the "go to sleep now" key during the active loop. That double role needs two safeguards:

- **Wait for release.** The long-press measurement loop gives up after `buttonLongPressMs + 1 s`. Without `wait_button_release()` afterwards, a hold longer than that leaves the button still down when the active loop starts, which reads as an immediate sleep request — the device would wake and go straight back to sleep. The same applies before entering deep sleep: a held button would re-trigger the wake instantly.
- **Debounce.** `button_pressed()` re-samples after 30 ms. A single glitch on GPIO 0 must not end the active session.

### Active-loop responsibilities

`while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end))` — one iteration does:
1. Reload `cfg` from NVS if `g_cfg_dirty` is set (after web panel save). Re-evaluates `run_forever`. If sleep was just re-enabled (`was_forever && !run_forever`), resets `active_end` to now + `buttonActiveMin`. Re-arms the invert timer, and un-inverts the display if `oledInvertMin` was just switched to 0.
2. Retry-fetch departures (`cfg.apiRetryCount` attempts, `cfg.apiRetryDelayS` apart).
3. If success → update in-RAM cache (`last_deps`, `cached_time`). If failure → show cached data with `!` prefix if `< cfg.staleMaxMin` minutes old.
4. LED: **worst status across all 4 valid, non-cancelled departures** (Ausfall > big delay > small delay > OK).
5. Render via `display_departures()` which in turn calls `draw_header()` (station name + clock).
6. Draw countdown bar — full (100 %) when `run_forever`, counting down otherwise.
7. Compute minutes to next non-cancelled future train → tiered `refresh_sec`.
8. Inner wait loop handles: LED blink in error state, OLED invert for burn-in protection, re-render every 30 s, debounced button-press → `force_sleep`, `g_cfg_dirty` → break to outer loop immediately.

The 30 s re-render exists to keep the header clock live. It must repeat **whatever screen the outer loop chose** — departures when `success || show_stale`, the error screen otherwise. Rendering `last_deps` unconditionally puts expired cache data on screen without the `!` marker, i.e. stale departures that look current.

#### Duration arithmetic

Convert durations with `minutes_to_ticks()` / `seconds_to_ticks()`, never `pdMS_TO_TICKS()`. The latter computes `ms * configTICK_RATE_HZ` in 32 bits and wraps above ~11.9 h (715 min):

```
  715 min -> 4290000 ticks (715.0 min)   ok
  716 min ->    1032 ticks (  0.2 min)   wrapped
 1439 min ->   44065 ticks (  7.3 min)   wrapped
```

That silently turned a 12 h time window into a 12-second one. Affected inputs are `active_rem_min` (up to 1439 for a near-24 h window), `buttonActiveMin` / `buttonLongActiveMin`, and the `oledInvertMin` fallback of 1440. `pdMS_TO_TICKS()` is still fine for the short fixed intervals (100 ms poll, 1 s bar, 30 s clock).

### Stack and memory gotchas

- The main task's default stack is tight. Both `SbbDeparture[4]` arrays (cache + current) are `static` inside `app_main` — keep them static, and be cautious when adding large locals.
- `http_buf` in `sbb.c` is a 32 KB heap buffer, allocated lazily on first call and reused. `limit=15` plus `passList` for a station with many intermediate stops gets close to that ceiling, so don't raise `limit` without raising the buffer. Overflow is detected (`http_truncated`) and reported instead of surfacing as a bogus JSON parse error.
- `handler_config_post()` parks its 4 KB request buffer in `static` for the same reason — it does not fit the 8 KB httpd task stack. The server handles requests sequentially, so there's no race.
- The URL buffer is `char url[512]` — large enough for the full URL with the optional `passList` field. Don't shrink it.
- NVS keys must be ≤ 15 characters (ESP-IDF limit).
- `main.c`, `sbb.c` and `http_server.c` include `<time.h>` / `<stdlib.h>` explicitly. They used to come in transitively through ESP-IDF headers, which broke as soon as those headers changed.

### `sbb_get_departures()` contract

Signature: `bool sbb_get_departures(const char *station, SbbDeparture results[4], const char *dest_filters[], int filter_count)`.

- **Count-based, not NULL-terminated.** `filter_count = 0` disables filtering entirely.
- Station name is URL-encoded inside (space → `%20`), so callers pass the canonical name as it appears on sbb.ch (e.g. `"Basel SBB"`).
- Filter matches both the end destination (`to`) and intermediate stops (`passList/station/name`), case-insensitive substring. The `passList` field is requested from the API **only when `filter_count > 0`** to save bandwidth. Empty filter strings are skipped — `str_contains_ci()` treats an empty needle as a match, so one blank entry would silently disable the whole filter.
- Results are chosen to start at the first departure ≥ current HH:MM; if fewer than 4 future trains are available, the window backs up and some `results[i]` may have `valid = false`.
- Returns `false` (and logs the reason) on: WiFi down, transport error, **non-2xx HTTP status**, truncated response, unparseable JSON, missing/non-array `stationboard`, or no future train. The HTTP status check matters because `esp_http_client_perform()` returns `ESP_OK` for a 404 too — a misspelled station name would otherwise show up as "JSON Parse Fehler". Read the status code *before* `esp_http_client_cleanup()`.

### Web panel

`GET /api/status` is the panel's single source of truth for device state: `wifi`, `ntp`, `time` (HH:MM:SS), `weekday`, `inWindow`, `runForever`, `activeUntilS` (−1 when unlimited), `ip`, `rssi`, `heapKb`, `uptimeS`, `lastError`. Poll interval is 5 s.

**Never derive device state from the browser clock.** The panel used to render "Systemzeit" and "Im aktiven Zeitfenster" from `new Date()`, which is simply wrong in another timezone or when the RTC has drifted — under a heading that promised real device data. The clock now interpolates locally between polls (`_devSec` + elapsed) and resyncs on every response. The one legitimate local computation is the sleep preview, because it must reflect *unsaved* form values — but it too uses the device clock when one is available.

**`lastError`** comes from `sbb_last_error()`, set at every failure return in `sbb_get_departures()` and cleared on success. It turns the display's generic "API FEHLER" into a diagnosis in the panel (wrong station name, no WiFi, truncated response). The OLED text itself is deliberately unchanged.

**Saving is locked until `GET /api/config` has succeeded once** (`_cfgLoaded`). Without that guard, a failed load leaves the form holding its HTML defaults and one click on Speichern writes those back to the device — five time windows collapse to one, station resets, filters vanish. When the load fails, `checkEsp()` retries it on the next poll and unlocks the button.

The panel keeps `destFilters` positions: it sends all four slots and only trims trailing empties, so a gap at slot 1 survives a save round-trip. This relies on the firmware skipping empty filter strings.

There is a Playwright smoke test for these behaviours; it runs against a mock server that serves `index.html` plus the three API endpoints — no hardware needed. Worth re-running after panel changes.

### Font and UTF-8

`main.c` ships a 5×7 uppercase-only font plus six custom umlaut glyphs (`ÄÖÜäöü`). Lowercase input is mapped to uppercase before lookup. Other Latin-1 accented chars (é, è, â, ô, ç, …) fall back to their base letter via range checks on the second UTF-8 byte (`0xC3 0xXX`), so `Delémont` renders as `DELEMONT`.

Bytes in the UTF-8 continuation range (`0x80`–`0xBF`) that aren't consumed by one of those cases are skipped without advancing x. Otherwise a single unsupported character (a 3-byte sequence, say) would eat one 6 px cell per byte and push the rest of the line off screen.
