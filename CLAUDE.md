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
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor               # Ctrl-] to exit monitor
```

**No `secrets.h` is needed** — a fresh clone builds as-is. `main.c` pulls it in via `__has_include` and falls back to empty credentials, which sends the device straight to AP mode (`SBB-Monitor` / `192.168.4.1`) where WiFi is entered in the web panel and stored in NVS. That include used to be unconditional, so a clone would not compile until you hand-created a gitignored file for a fallback nobody uses in normal operation. Copy `main/secrets.h.example` to `main/secrets.h` only if you want credentials compiled in; NVS always wins over it.

On first flash or after partition table changes: `idf.py fullclean` before build.

There is no linter. Two test suites run without hardware and should both be green before claiming a change works:

```
./test/native/run.sh                  # Firmware: Build, Link und Regressionen
node test/panel/panel.test.js         # Web-Panel (Playwright)
node test/panel/roundtrip.test.js
```

`test/native/run.sh` needs only `gcc` and `python3`. It syntax-checks every `main/*.c` against the stub headers in `test/native/idfstub/` with `-Wall -Wextra`, then **links the whole firmware natively** against generated stub implementations — that second step is what catches missing and duplicate symbols when code moves between translation units, which a per-file syntax check cannot see. It also runs configuration, departure selection, Origin validation, retry interruption and cache-key regression tests. The portable entry point is `python test/native/run.py`; `run.sh` delegates to it. Windows can use `--cc path/to/zig.exe cc`. Details and limits in `test/native/README.md`.

Neither suite substitutes for a real `idf.py build` or a hardware test; say so explicitly when that's all that was run. If you use a new IDF function, add it to `test/native/idfstub/` — `gen_stubs.py` derives the empty implementation from the header itself.

## Git workflow

Develop on the active feature branch, not `main`. User-facing configuration values (station, filters, time windows, etc.) are managed via NVS and the web panel — do not hardcode them in source. Defaults live in `config_fields.def`; changing one there changes what a factory-reset device starts with, not what an existing device already has stored.

## Architecture

All application code lives in `main/`. Source files:

- **`main/main.c`** — wake/sleep flow, active-window main loop, WiFi/NTP bring-up, time-window logic.
- **`main/display.c` / `display.h`** — SSD1306 driver, 5×7 font with UTF-8 mapping, the finished screens (`display_departures()`, `display_error()`, `display_message()`, `display_sleep_info()`) and the countdown bar.
- **`main/led.c` / `led.h`** — WS2812 wrapper, worst-of-4 status colour, brightness scaling.
- **`main/button.c` / `button.h`** — debounce, release-wait, hold measurement.
- **`main/sbb.c` / `sbb.h`** — WiFi, HTTP, JSON parsing, filter logic. Public API: `sbb_wifi_init()` and `sbb_get_departures()`.
- **`main/config_fields.def`** — the table of all scalar config fields (default, range, NVS key, JSON key). See "Configuration" below.
- **`main/nvs_config.c` / `nvs_config.h`** — configuration in NVS. `blink_config_t` is the central struct; defaults, ranges, load and save are all loops over `config_fields.def`.
- **`main/http_server.c` / `http_server.h`** — web panel (SPIFFS + `/api/config` GET/POST + `/api/status` GET + `/api/departures` GET + `/api/restart` POST). Sets `g_cfg_dirty = true` after successful save so the main loop reloads cfg. Optional HTTP Basic Auth via `panelPass` (empty = no auth, the default). Also owns the runtime state the panel mirrors — see "Runtime state" below. Request bodies are read in a loop up to `req->content_len` — a single `httpd_req_recv()` only returns what is currently in the socket, so a config body spanning more than one TCP segment used to arrive truncated and fail as "Invalid JSON".
- **`main/spiffs/index.html`** — web panel UI, flashed to SPIFFS. See "Web panel" below.
- **`main/cJSON.c` / `cJSON.h`** — vendored JSON library, do not modify.

### Configuration

All tunables live in `blink_config_t` (`nvs_config.h`). They are:
- Loaded from NVS at startup via `nvs_config_load()`.
- Editable at runtime via `http://sbb-monitor.local` while the device is active.
- Persisted to NVS on save; survive deep sleep and reboots.
- `secrets.h` (`WIFI_SSID` / `WIFI_PASS`) is an *optional* compile-time fallback if NVS has no credentials. Absent (the normal case) the fallback is `""`, and `sbb_wifi_init()` short-circuits to AP mode instead of burning the 15 s connect timeout first.

**Adding a new tunable is two edits**: the field in `blink_config_t`, and one line in `config_fields.def`. Defaults, range clamping, NVS load/save and both HTTP handlers are loops over that table. The panel still needs its own three lines (input element, `saveConfig()`, `loadConfig()`).

The table macros are `CFG_INT`, `CFG_GPIO`, `CFG_BOOL`, `CFG_STR` and `CFG_RGB`; each consumer defines only the ones it cares about and includes the file. **NVS and JSON keys must never change** — a renamed NVS key silently stops reading the user's stored setting and falls back to the default, a renamed JSON key breaks the panel. `test/native/config_table.test.c` enforces keys ≤ 15 chars (the ESP-IDF limit), key uniqueness, and defaults inside their own declared range.

Only fields with real special handling stay out of the table: the two arrays (`timeWindows`, `destFilters`), the two passwords (never sent by `GET /api/config`; an empty field on POST means "unchanged"), the cross-field rule `delayBigMin > delaySmallMin`, and the format check on `oledAddr`.

`nvs_config_sanitize()` clamps every field to a plausible range. It runs at the end of `nvs_config_load()` and again in `handler_config_post()` before the save, so neither a stale NVS entry nor a hand-crafted `POST /api/config` can store a value that bricks the device (e.g. `apiRetryCount = 0` → the retry loop never runs, `refresh*Sec = 0` → the API is hammered without pause).

### Wake-up and sleep flow (`app_main` in `main.c`)

1. Check wake cause. `ESP_SLEEP_WAKEUP_EXT1` = button on GPIO 0.
2. `http_status_init()`, then init LED, display and button, set TZ (`CET-1CEST,M3.5.0,M10.5.0/3`).
3. On a button wake: measure the hold to pick short/long active duration, then **wait for release** before continuing (see "Button handling").
4. `time()` + `localtime_r()` — the ESP32 RTC keeps time across deep sleep, so a wake with `tm_year >= 100` skips the cold-start WiFi+NTP screen.
5. Compute `in_window = time_valid && !weekend_skip && inside_active_time`.
6. **If not in window and not woken by button and `sleepEnabled = true` → sleep.** If `weekendSleepEnabled` and inside the weekend window: sleep directly until `weekendEnd` (no `sleepMaxMin` cap). Otherwise: sleep until the next window start that will actually become active, capped at `sleepMaxMin` — `minutes_to_next_window()` skips Sat/Sun when `weekdaysOnly` is set, because on a Saturday the naive computation returned "30 min until 06:45" for a window that never opens that day. Either way the result is floored at 1 min — a wake landing exactly on a window boundary would otherwise compute a 0 s sleep and wake straight back up. With no valid time this branch instead sleeps exactly `sleepFallbackS` **seconds** (not rounded down to whole minutes).
7. **Bring WiFi up if the cold-start branch in step 4 didn't** — see "WiFi must come up on every active path" below. Then run `ntp_sync()` once, which restarts SNTP and corrects RTC drift in the background.
8. **If `sleepEnabled = false` and not woken by button:** set `run_forever = true` — the active loop runs indefinitely, in and outside a time window alike. The OLED countdown bar stays full. When sleep is re-enabled via the web panel, a fresh `buttonActiveMin`-timer starts from the save moment. A button wake always keeps its timer, so the button still ends in deep sleep.
9. Otherwise run the active loop until `active_end` (end of time window, or button active duration).
10. Button pressed during active loop → `force_sleep = true`, exits immediately.
11. After the loop, `go_to_sleep(sleepAfterS)` (default 300 s = 5 min).

#### WiFi must come up on every active path

`app_main` reaches the active loop from three directions: cold boot (no valid time), a timer/button wake from deep sleep, and `esp_restart()` (web panel "Neustart", or the AP-mode config save). The third one is the trap: after `esp_restart()` the wake cause is `ESP_SLEEP_WAKEUP_UNDEFINED` (== 0, same as a cold boot) **but the RTC time is still valid**, so a guard like `if (wakeup != 0)` skips WiFi init entirely and the device runs netless until the next sleep cycle. WiFi is therefore gated on a local `wifi_started` flag, not on the wake cause. Keep it that way.

#### Button handling

The button (GPIO 0 by default) is both the deep-sleep wake source (`ESP_EXT1_WAKEUP_ANY_LOW`) and the "go to sleep now" key during the active loop. That double role needs two safeguards:

- **Wait for release.** The long-press measurement loop gives up after `buttonLongPressMs + 1 s`. Without `button_wait_release()` afterwards, a hold longer than that leaves the button still down when the active loop starts, which reads as an immediate sleep request — the device would wake and go straight back to sleep. The same applies before entering deep sleep: a held button would re-trigger the wake instantly.
- **Debounce.** `button_pressed()` re-samples after 30 ms. A single glitch on GPIO 0 must not end the active session.

### Active-loop responsibilities

`while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end))` — one iteration does:
1. Reload `cfg` from NVS if `g_cfg_dirty` is set (after web panel save). Re-evaluates `run_forever`. If sleep was just re-enabled (`was_forever && !run_forever`), resets `active_end` to now + `buttonActiveMin`. Re-arms the invert timer, and un-inverts the display if `oledInvertMin` was just switched to 0.
2. Retry-fetch departures (`cfg.apiRetryCount` attempts, `cfg.apiRetryDelayS` apart). Retry waits poll the button/config/deadline every 100 ms; reconnect waits check every second. An in-flight HTTP request retains its 10-second timeout.
3. Station/filter changes invalidate both local and HTTP caches; unrelated settings retain them. If success → update in-RAM cache (`last_deps`, `cached_time`). If failure → show cached data with `!` prefix if `< cfg.staleMaxMin` minutes old.
4. LED: **worst status across all valid, non-cancelled departures** (Ausfall > big delay > small delay > OK), scaled by `ledBrightness`.
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

### Runtime state

The state `GET /api/status` and `GET /api/departures` report lives as `static` in `http_server.c` behind a mutex, not as globals in `main.c`. Writers use `http_status_set_departures()` and `http_status_set_active()`; `http_status_init()` must run before the first setter (it does, at the top of `app_main`).

The lock is not decoration: the departures cache is several hundred bytes, so an unlocked `memcpy` from the main task can land in the middle of the httpd task's JSON serialisation and show one train's time next to the next train's destination. `time_t` is 64-bit on ESP-IDF and not atomically readable on a 32-bit core either. Readers copy under the lock and serialise afterwards; the main task holds it only for a `memcpy`.

`publish_status(run_forever, active_end)` in `main.c` is the single place that pushes loop state out. It recomputes the window position every iteration and on every 30 s clock tick — `inWindow` used to be set once at startup and never again, so with `sleepEnabled = false` the panel claimed "in the active window" until the next reboot.

### API security

The panel is served same-origin and the API carries **no** `Access-Control-Allow-Origin` header. Don't add one back: with it, any website the user visits on the same LAN can read `GET /api/config` and harvest the SSID, station and GPIO layout.

`panelPass` is empty by default, so writing endpoints are guarded by `require_write_access()` instead:

- `POST /api/config` must carry `Content-Type: application/json`. That makes the request non-simple, so a browser sends a preflight first — and there is no `OPTIONS` handler to answer it. Without this check, `Content-Type: text/plain` (CORS-safelisted, no preflight) sails straight through and the firmware parses the body as JSON anyway.
- A present `Origin` header must match `Host`. A missing `Origin` is allowed through so curl and local tooling keep working; no browser can be tricked into omitting it.

`POST /api/restart` gets the origin check too — otherwise a plain `<form>` submit from a foreign page reboots the device.

### Stack and memory gotchas

- The main task's default stack is tight. Both `SbbDeparture[DEP_COUNT]` arrays (cache + current) are `static` inside `app_main` — keep them static, and be cautious when adding large locals.
- `http_buf` in `sbb.c` is a 32 KB heap buffer, allocated lazily on first call and reused. `limit=15` plus `passList` for a station with many intermediate stops gets close to that ceiling, so don't raise `limit` without raising the buffer. Overflow is detected (`http_truncated`) and reported instead of surfacing as a bogus JSON parse error.
- `handler_config_post()` parks its 4 KB request buffer in `static` for the same reason — it does not fit the 8 KB httpd task stack. The server handles requests sequentially, so there's no race.
- The URL buffer is `char url[512]` — large enough for the full URL with the optional `passList` field. Don't shrink it.
- NVS keys must be ≤ 15 characters (ESP-IDF limit).
- `main.c`, `sbb.c`, `display.c` and `http_server.c` include `<time.h>` / `<stdlib.h>` explicitly. They used to come in transitively through ESP-IDF headers, which broke as soon as those headers changed.
- `DEP_COUNT` (`sbb.h`) and `MAX_TIME_WINDOWS` / `MAX_DEST_FILTERS` (`nvs_config.h`) are the array sizes — don't reintroduce the bare `4` and `8`.

### `sbb_get_departures()` contract

Signature: `bool sbb_get_departures(const char *station, SbbDeparture results[DEP_COUNT], const char *dest_filters[], int filter_count)`.

- **Count-based, not NULL-terminated.** `filter_count = 0` disables filtering entirely.
- Station name is URL-encoded inside (space → `%20`), so callers pass the canonical name as it appears on sbb.ch (e.g. `"Basel SBB"`).
- Filter matches both the end destination (`to`) and intermediate stops (`passList/station/name`), case-insensitive substring. The `passList` field is requested from the API **only when `filter_count > 0`** to save bandwidth. Empty filter strings are skipped — `str_contains_ci()` treats an empty needle as a match, so one blank entry would silently disable the whole filter.
- Results use full ISO departure timestamps (date and UTC offset) plus delay, sorted by expected departure. Only trains in the current minute or later are returned; unused slots stay invalid. `expectedDeparture` is also used for adaptive refresh, without a 12-hour heuristic.
- Returns `false` (and logs the reason) on: WiFi down, transport error, **non-2xx HTTP status**, truncated response, unparseable JSON, missing/non-array `stationboard`, or no future train. The HTTP status check matters because `esp_http_client_perform()` returns `ESP_OK` for a 404 too — a misspelled station name would otherwise show up as "JSON Parse Fehler". Read the status code *before* `esp_http_client_cleanup()`.

### Web panel

`GET /api/status` is the panel's single source of truth for device state: `wifi`, `apMode`, `ntp`, `time` (HH:MM:SS), `weekday`, `inWindow`, `runForever`, `activeUntilS` (−1 when unlimited), `ip`, `rssi`, `heapKb`, `uptimeS`, `lastError`. Poll interval is 5 s.

**Never derive device state from the browser clock.** The panel used to render "Systemzeit" and "Im aktiven Zeitfenster" from `new Date()`, which is simply wrong in another timezone or when the RTC has drifted — under a heading that promised real device data. The clock now interpolates locally between polls (`_devSec` + elapsed) and resyncs on every response. The one legitimate local computation is the sleep preview, because it must reflect *unsaved* form values — but it too uses the device clock when one is available.

**`lastError`** comes from `sbb_last_error()`, set at every failure return in `sbb_get_departures()` and cleared on success. It turns the display's generic "API FEHLER" into a diagnosis in the panel (wrong station name, no WiFi, truncated response). The OLED text itself is deliberately unchanged.

**Saving is locked until `GET /api/config` has succeeded once** (`_cfgLoaded`). Without that guard, a failed load leaves the form holding its HTML defaults and one click on Speichern writes those back to the device — five time windows collapse to one, station resets, filters vanish. When the load fails, `checkEsp()` retries it on the next poll and unlocks the button.

The panel keeps `destFilters` positions: it sends all four slots and only trims trailing empties, so a gap at slot 1 survives a save round-trip. This relies on the firmware skipping empty filter strings.

**After a successful save the panel re-reads `/api/config`**, diffs it against what it sent, and names any field the device stored differently (`verifySaved()`). The firmware clamps values and rejects invalid GPIOs; without the read-back the form keeps showing the typed value and the setting looks applied when it isn't. `loadConfig()` therefore has to write *all four* filter slots, including empty ones — otherwise a deleted filter stays visible in the form after the reload.

Text inputs carry `maxlength` matching the firmware buffers (`station`/`ssid`/`password` 63, `panelPass`/`destFilters` 31, `oledAddr` 7). Longer input would be silently truncated by `strncpy()` on the device.

Two Playwright tests under `test/panel/` cover this, against a mock server serving `index.html` plus the API endpoints — no hardware needed. `roundtrip.test.js` in particular loads a maximal config, saves it untouched, and asserts all 45 fields survive; it also documents that a full config POST reaches ~1500 bytes, i.e. more than one TCP segment. The mock enforces the same `Content-Type`/`Origin` rules as `require_write_access()`, so the test goes red if the panel ever stops sending them. Re-run both after panel changes.

`GET /api/status` distinguishes three network states, not two: `wifi` (STA connected) and `apMode` (AP fallback). It used to report `wifi = !apMode`, which meant a connection that dropped mid-session still showed green "Verbunden" while `lastError` right below it said "Kein WLAN".

### Display line width

128 px / 6 px per cell = **21 cells per line**. Don't budget them with fixed `%.Ns` bounds: `snprintf` counts bytes, so an umlaut destination came out short, and three of the five departure-line formats could exceed 21 cells and clip the last platform digit. `display_departures()` builds the ASCII prefix (`HH:MM` / `HH:MM+7`) and suffix (` G12`) first, then gives the destination whatever is left via `copy_glyphs()`, which counts UTF-8 sequences as one cell each — matching what `draw_char_utf8()` actually renders.

### Font and UTF-8

`display.c` ships a 5×7 uppercase-only font plus six custom umlaut glyphs (`ÄÖÜäöü`). Lowercase input is mapped to uppercase before lookup. Other Latin-1 accented chars (é, è, â, ô, ç, …) fall back to their base letter via range checks on the second UTF-8 byte (`0xC3 0xXX`), so `Delémont` renders as `DELEMONT`.

Bytes in the UTF-8 continuation range (`0x80`–`0xBF`) that aren't consumed by one of those cases are skipped without advancing x. Otherwise a single unsupported character (a 3-byte sequence, say) would eat one 6 px cell per byte and push the rest of the line off screen.
