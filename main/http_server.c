#include "http_server.h"
#include "nvs_config.h"
#include "sbb.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <strings.h>   // strncasecmp() fuer die Content-Type-Pruefung
#include <stdio.h>
#include <stdlib.h>   // free() für die cJSON-Print-Puffer
#include <time.h>

static const char *TAG = "http_server";
static httpd_handle_t server = NULL;

// ===== Laufzeitstatus (siehe http_server.h) =====
static SemaphoreHandle_t state_lock = NULL;
static SbbDeparture last_deps[DEP_COUNT];
static time_t       last_deps_time  = 0;   // 0 = noch keine erfolgreiche Abfrage
static bool         st_in_window    = false;
static bool         st_run_forever  = false;
static time_t       st_active_end   = 0;

void http_status_init(void) {
    if (!state_lock) state_lock = xSemaphoreCreateMutex();
}

// Der Main-Task haelt die Sperre nur fuer ein memcpy von wenigen hundert Byte.
static void state_lock_take(void) {
    if (state_lock) xSemaphoreTake(state_lock, portMAX_DELAY);
}
static void state_lock_give(void) {
    if (state_lock) xSemaphoreGive(state_lock);
}

void http_status_set_departures(const SbbDeparture deps[DEP_COUNT], time_t when) {
    state_lock_take();
    memcpy(last_deps, deps, sizeof(last_deps));
    last_deps_time = when;
    state_lock_give();
}

void http_status_set_active(bool in_window, bool run_forever, time_t active_end) {
    state_lock_take();
    st_in_window   = in_window;
    st_run_forever = run_forever;
    st_active_end  = active_end;
    state_lock_give();
}

// ===== Optionaler Panel-Login (HTTP Basic Auth) =====
// Leer = kein Login (Default fürs Heimnetz). Wird beim Serverstart aus NVS
// geladen und nach jedem Config-Save aktualisiert. Achtung: läuft über
// unverschlüsseltes HTTP — schützt vor Mitbewohnern, nicht vor Angreifern.
static char panel_pass[32] = "";

static void panel_pass_load(void) {
    nvs_handle_t h;
    panel_pass[0] = 0;
    if (nvs_open(NVS_CONFIG_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(panel_pass);
        if (nvs_get_str(h, "panelPass", panel_pass, &len) != ESP_OK)
            panel_pass[0] = 0;
        nvs_close(h);
    }
}

static bool auth_ok(httpd_req_t *req) {
    if (!panel_pass[0]) return true;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    unsigned char dec[96];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)hdr + 6, strlen(hdr) - 6) != 0)
        return false;
    dec[dlen] = 0;
    const char *colon = strchr((const char *)dec, ':');   // "user:pass", User egal
    if (!colon) return false;
    return strcmp(colon + 1, panel_pass) == 0;
}

// ESP_OK = durchgelassen; sonst wurde bereits eine 401-Antwort gesendet.
static esp_err_t require_auth(httpd_req_t *req) {
    if (auth_ok(req)) return ESP_OK;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SBB-Monitor\"");
    httpd_resp_sendstr(req, "Login erforderlich");
    return ESP_FAIL;
}

// ===== CSRF-Schutz fuer schreibende Endpunkte =====
// panelPass ist per Default leer, das Geraet haengt im Heimnetz. Ohne diese
// Pruefung kann jede beliebige Webseite, die der Nutzer im selben Netz oeffnet,
// per fetch() die Konfiguration ueberschreiben: mit Content-Type text/plain
// (CORS-safelisted) entfaellt der Preflight, der Request geht durch, und die
// Firmware parst den Body trotzdem als JSON. Zwei Riegel dagegen:
//
//  1. Content-Type muss application/json sein. Damit ist der Request nicht mehr
//     "simple" und der Browser schickt zuerst einen Preflight, den der Server
//     mangels OPTIONS-Handler nicht beantwortet.
//  2. Ein mitgeschickter Origin-Header muss zum eigenen Host passen. Browser
//     senden Origin bei jedem POST, also auch beim same-origin-POST des Panels
//     — dort ist er identisch mit Host. Ein abweichender Origin ist immer ein
//     Cross-Site-Aufruf. Fehlt der Header ganz (curl, lokales Tooling), wird
//     durchgelassen: dann steht kein Browser dahinter, der sich missbrauchen
//     liesse.
static bool content_type_is_json(httpd_req_t *req) {
    static const char JSON_CT[] = "application/json";
    const size_t n = sizeof(JSON_CT) - 1;
    char ct[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK)
        return false;
    if (strncasecmp(ct, JSON_CT, n) != 0) return false;
    // Danach darf nur noch ein Parameter folgen ("; charset=utf-8"),
    // damit "application/jsonx" nicht durchrutscht.
    return ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ';
}

static bool origin_is_self(httpd_req_t *req) {
    char origin[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
        return true;   // kein Origin = kein Browser-Cross-Site-Request
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    // "http://host[:port]" — Schema abschneiden und mit Host vergleichen
    const char *o = strstr(origin, "://");
    o = o ? o + 3 : origin;
    return strcmp(o, host) == 0;
}

// ESP_OK = durchgelassen; sonst wurde bereits eine Fehlerantwort gesendet.
static esp_err_t require_write_access(httpd_req_t *req, bool needs_json_body) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!origin_is_self(req)) {
        ESP_LOGW(TAG, "Schreibzugriff mit fremdem Origin abgelehnt");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Fremder Origin");
        return ESP_FAIL;
    }
    if (needs_json_body && !content_type_is_json(req)) {
        ESP_LOGW(TAG, "Schreibzugriff ohne application/json abgelehnt");
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        httpd_resp_sendstr(req, "Content-Type muss application/json sein");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ===== Hilfsfunktion: Datei aus SPIFFS streamen =====
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *mime) {
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime);
    // Sonst zeigt der Browser nach einem Firmware-Update u.U. die alte Panel-Version
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ===== Hilfsfunktion: RGB uint8[3] → "#RRGGBB" =====
static void rgb_to_hex(const uint8_t rgb[3], char *out) {
    snprintf(out, 8, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
}

// ===== Hilfsfunktion: "#RRGGBB" → RGB uint8[3] =====
static void hex_to_rgb(const char *hex, uint8_t rgb[3]) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return;
    unsigned r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        rgb[0] = (uint8_t)r;
        rgb[1] = (uint8_t)g;
        rgb[2] = (uint8_t)b;
    }
}

// ===== GET / → index.html =====
static esp_err_t handler_root(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    return send_file(req, "/spiffs/index.html", "text/html");
}

// ===== GET /api/config → vollständiges JSON =====
static esp_err_t handler_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    blink_config_t cfg;
    nvs_config_load(&cfg);

    cJSON *j = cJSON_CreateObject();

    // Zeitfenster-Array
    cJSON *tws = cJSON_AddArrayToObject(j, "timeWindows");
    for (int i = 0; i < cfg.timeWindowCount; i++) {
        cJSON *tw = cJSON_CreateObject();
        cJSON_AddNumberToObject(tw, "startH", cfg.timeWindows[i].startH);
        cJSON_AddNumberToObject(tw, "startM", cfg.timeWindows[i].startM);
        cJSON_AddNumberToObject(tw, "endH",   cfg.timeWindows[i].endH);
        cJSON_AddNumberToObject(tw, "endM",   cfg.timeWindows[i].endM);
        cJSON_AddItemToArray(tws, tw);
    }

    // Ziel-Filter
    cJSON *filters = cJSON_AddArrayToObject(j, "destFilters");
    for (int i = 0; i < cfg.destFilterCount; i++)
        cJSON_AddItemToArray(filters, cJSON_CreateString(cfg.destFilters[i]));
    cJSON_AddNumberToObject(j, "destFilterCount", cfg.destFilterCount);

    // Passwörter werden nie ausgeliefert — nur, ob der Panel-Login aktiv ist.
    cJSON_AddBoolToObject(j, "panelAuthEnabled", cfg.panelPass[0] != 0);

    // Alle übrigen Felder direkt aus der Tabelle
    #define CFG_INT(f, nk, jk, def, lo, hi)  cJSON_AddNumberToObject(j, jk, cfg.f);
    #define CFG_GPIO(f, nk, jk, def, maxg)   cJSON_AddNumberToObject(j, jk, cfg.f);
    #define CFG_BOOL(f, nk, jk, def)         cJSON_AddBoolToObject(j, jk, cfg.f);
    #define CFG_STR(f, nk, jk, def)          cJSON_AddStringToObject(j, jk, cfg.f);
    #define CFG_RGB(f, nk, jk, r, g, b) \
        { char hx_[8]; rgb_to_hex(cfg.f, hx_); cJSON_AddStringToObject(j, jk, hx_); }
    #include "config_fields.def"

    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
    return ESP_OK;
}

// ===== POST /api/config =====
static esp_err_t handler_config_post(httpd_req_t *req) {
    if (require_write_access(req, true) != ESP_OK) return ESP_OK;
    // static: 4 KB passen schlecht in den 8-KB-httpd-Stack; der Server
    // verarbeitet Requests sequentiell, daher kein Race.
    static char buf[4096];
    // httpd_req_recv() liefert nur, was gerade im Socket steht — bei einem
    // Body über einem TCP-Segment kam sonst abgeschnittenes JSON an und der
    // Save schlug mit "Invalid JSON" fehl. Deshalb bis content_len einlesen.
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body zu gross");
        return ESP_FAIL;
    }
    int received = 0, timeouts = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) r = 0;    // hängender Client
            else continue;
        }
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body unvollstaendig");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Basis ist der gespeicherte Stand: ein Body, der nur einzelne Felder
    // mitbringt (z.B. panelPassClear), darf den Rest nicht zuruecksetzen.
    blink_config_t cfg;
    nvs_config_load(&cfg);

    #define GET_INT(key, field) \
        { cJSON *v = cJSON_GetObjectItem(j, key); \
          if (cJSON_IsNumber(v)) cfg.field = (int)v->valuedouble; }
    #define GET_BOOL(key, field) \
        { cJSON *v = cJSON_GetObjectItem(j, key); \
          if (cJSON_IsBool(v)) cfg.field = cJSON_IsTrue(v); }
    #define GET_STR(key, field) \
        { cJSON *v = cJSON_GetObjectItem(j, key); \
          if (cJSON_IsString(v) && v->valuestring) { \
              strncpy(cfg.field, v->valuestring, sizeof(cfg.field) - 1); \
              cfg.field[sizeof(cfg.field) - 1] = '\0'; } }
    #define GET_RGB(key, field) \
        { cJSON *v = cJSON_GetObjectItem(j, key); \
          if (cJSON_IsString(v) && v->valuestring) hex_to_rgb(v->valuestring, cfg.field); }
    // GPIO nur in gültigem Bereich übernehmen — ein Tippfehler im Panel darf
    // das Gerät nicht in einen Panic-Boot-Loop schicken. Anders als bei einem
    // clamp bleibt der alte Wert stehen, damit das Panel den Unterschied im
    // Read-back sieht und meldet.
    #define GET_GPIO(key, field, maxg) \
        { cJSON *v = cJSON_GetObjectItem(j, key); \
          if (cJSON_IsNumber(v)) { \
              int g_ = (int)v->valuedouble; \
              if (g_ >= 0 && g_ <= (maxg)) cfg.field = g_; \
              else ESP_LOGW(TAG, "%s: GPIO %d ungueltig, behalte %d", key, g_, cfg.field); } }

    // --- Zeitfenster-Array ---
    cJSON *tws = cJSON_GetObjectItem(j, "timeWindows");
    if (cJSON_IsArray(tws)) {
        int cnt = cJSON_GetArraySize(tws);
        if (cnt > MAX_TIME_WINDOWS) cnt = MAX_TIME_WINDOWS;
        cfg.timeWindowCount = cnt;
        for (int i = 0; i < cnt; i++) {
            cJSON *tw = cJSON_GetArrayItem(tws, i);
            cJSON *sh = cJSON_GetObjectItem(tw, "startH");
            cJSON *sm = cJSON_GetObjectItem(tw, "startM");
            cJSON *eh = cJSON_GetObjectItem(tw, "endH");
            cJSON *em = cJSON_GetObjectItem(tw, "endM");
            if (cJSON_IsNumber(sh)) cfg.timeWindows[i].startH = (int)sh->valuedouble;
            if (cJSON_IsNumber(sm)) cfg.timeWindows[i].startM = (int)sm->valuedouble;
            if (cJSON_IsNumber(eh)) cfg.timeWindows[i].endH   = (int)eh->valuedouble;
            if (cJSON_IsNumber(em)) cfg.timeWindows[i].endM   = (int)em->valuedouble;
        }
    }

    // --- Ziel-Filter ---
    cJSON *filters = cJSON_GetObjectItem(j, "destFilters");
    if (cJSON_IsArray(filters)) {
        int cnt = cJSON_GetArraySize(filters);
        if (cnt > MAX_DEST_FILTERS) cnt = MAX_DEST_FILTERS;
        cfg.destFilterCount = cnt;
        for (int i = 0; i < cnt; i++) {
            cJSON *f = cJSON_GetArrayItem(filters, i);
            if (cJSON_IsString(f) && f->valuestring) {
                strncpy(cfg.destFilters[i], f->valuestring, sizeof(cfg.destFilters[i]) - 1);
                cfg.destFilters[i][sizeof(cfg.destFilters[i]) - 1] = '\0';
            }
        }
    }

    // --- Alle Skalarfelder aus der Tabelle ---
    #define CFG_INT(f, nk, jk, def, lo, hi)  GET_INT(jk, f)
    #define CFG_GPIO(f, nk, jk, def, maxg)   GET_GPIO(jk, f, maxg)
    #define CFG_BOOL(f, nk, jk, def)         GET_BOOL(jk, f)
    #define CFG_STR(f, nk, jk, def)          GET_STR(jk, f)
    #define CFG_RGB(f, nk, jk, r, g, b)      GET_RGB(jk, f)
    #include "config_fields.def"

    // --- Sonderfaelle rund um die Passwoerter ---
    // Geleerte SSID = explizit zurück auf secrets.h. Dann auch das gespeicherte
    // Passwort verwerfen — sonst entsteht die Kombination "SSID aus secrets.h
    // + altes NVS-Passwort" und der Connect schlägt fehl.
    {
        cJSON *v = cJSON_GetObjectItem(j, "ssid");
        if (cJSON_IsString(v) && v->valuestring && !v->valuestring[0])
            cfg.password[0] = '\0';
    }
    // Passwort: nur überschreiben wenn nicht leer. GET sendet es aus
    // Sicherheitsgründen nicht zurück, das Feld im UI ist nach dem Laden also
    // leer — würden wir den leeren String übernehmen, löschte jeder Save das
    // bestehende Passwort.
    {
        cJSON *v = cJSON_GetObjectItem(j, "password");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            strncpy(cfg.password, v->valuestring, sizeof(cfg.password) - 1);
            cfg.password[sizeof(cfg.password) - 1] = '\0';
        }
    }
    // Panel-Login: leeres Feld = unverändert (wie WLAN-Passwort);
    // explizites Deaktivieren über panelPassClear:true.
    {
        cJSON *v = cJSON_GetObjectItem(j, "panelPass");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            strncpy(cfg.panelPass, v->valuestring, sizeof(cfg.panelPass) - 1);
            cfg.panelPass[sizeof(cfg.panelPass) - 1] = '\0';
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "panelPassClear")))
            cfg.panelPass[0] = '\0';
    }

    #undef GET_INT
    #undef GET_BOOL
    #undef GET_STR
    #undef GET_RGB
    #undef GET_GPIO

    cJSON_Delete(j);

    // Die API ist offen (curl, altes Panel) — Werte hart begrenzen, bevor sie
    // in NVS landen. Sonst bootet das Gerät später mit z.B. endH = 99.
    nvs_config_sanitize(&cfg);

    esp_err_t err = nvs_config_save(&cfg);
    if (err == ESP_OK) {
        extern volatile bool g_cfg_dirty;
        g_cfg_dirty = true;
        strncpy(panel_pass, cfg.panelPass, sizeof(panel_pass) - 1);
        panel_pass[sizeof(panel_pass) - 1] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ===== GET /api/departures — letzte geholte Abfahrten (Cache aus main.c) =====
static esp_err_t handler_departures_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    time_t now; time(&now);

    // Erst unter der Sperre kopieren, dann in Ruhe serialisieren — sonst
    // schreibt der Main-Task waehrend des Aufbaus des JSON dazwischen.
    SbbDeparture deps[DEP_COUNT];
    time_t deps_time;
    state_lock_take();
    memcpy(deps, last_deps, sizeof(deps));
    deps_time = last_deps_time;
    state_lock_give();

    cJSON *j = cJSON_CreateObject();
    // ageS = Sekunden seit der letzten erfolgreichen Abfrage, -1 = noch keine
    cJSON_AddNumberToObject(j, "ageS", deps_time ? (double)(now - deps_time) : -1);
    cJSON *arr = cJSON_AddArrayToObject(j, "departures");
    for (int i = 0; i < DEP_COUNT; i++) {
        if (!deps[i].valid) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "time",        deps[i].time);
        cJSON_AddStringToObject(d, "destination", deps[i].destination);
        cJSON_AddStringToObject(d, "platform",    deps[i].platform);
        cJSON_AddNumberToObject(d, "delay",       deps[i].delay);
        cJSON_AddBoolToObject(d,   "cancelled",   deps[i].cancelled);
        cJSON_AddItemToArray(arr, d);
    }
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

// ===== POST /api/restart =====
static esp_err_t handler_restart(httpd_req_t *req) {
    // Kein JSON-Body noetig, aber derselbe Origin-Riegel: sonst genuegt ein
    // <form>-Submit von einer fremden Seite, um das Geraet neu zu starten.
    if (require_write_access(req, false) != ESP_OK) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Neustart per Web-Panel");
    // Response noch ausliefern lassen (gleiches Muster wie AP-Mode-Restart)
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===== GET /api/status =====
// Liefert den Zustand, den das Gerät selbst kennt. Das Panel soll nichts davon
// aus der Browser-Uhr ableiten müssen — die geht in einer anderen Zeitzone
// oder bei RTC-Drift anders als die Uhr auf dem Display.
static esp_err_t handler_status_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_OK;
    // wifi = STA-Verbindung steht wirklich (IP bezogen). Frueher stand hier
    // !sbb_wifi_is_ap_mode(), also nur "kein AP-Fallback" — bricht die
    // Verbindung im Betrieb weg, meldete das Panel weiter gruen "Verbunden",
    // waehrend direkt darunter lastError "Kein WLAN" stand.
    // apMode trennt die beiden Offline-Faelle fuer die Anzeige.
    // ntp = gueltige Systemzeit vorhanden (tm_year >= 100 == ab Jahr 2000).
    bool ap_mode = sbb_wifi_is_ap_mode();
    bool wifi = sbb_wifi_is_connected();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool ntp = (ti.tm_year >= 100);

    char ip[16];
    sbb_wifi_get_ip(ip, sizeof(ip));

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "wifi",   wifi);
    cJSON_AddBoolToObject(j, "apMode", ap_mode);
    cJSON_AddBoolToObject(j, "ntp",    ntp);
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "rssi", sbb_wifi_get_rssi());
    cJSON_AddNumberToObject(j, "heapKb", (double)(esp_get_free_heap_size() / 1024));
    cJSON_AddNumberToObject(j, "uptimeS",
        (double)(xTaskGetTickCount() / configTICK_RATE_HZ));

    // Geraetezeit als HH:MM:SS — nur wenn sie ueberhaupt gueltig ist
    if (ntp) {
        char clk[9];
        snprintf(clk, sizeof(clk), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
        cJSON_AddStringToObject(j, "time", clk);
        cJSON_AddNumberToObject(j, "weekday", ti.tm_wday);
    }

    state_lock_take();
    bool   in_window   = st_in_window;
    bool   run_forever = st_run_forever;
    time_t active_end  = st_active_end;
    state_lock_give();

    cJSON_AddBoolToObject(j, "inWindow",   in_window);
    cJSON_AddBoolToObject(j, "runForever", run_forever);
    // Sekunden bis zum geplanten Schlafen; -1 = laeuft unbegrenzt/unbekannt
    double until = -1;
    if (!run_forever && active_end > 0 && ntp)
        until = (double)(active_end > now ? active_end - now : 0);
    cJSON_AddNumberToObject(j, "activeUntilS", until);

    cJSON_AddStringToObject(j, "lastError", sbb_last_error());

    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
    return ESP_OK;
}

// ===== Server starten =====
esp_err_t http_server_start(void) {
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPIFFS mount fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    panel_pass_load();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start fehlgeschlagen");
        return ESP_FAIL;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/api/config",     .method = HTTP_GET,  .handler = handler_config_get },
        { .uri = "/api/config",     .method = HTTP_POST, .handler = handler_config_post },
        { .uri = "/api/status",     .method = HTTP_GET,  .handler = handler_status_get },
        { .uri = "/api/departures", .method = HTTP_GET,  .handler = handler_departures_get },
        { .uri = "/api/restart",    .method = HTTP_POST, .handler = handler_restart },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server gestartet — http://sbb-monitor.local");
    return ESP_OK;
}

void http_server_stop(void) {
    if (server) { httpd_stop(server); server = NULL; }
    esp_vfs_spiffs_unregister(NULL);
}
