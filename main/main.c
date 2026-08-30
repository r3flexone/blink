#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "sbb.h"
#include "display.h"
#include "led.h"
#include "button.h"
// secrets.h ist optional. Eingerichtet wird das Geraet ueber das Web-Panel:
// ohne brauchbare Zugangsdaten geht es in den AP-Modus (SSID "SBB-Monitor",
// 192.168.4.1), dort traegt man WLAN ein, und es landet in NVS. Der Include
// war frueher hart — ein frischer Clone liess sich damit gar nicht bauen,
// bevor man die gitignorete Datei von Hand angelegt hatte, fuer einen
// Fallback, den im Normalbetrieb niemand benutzt.
// Wer die Zugangsdaten trotzdem einkompilieren will, legt main/secrets.h an
// (Vorlage: main/secrets.h.example).
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "nvs_config.h"
#include "http_server.h"
#include "nvs_flash.h"
#include "mdns.h"

static const char *TAG = "main";

// Konfiguration — geladen aus NVS in app_main, überall verfügbar
static blink_config_t cfg;

// Gesetzt vom HTTP-Server nach erfolgreichem POST /api/config
volatile bool g_cfg_dirty = false;

// pdMS_TO_TICKS() rechnet intern in 32 Bit (ms * configTICK_RATE_HZ) und
// läuft ab ca. 11.9 h über — ein Zeitfenster von 23 h oder ein OLED-Invert-
// Intervall von 1440 min ergäbe sonst eine viel zu kurze Dauer. Deshalb
// Minuten/Sekunden direkt in Ticks umrechnen.
#define MAX_DURATION_MIN (7 * 24 * 60)
static TickType_t minutes_to_ticks(uint32_t minutes) {
    if (minutes > MAX_DURATION_MIN) minutes = MAX_DURATION_MIN;
    return (TickType_t)minutes * 60 * configTICK_RATE_HZ;
}
static TickType_t seconds_to_ticks(uint32_t seconds) {
    if (seconds > MAX_DURATION_MIN * 60) seconds = MAX_DURATION_MIN * 60;
    return (TickType_t)seconds * configTICK_RATE_HZ;
}

// ===== SLEEP =====
static void go_to_sleep(uint64_t us) {
    display_clear();
    display_off();
    led_off();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_timer_wakeup(us);
    // Interner Pullup gilt im Deep Sleep nur über die RTC-Domain — ohne
    // externen Pullup würde der Button-Pin sonst floaten (Geister-Wakeups).
    if (rtc_gpio_is_valid_gpio(cfg.buttonGpio)) {
        rtc_gpio_pullup_en(cfg.buttonGpio);
        rtc_gpio_pulldown_dis(cfg.buttonGpio);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup(1ULL << cfg.buttonGpio, ESP_EXT1_WAKEUP_ANY_LOW);
    } else {
        ESP_LOGW(TAG, "GPIO %d ist kein RTC-GPIO — kein Button-Wakeup", cfg.buttonGpio);
    }
    esp_deep_sleep_start();
}

// ===== WIFI =====
// NVS-Credentials, mit dem optionalen secrets.h als Compile-Time-Fallback
static void wifi_connect_from_cfg(void) {
    const char *ssid = cfg.ssid[0]     ? cfg.ssid     : WIFI_SSID;
    const char *pass = cfg.password[0] ? cfg.password : WIFI_PASS;
    sbb_wifi_init(ssid, pass);
}

// Reconnect anstossen und in 1-s-Scheiben darauf warten, dazwischen den Taster
// pollen. Ein einzelner 15-s-Block in sbb_wifi_reconnect() liess das Geraet bei
// WLAN-Problemen so lange nicht auf "Schlafen" reagieren.
#define WIFI_RECONNECT_TIMEOUT_S 15
static void wifi_reconnect_interruptible(bool *force_sleep) {
    if (sbb_wifi_is_connected()) return;
    sbb_wifi_reconnect_start();
    for (int i = 0; i < WIFI_RECONNECT_TIMEOUT_S; i++) {
        if (sbb_wifi_wait_connected(1000)) return;
        if (button_pressed()) {
            ESP_LOGI(TAG, "Button während Reconnect → Schlaf");
            button_wait_release(5000);
            *force_sleep = true;
            return;
        }
    }
}

// ===== NTP =====
static bool ntp_sync(void) {
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    if (!esp_sntp_enabled()) esp_sntp_init();
    else { esp_sntp_stop(); esp_sntp_init(); }

    time_t now; struct tm ti;
    int steps = (cfg.ntpTimeoutS * 1000) / 250;
    for (int i = 0; i < steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        time(&now); localtime_r(&now, &ti);
        if (ti.tm_year >= 100) {
            ESP_LOGI(TAG, "NTP OK (%02d:%02d:%02d)", ti.tm_hour, ti.tm_min, ti.tm_sec);
            return true;
        }
    }
    ESP_LOGE(TAG, "NTP FAIL nach %d s", cfg.ntpTimeoutS);
    return false;
}

// ===== BOARD INFO =====
static void log_board_info(void) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%d.%d, %d Core(s)",
             ci.revision / 100, ci.revision % 100, ci.cores);
    ESP_LOGI(TAG, "Heap frei: %lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
}

// ===== ZEITFENSTER =====
// Liegt cur_min in einem konfigurierten Fenster? Setzt *rem_min (falls != NULL)
// auf die Minuten bis zum Fensterende. Ende <= Start heisst "ueber Mitternacht".
static bool find_active_window(const blink_config_t *c, int cur_min, int *rem_min) {
    for (int i = 0; i < c->timeWindowCount; i++) {
        int ws = c->timeWindows[i].startH * 60 + c->timeWindows[i].startM;
        int we = c->timeWindows[i].endH   * 60 + c->timeWindows[i].endM;
        bool inside = (we > ws) ? (cur_min >= ws && cur_min < we)
                                : (cur_min >= ws || cur_min < we);
        if (!inside) continue;
        if (rem_min) {
            int rem = we - cur_min;
            if (rem <= 0) rem += 24 * 60;   // Ende liegt am Folgetag
            *rem_min = rem;
        }
        return true;
    }
    return false;
}

// Minuten bis zum naechsten Fensterstart, der auch wirklich aktiv wird.
// Bei weekdaysOnly muessen Sa/So uebersprungen werden: sonst rechnete das
// Geraet am Samstag "noch 30 Min bis 06:45" aus und zeigte das auch an,
// obwohl dieses Fenster am Wochenende gar nicht greift.
static int minutes_to_next_window(const blink_config_t *c, const struct tm *ti) {
    int cur_min = ti->tm_hour * 60 + ti->tm_min;
    int best = 7 * 24 * 60;
    for (int day = 0; day <= 7; day++) {
        int wday = (ti->tm_wday + day) % 7;
        if (c->weekdaysOnly && (wday == 0 || wday == 6)) continue;
        for (int i = 0; i < c->timeWindowCount; i++) {
            int ws = c->timeWindows[i].startH * 60 + c->timeWindows[i].startM;
            int diff = day * 24 * 60 + ws - cur_min;
            if (diff > 0 && diff < best) best = diff;
        }
    }
    return best;
}

// Laufzeitstatus fuer GET /api/status nachfuehren. Muss regelmaessig laufen:
// bei run_forever laeuft das Geraet ueber das Fensterende hinaus weiter, und
// ein Config-Reload kann das gerade aktive Fenster entfernt haben. Frueher
// wurde die Fensterlage genau einmal beim Start gesetzt und nie wieder — das
// Panel meldete dann bis zum Neustart "Im aktiven Zeitfenster".
//
// active_end kommt in Ticks und wird hier in Wanduhrzeit umgerechnet: mit
// Ticks kann das Panel nichts anfangen.
static void publish_status(bool run_forever, TickType_t active_end) {
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);

    bool in_window = false;
    if (ti.tm_year >= 100) {
        bool weekend_skip = cfg.weekdaysOnly && (ti.tm_wday == 0 || ti.tm_wday == 6);
        in_window = !weekend_skip &&
                    find_active_window(&cfg, ti.tm_hour * 60 + ti.tm_min, NULL);
    }

    time_t end_wall = 0;
    if (!run_forever) {
        TickType_t now_ticks = xTaskGetTickCount();
        uint32_t remain_s = (active_end > now_ticks)
            ? (uint32_t)((active_end - now_ticks) / configTICK_RATE_HZ) : 0;
        end_wall = now + (time_t)remain_s;
    }

    http_status_set_active(in_window, run_forever, end_wall);
}

// ===== ZEITFENSTER-VALIDIERUNG =====
static void check_window_overlaps(void) {
    for (int i = 0; i < cfg.timeWindowCount; i++) {
        int s1 = cfg.timeWindows[i].startH * 60 + cfg.timeWindows[i].startM;
        int e1 = cfg.timeWindows[i].endH * 60 + cfg.timeWindows[i].endM;
        // e1 < s1 ist ein gewolltes Fenster über Mitternacht; nur e1 == s1 ist leer.
        if (e1 == s1) {
            ESP_LOGW(TAG, "Zeitfenster %d: Laenge 0!", i + 1);
        }
        bool wrap1 = (e1 <= s1);
        for (int j = i + 1; j < cfg.timeWindowCount; j++) {
            int s2 = cfg.timeWindows[j].startH * 60 + cfg.timeWindows[j].startM;
            int e2 = cfg.timeWindows[j].endH * 60 + cfg.timeWindows[j].endM;
            bool wrap2 = (e2 <= s2);
            // Overlap-Pruefung gilt nur fuer zwei nicht-umschlagende Fenster.
            if (!wrap1 && !wrap2 && s1 < e2 && s2 < e1) {
                ESP_LOGW(TAG, "Zeitfenster %d und %d ueberlappen!", i + 1, j + 1);
            }
        }
    }
}

// ===== MAIN =====
static bool in_weekend_window(const struct tm *ti, const blink_config_t *c) {
    int cur   = ti->tm_wday * 24 * 60 + ti->tm_hour * 60 + ti->tm_min;
    int start = c->weekendStartDay * 24 * 60 + c->weekendStartH * 60 + c->weekendStartM;
    int end   = c->weekendEndDay   * 24 * 60 + c->weekendEndH   * 60 + c->weekendEndM;
    if (start <= end) return (cur >= start && cur < end);
    return (cur >= start || cur < end);
}

void app_main(void) {
    // NVS + Config ganz oben (vor Hardware-Init, da GPIO-Pins aus Config kommen)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_config_load(&cfg);
    http_status_init();   // Mutex fuer den Laufzeitstatus, vor dem ersten Setter

    // Singular-API: auf allen ESP-IDF v5.x verfügbar. ESP_SLEEP_WAKEUP_UNDEFINED == 0
    // (Kaltstart), daher bleiben die wakeup==0 / !=0 Prüfungen weiter unten gültig.
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    bool woken_by_button = (wakeup == ESP_SLEEP_WAKEUP_EXT1);

    led_init(&cfg);
    display_init(&cfg);
    button_init(&cfg);

    if (wakeup == 0) {
        log_board_info();
        check_window_overlaps();
    }

    int button_active_min = cfg.buttonActiveMin;
    if (woken_by_button) {
        int hold_ms = button_measure_hold(cfg.buttonLongPressMs + 1000);
        if (hold_ms >= cfg.buttonLongPressMs) {
            button_active_min = cfg.buttonLongActiveMin;
        }
        ESP_LOGI(TAG, "Button %d ms -> %d Min aktiv", hold_ms, button_active_min);
        // Die Messschleife bricht nach buttonLongPressMs + 1 s ab. Wird der
        // Taster länger gehalten, sähe die Aktiv-Schleife ihn sofort als
        // "Sleep"-Druck und das Gerät schliefe direkt wieder ein.
        button_wait_release(10000);
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool time_valid = (ti.tm_year >= 100);
    bool ap_mode = false;
    bool wifi_started = false;
    bool ntp_tried = false;

    if (!time_valid) {
        display_on();
        display_message("KALTSTART", "WIFI+NTP...", NULL, NULL);
        led_set_rgb(cfg.ledLoadingRgb);
        wifi_connect_from_cfg();
        wifi_started = true;
        ap_mode = sbb_wifi_is_ap_mode();
        if (!ap_mode) {
            time_valid = ntp_sync();
            ntp_tried = true;
            time(&now); localtime_r(&now, &ti);
        }
    }

    int cur_min = ti.tm_hour * 60 + ti.tm_min;
    bool is_weekend = (ti.tm_wday == 0 || ti.tm_wday == 6);
    bool weekend_skip = cfg.weekdaysOnly && is_weekend;

    int active_rem_min = 0;   // Minuten bis zum Fenster-Ende (wrap-fähig)
    bool in_window = time_valid && !weekend_skip &&
                     find_active_window(&cfg, cur_min, &active_rem_min);

    if (!in_window && !woken_by_button && cfg.sleepEnabled && !ap_mode) {
        uint64_t sleep_us;
        int d;
        if (time_valid) {
            d = minutes_to_next_window(&cfg, &ti);
            if (cfg.weekendSleepEnabled && in_weekend_window(&ti, &cfg)) {
                int end_abs = cfg.weekendEndDay * 24 * 60 + cfg.weekendEndH * 60 + cfg.weekendEndM;
                int cur_abs = ti.tm_wday * 24 * 60 + cur_min;
                int d_weekend = end_abs - cur_abs;
                if (d_weekend < 0) d_weekend += 7 * 24 * 60;
                d = d_weekend;
                ESP_LOGI(TAG, "Wochenend-Fenster: schlafe %d Min", d);
            } else {
                if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
                if (weekend_skip) ESP_LOGI(TAG, "Wochenende, schlafe %d Min", d);
                else              ESP_LOGI(TAG, "Schlafe %d Min", d);
            }
            // d == 0 (genau auf der Fenstergrenze) hiesse "sofort aufwachen"
            if (d < 1) d = 1;
            sleep_us = (uint64_t)d * 60ULL * 1000000ULL;
        } else {
            // Ohne gültige Zeit exakt sleepFallbackS schlafen — die frühere
            // Umrechnung auf volle Minuten machte aus 90 s stille 60 s.
            sleep_us = (uint64_t)cfg.sleepFallbackS * 1000000ULL;
            d = (cfg.sleepFallbackS + 59) / 60;
        }
        display_sleep_info(d);
        go_to_sleep(sleep_us);
        return;
    }

    if (woken_by_button) ESP_LOGI(TAG, "Button aktiv (%d Min)", button_active_min);
    else                 ESP_LOGI(TAG, "Zeitfenster aktiv");

    display_on();
    display_message(cfg.station, "LADE ZUEGE...", NULL, NULL);
    led_set_rgb(cfg.ledLoadingRgb);

    // WiFi in jedem Fall aufbauen, wenn es die Kaltstart-Phase nicht schon tat.
    // Früher hing das an "wakeup != 0": nach einem esp_restart (Panel-Neustart)
    // ist die Wake-Cause aber UNDEFINED, während die RTC-Zeit gültig bleibt —
    // das Gerät lief dann ohne Netz weiter und zeigte dauerhaft "API FEHLER".
    if (!wifi_started) {
        wifi_connect_from_cfg();
        wifi_started = true;
        ap_mode = sbb_wifi_is_ap_mode();
    }
    if (!ap_mode && !ntp_tried) {
        // Stösst SNTP erneut an und korrigiert so die RTC-Drift über lange
        // Schlafphasen hinweg (kehrt sofort zurück, wenn die Zeit schon passt).
        ntp_sync();
        ntp_tried = true;
    }

    // mDNS + HTTP-Server (WiFi/netif bereits durch sbb_wifi_init aktiv)
    if (!ap_mode) {
        mdns_init();
        mdns_hostname_set("sbb-monitor");
        mdns_instance_name_set("SBB Monitor");
    }
    http_server_start();

    if (ap_mode) {
        display_message("KEIN WLAN", "SSID: SBB-MONITOR", "192.168.4.1",
                        "WLAN EINRICHTEN");
        led_set_rgb(cfg.ledLoadingRgb);
        while (!g_cfg_dirty) {
            // Button im AP-Modus: sleepFallbackS schlafen statt fixer 30 s —
            // ein kurzer Zyklus würde sonst nur AP→Sleep→AP im Minutentakt kosten.
            if (button_pressed())
                go_to_sleep((uint64_t)cfg.sleepFallbackS * 1000000ULL);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // Kurze Pause damit die HTTP-Response noch ausgeliefert wird
        vTaskDelay(pdMS_TO_TICKS(500));
        // Neue WLAN-Credentials gespeichert → neu starten
        esp_restart();
    }

    TickType_t active_start = xTaskGetTickCount();
    TickType_t active_end;
    if (woken_by_button) {
        active_end = active_start + minutes_to_ticks((uint32_t)button_active_min);
    } else {
        int rem = active_rem_min;
        if (rem < 1) rem = 1;
        active_end = active_start + minutes_to_ticks((uint32_t)rem);
    }

    // Dest-Filter: Pointer-Array aus cfg.destFilters[][] bauen
    const char *filter_ptrs[MAX_DEST_FILTERS] = {0};
    for (int i = 0; i < cfg.destFilterCount && i < MAX_DEST_FILTERS; i++)
        filter_ptrs[i] = cfg.destFilters[i];

    // static: aus dem Stack raus (verhindert Stack-Overflow im Main-Task)
    static SbbDeparture deps[DEP_COUNT];
    static SbbDeparture last_deps[DEP_COUNT];
    memset(deps, 0, sizeof(deps));
    memset(last_deps, 0, sizeof(last_deps));
    bool has_cached = false;
    time_t cached_time = 0;

    bool inverted = false;
    bool force_sleep = false;
    // sleepEnabled=false: Schleife läuft unbegrenzt (bis Button-Sleep oder Sleep wird aktiviert).
    // Gilt auch im Zeitfenster — sonst wäre das Gerät bei "Schlaf aus" nach dem
    // Fensterende trotzdem für sleepAfterS eingeschlafen.
    bool run_forever = !cfg.sleepEnabled && !woken_by_button;
    publish_status(run_forever, active_end);
    TickType_t next_invert = xTaskGetTickCount() +
        minutes_to_ticks((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440));

    while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end)) {
        if (g_cfg_dirty) {
            bool was_forever = run_forever;
            esp_err_t load_err = nvs_config_load(&cfg);
            g_cfg_dirty = false;
            if (load_err != ESP_OK) {
                ESP_LOGE(TAG, "Config Reload fehlgeschlagen: %s", esp_err_to_name(load_err));
            } else {
                ESP_LOGI(TAG, "Config neu geladen (Web-Panel)");
            }
            run_forever = !cfg.sleepEnabled && !woken_by_button;
            if (was_forever && !run_forever) {
                // Sleep wurde aktiviert → frischen buttonActiveMin-Timer starten
                active_start = xTaskGetTickCount();
                active_end   = active_start + minutes_to_ticks((uint32_t)cfg.buttonActiveMin);
                ESP_LOGI(TAG, "Sleep aktiviert → Timer %d Min", cfg.buttonActiveMin);
            }
            // Invert-Intervall neu ansetzen; bei 0 (aus) sofort zurückschalten,
            // sonst bliebe das Display bis zum Schlafen invertiert.
            if (cfg.oledInvertMin <= 0 && inverted) {
                display_set_inverted(false);
                inverted = false;
            }
            next_invert = xTaskGetTickCount() +
                minutes_to_ticks((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440));
            for (int i = 0; i < MAX_DEST_FILTERS; i++)
                filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
            // Die Fenster koennen sich gerade geaendert haben — sonst stuende
            // die Warnung nur im Log des Kaltstarts, also genau dann nicht,
            // wenn der Nutzer sie gerade verstellt hat.
            check_window_overlaps();
        }
        publish_status(run_forever, active_end);
        wifi_reconnect_interruptible(&force_sleep);
        if (force_sleep) break;

        bool success = false;
        for (int attempt = 0; attempt < cfg.apiRetryCount && !success; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "API Retry %d/%d", attempt + 1, cfg.apiRetryCount);
                vTaskDelay(seconds_to_ticks((uint32_t)cfg.apiRetryDelayS));
            }
            success = sbb_get_departures(cfg.station, deps, filter_ptrs, cfg.destFilterCount);
        }

        bool show_stale = false;
        if (success) {
            memcpy(last_deps, deps, sizeof(deps));
            has_cached = true;
            time(&cached_time);
            http_status_set_departures(deps, cached_time);
        } else {
            time_t n; time(&n);
            if (has_cached && (n - cached_time) < cfg.staleMaxMin * 60) {
                show_stale = true;
            }
        }

        if (success) {
            led_show_worst_status(deps);
        } else {
            led_set_rgb(cfg.ledCancelledRgb);
        }

        // Display
        if (success || show_stale) {
            display_departures(success ? deps : last_deps, show_stale);
        } else {
            display_error();
        }
        display_countdown_bar(run_forever, active_start, active_end);

        // Adaptiver Refresh
        int refresh_sec;
        if (success) {
            int min_to_next = -1;
            time_t n; struct tm nt; time(&n); localtime_r(&n, &nt);
            int cur_m = nt.tm_hour * 60 + nt.tm_min;
            for (int i = 0; i < DEP_COUNT; i++) {
                if (!deps[i].valid || deps[i].cancelled) continue;
                int h, m;
                if (sscanf(deps[i].time, "%d:%d", &h, &m) == 2) {
                    // Wrap-fähig: Zug nach Mitternacht zählt als zukünftig (≤ 12 h)
                    int diff = ((h * 60 + m + deps[i].delay) - cur_m + 24 * 60) % (24 * 60);
                    if (diff <= 12 * 60 && (min_to_next < 0 || diff < min_to_next))
                        min_to_next = diff;
                }
            }
            if (min_to_next < 0) {
                ESP_LOGW(TAG, "Kein Zug in Zukunft! cur_m=%d", cur_m);
                refresh_sec = cfg.refreshFarSec;
            } else if (min_to_next <= cfg.refreshNearMin) refresh_sec = cfg.refreshNearSec;
            else if (min_to_next <= cfg.refreshMidMin)    refresh_sec = cfg.refreshMidSec;
            else if (min_to_next <= cfg.refreshFarMin)    refresh_sec = cfg.refreshFarSec;
            else                                          refresh_sec = cfg.refreshVeryfarSec;
            if (min_to_next >= 0)
                ESP_LOGI(TAG, "Nächster Zug in %d Min -> Refresh %d s", min_to_next, refresh_sec);
        } else {
            refresh_sec = cfg.refreshMidSec;
        }

        // Wartephase mit LED-Blink, OLED-Invert und Uhr-Update
        TickType_t wait_end = xTaskGetTickCount() + seconds_to_ticks((uint32_t)refresh_sec);
        bool blink_on = true;
        TickType_t next_toggle = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
        TickType_t next_clock = xTaskGetTickCount() + pdMS_TO_TICKS(30 * 1000);
        TickType_t next_bar = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (xTaskGetTickCount() < wait_end && (run_forever || xTaskGetTickCount() < active_end)) {
            TickType_t t = xTaskGetTickCount();
            if (!success && cfg.ledErrorBlinkMs > 0 && t >= next_toggle) {
                blink_on = !blink_on;
                if (blink_on) led_set_rgb(cfg.ledCancelledRgb);
                else          led_off();
                next_toggle = t + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
            }
            if (cfg.oledInvertMin > 0 && t >= next_invert) {
                inverted = !inverted;
                display_set_inverted(inverted);
                next_invert = t + minutes_to_ticks((uint32_t)cfg.oledInvertMin);
            }
            if (t >= next_clock) {
                // Nur wiederholen, was die äußere Schleife entschieden hat:
                // sonst überschrieb der 30-s-Redraw die Fehlerseite mit alten
                // Daten — ohne "!"-Marker, also scheinbar aktuell.
                if (has_cached && (success || show_stale))
                    display_departures(last_deps, show_stale);
                else if (!success)
                    display_error();
                display_countdown_bar(run_forever, active_start, active_end);
                // Auch hier nachfuehren: zwischen zwei API-Zyklen liegen bis zu
                // refreshVeryfarSec, so lange soll das Panel nicht veralten.
                publish_status(run_forever, active_end);
                next_clock = t + pdMS_TO_TICKS(30 * 1000);
            }
            if (t >= next_bar) {
                display_countdown_bar(run_forever, active_start, active_end);
                next_bar = t + pdMS_TO_TICKS(1000);
            }
            // Button während aktivem Betrieb → sofort schlafen
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button gedrückt → Schlaf");
                button_wait_release(5000);   // sonst weckt derselbe Druck sofort wieder
                force_sleep = true;
                break;
            }
            // Config-Änderung → äußere Schleife sofort reagieren lassen
            if (g_cfg_dirty) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (force_sleep) break;
    }

    if (inverted) display_set_inverted(false);
    http_server_stop();
    display_sleep_info((cfg.sleepAfterS + 59) / 60);
    go_to_sleep((uint64_t)cfg.sleepAfterS * 1000000ULL);
}
