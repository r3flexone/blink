#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtol() für die I2C-Adressprüfung

static const char *TAG = "nvs_config";

// Alle skalaren Felder kommen aus config_fields.def — Defaults, Wertebereiche,
// NVS-Keys und JSON-Keys stehen dort an einer Stelle. Hier bleiben nur die
// Felder mit Sonderbehandlung (Arrays, Querbezüge) sowie die NVS-Mechanik.

// ===== DEFAULTS =====
void nvs_config_defaults(blink_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    // Zeitfenster: 1 Fenster 06:45–07:00
    cfg->timeWindowCount = 1;
    cfg->timeWindows[0]  = (time_window_t){6, 45, 7, 0};

    // Ziel-Filter: keiner = alle Züge
    cfg->destFilterCount = 0;

    #define CFG_INT(f, nk, jk, def, lo, hi)  cfg->f = (def);
    #define CFG_GPIO(f, nk, jk, def, maxg)   cfg->f = (def);
    #define CFG_BOOL(f, nk, jk, def)         cfg->f = (def);
    #define CFG_STR(f, nk, jk, def)          snprintf(cfg->f, sizeof(cfg->f), "%s", (def));
    #define CFG_RGB(f, nk, jk, r, g, b)      cfg->f[0] = (r); cfg->f[1] = (g); cfg->f[2] = (b);
    #include "config_fields.def"
}

// ===== SANITIZE =====
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void nvs_config_sanitize(blink_config_t *cfg) {
    cfg->timeWindowCount = clampi(cfg->timeWindowCount, 1, MAX_TIME_WINDOWS);
    for (int i = 0; i < MAX_TIME_WINDOWS; i++) {
        cfg->timeWindows[i].startH = clampi(cfg->timeWindows[i].startH, 0, 23);
        cfg->timeWindows[i].startM = clampi(cfg->timeWindows[i].startM, 0, 59);
        cfg->timeWindows[i].endH   = clampi(cfg->timeWindows[i].endH,   0, 23);
        cfg->timeWindows[i].endM   = clampi(cfg->timeWindows[i].endM,   0, 59);
    }
    cfg->destFilterCount = clampi(cfg->destFilterCount, 0, MAX_DEST_FILTERS);

    #define CFG_INT(f, nk, jk, def, lo, hi)  cfg->f = clampi(cfg->f, (lo), (hi));
    #define CFG_GPIO(f, nk, jk, def, maxg)   cfg->f = clampi(cfg->f, 0, (maxg));
    #include "config_fields.def"

    // --- Querbezüge und Formate, die keine reine Bereichsprüfung sind ---

    // "stark verspätet" muss über "leicht verspätet" liegen, sonst ist eine
    // der beiden LED-Farben unerreichbar.
    cfg->delaySmallMin = clampi(cfg->delaySmallMin, 1, 239);
    if (cfg->delayBigMin <= cfg->delaySmallMin)
        cfg->delayBigMin = cfg->delaySmallMin + 1;

    if (!cfg->station[0])
        snprintf(cfg->station, sizeof(cfg->station), "Gelterkinden");

    // Als 7-Bit-I2C-Adresse lesbar? Sonst zurücksetzen — sonst zeigte das
    // Panel eine Adresse, die main.c beim Start ohnehin verwirft.
    char *end;
    long addr = strtol(cfg->oledAddr, &end, 16);
    if (end == cfg->oledAddr || *end != '\0' || addr < 0x08 || addr > 0x77)
        snprintf(cfg->oledAddr, sizeof(cfg->oledAddr), "0x3C");
}

// ===== LOAD =====
// Fehlt ein Key in NVS, bleibt der Default aus nvs_config_defaults() stehen.
#define LOAD_I32(key, field) \
    do { int32_t v_; if (nvs_get_i32(h, (key), &v_) == ESP_OK) cfg->field = (int)v_; } while (0)
#define LOAD_BOOL(key, field) \
    do { uint8_t v_; if (nvs_get_u8(h, (key), &v_) == ESP_OK) cfg->field = (bool)v_; } while (0)
#define LOAD_STR(key, field) \
    do { size_t l_ = sizeof(cfg->field); nvs_get_str(h, (key), cfg->field, &l_); } while (0)
#define LOAD_RGB(key, field) \
    do { size_t l_ = 3; nvs_get_blob(h, (key), cfg->field, &l_); } while (0)

esp_err_t nvs_config_load(blink_config_t *cfg) {
    nvs_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_CONFIG_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Kein NVS-Config — Defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    // --- Sonderfall Zeitfenster: Anzahl zuerst, dann so viele Einträge ---
    LOAD_I32("twCount", timeWindowCount);
    cfg->timeWindowCount = clampi(cfg->timeWindowCount, 1, MAX_TIME_WINDOWS);
    for (int i = 0; i < cfg->timeWindowCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "tw%d_sh", i); LOAD_I32(key, timeWindows[i].startH);
        snprintf(key, sizeof(key), "tw%d_sm", i); LOAD_I32(key, timeWindows[i].startM);
        snprintf(key, sizeof(key), "tw%d_eh", i); LOAD_I32(key, timeWindows[i].endH);
        snprintf(key, sizeof(key), "tw%d_em", i); LOAD_I32(key, timeWindows[i].endM);
    }

    // --- Sonderfall Ziel-Filter: dito ---
    LOAD_I32("filtCount", destFilterCount);
    cfg->destFilterCount = clampi(cfg->destFilterCount, 0, MAX_DEST_FILTERS);
    for (int i = 0; i < cfg->destFilterCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "filt%d", i);
        LOAD_STR(key, destFilters[i]);
    }

    // --- Sonderfall Passwörter: stehen nicht in der Tabelle, weil sie per
    //     GET /api/config nie ausgeliefert werden ---
    LOAD_STR("password",  password);
    LOAD_STR("panelPass", panelPass);

    #define CFG_INT(f, nk, jk, def, lo, hi)  LOAD_I32(nk, f);
    #define CFG_GPIO(f, nk, jk, def, maxg)   LOAD_I32(nk, f);
    #define CFG_BOOL(f, nk, jk, def)         LOAD_BOOL(nk, f);
    #define CFG_STR(f, nk, jk, def)          LOAD_STR(nk, f);
    #define CFG_RGB(f, nk, jk, r, g, b)      LOAD_RGB(nk, f);
    #include "config_fields.def"

    nvs_close(h);
    nvs_config_sanitize(cfg);
    ESP_LOGI(TAG, "Config geladen: %d Zeitfenster, Station=%s",
             cfg->timeWindowCount, cfg->station);
    return ESP_OK;
}

// ===== SAVE =====
// Ersten Fehler festhalten statt nur nvs_commit() zu prüfen: laufen die
// NVS-Seiten voll, scheitert ein einzelnes nvs_set_*, waehrend commit() sauber
// zurueckkommt — das Panel meldete dann "Gespeichert" fuer einen Wert, der
// nirgends steht.
#define SAVE(call) do { esp_err_t e_ = (call); if (e_ != ESP_OK && err == ESP_OK) err = e_; } while (0)

esp_err_t nvs_config_save(const blink_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_CONFIG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // Zeitfenster: immer alle Slots schreiben, damit ein spaeter wieder
    // erhoehtes twCount keine Reste eines frueheren Stands liest.
    SAVE(nvs_set_i32(h, "twCount", (int32_t)cfg->timeWindowCount));
    for (int i = 0; i < MAX_TIME_WINDOWS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "tw%d_sh", i); SAVE(nvs_set_i32(h, key, cfg->timeWindows[i].startH));
        snprintf(key, sizeof(key), "tw%d_sm", i); SAVE(nvs_set_i32(h, key, cfg->timeWindows[i].startM));
        snprintf(key, sizeof(key), "tw%d_eh", i); SAVE(nvs_set_i32(h, key, cfg->timeWindows[i].endH));
        snprintf(key, sizeof(key), "tw%d_em", i); SAVE(nvs_set_i32(h, key, cfg->timeWindows[i].endM));
    }

    SAVE(nvs_set_i32(h, "filtCount", (int32_t)cfg->destFilterCount));
    for (int i = 0; i < MAX_DEST_FILTERS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "filt%d", i);
        SAVE(nvs_set_str(h, key, cfg->destFilters[i]));
    }

    SAVE(nvs_set_str(h, "password",  cfg->password));
    SAVE(nvs_set_str(h, "panelPass", cfg->panelPass));

    #define CFG_INT(f, nk, jk, def, lo, hi)  SAVE(nvs_set_i32(h, nk, (int32_t)cfg->f));
    #define CFG_GPIO(f, nk, jk, def, maxg)   SAVE(nvs_set_i32(h, nk, (int32_t)cfg->f));
    #define CFG_BOOL(f, nk, jk, def)         SAVE(nvs_set_u8(h, nk, (uint8_t)cfg->f));
    #define CFG_STR(f, nk, jk, def)          SAVE(nvs_set_str(h, nk, cfg->f));
    #define CFG_RGB(f, nk, jk, r, g, b)      SAVE(nvs_set_blob(h, nk, cfg->f, 3));
    #include "config_fields.def"

    SAVE(nvs_commit(h));
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config speichern fehlgeschlagen: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Config gespeichert");
    }
    return err;
}
