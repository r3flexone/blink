// Nativer Test der Konfigurationstabelle (config_fields.def).
//
// Prueft, was beim Eintragen eines neuen Feldes schiefgehen kann und auf dem
// Geraet erst Wochen spaeter auffaellt:
//   - Default ausserhalb [min,max] → sanitize() verbiegt ihn still beim Boot,
//     das Panel zeigt einen anderen Wert als die Tabelle verspricht.
//   - NVS-Key laenger als 15 Zeichen → ESP-IDF lehnt ihn ab, das Feld laesst
//     sich speichern und steht nach dem Reboot wieder auf dem Default.
//   - Doppelter NVS- oder JSON-Key → zwei Felder ueberschreiben sich.
//   - sanitize() ist nicht idempotent.
#include "nvs_config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static int fails = 0;
static void check(int cond, const char *fmt, ...) {
    if (cond) return;
    va_list ap; va_start(ap, fmt);
    printf("  FAIL "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fails++;
}

int main(void) {
    blink_config_t a, b;
    nvs_config_defaults(&a);

    // --- 1. Defaults liegen in ihrem eigenen Wertebereich ---
    #define CFG_INT(f, nk, jk, def, lo, hi) \
        check((def) >= (lo) && (def) <= (hi), \
              "Default von %s ist %d, erlaubt ist [%d,%d]", #f, (def), (lo), (hi));
    #define CFG_GPIO(f, nk, jk, def, maxg) \
        check((def) >= 0 && (def) <= (maxg), \
              "Default-GPIO von %s ist %d, erlaubt ist [0,%d]", #f, (def), (maxg));
    #include "config_fields.def"

    // --- 2. sanitize() laesst die Defaults unveraendert ---
    memcpy(&b, &a, sizeof(a));
    nvs_config_sanitize(&b);
    check(memcmp(&a, &b, sizeof(a)) == 0,
          "sanitize() veraendert die Firmware-Defaults");

    // --- 3. sanitize() ist idempotent ---
    memcpy(&a, &b, sizeof(b));
    nvs_config_sanitize(&b);
    check(memcmp(&a, &b, sizeof(a)) == 0, "sanitize() ist nicht idempotent");

    // --- 4. NVS-Keys: <= 15 Zeichen und eindeutig; JSON-Keys eindeutig ---
    const char *nvs_keys[]  = {
        #define CFG_INT(f, nk, jk, def, lo, hi)  nk,
        #define CFG_GPIO(f, nk, jk, def, maxg)   nk,
        #define CFG_BOOL(f, nk, jk, def)         nk,
        #define CFG_STR(f, nk, jk, def)          nk,
        #define CFG_RGB(f, nk, jk, r, g, b)      nk,
        #include "config_fields.def"
    };
    const char *json_keys[] = {
        #define CFG_INT(f, nk, jk, def, lo, hi)  jk,
        #define CFG_GPIO(f, nk, jk, def, maxg)   jk,
        #define CFG_BOOL(f, nk, jk, def)         jk,
        #define CFG_STR(f, nk, jk, def)          jk,
        #define CFG_RGB(f, nk, jk, r, g, b)      jk,
        #include "config_fields.def"
    };
    const int n = (int)(sizeof(nvs_keys) / sizeof(nvs_keys[0]));
    for (int i = 0; i < n; i++) {
        check(strlen(nvs_keys[i]) <= 15,
              "NVS-Key \"%s\" ist %d Zeichen lang, ESP-IDF erlaubt 15",
              nvs_keys[i], (int)strlen(nvs_keys[i]));
        for (int k = i + 1; k < n; k++) {
            check(strcmp(nvs_keys[i],  nvs_keys[k])  != 0, "NVS-Key \"%s\" doppelt",  nvs_keys[i]);
            check(strcmp(json_keys[i], json_keys[k]) != 0, "JSON-Key \"%s\" doppelt", json_keys[i]);
        }
    }

    // --- 5. Werte ausserhalb der Grenzen werden geklemmt, nicht uebernommen ---
    nvs_config_defaults(&a);
    a.apiRetryCount = 0;      // wuerde die Retry-Schleife nie laufen lassen
    a.refreshNearSec = 0;     // wuerde die API im Dauerlauf abfragen
    a.timeWindowCount = 99;
    a.delayBigMin = 1; a.delaySmallMin = 200;
    snprintf(a.oledAddr, sizeof(a.oledAddr), "3G");
    a.station[0] = '\0';
    nvs_config_sanitize(&a);
    check(a.apiRetryCount >= 1,        "apiRetryCount = 0 nicht korrigiert");
    check(a.refreshNearSec >= 10,      "refreshNearSec = 0 nicht korrigiert");
    check(a.timeWindowCount <= MAX_TIME_WINDOWS, "timeWindowCount nicht geklemmt");
    check(a.delayBigMin > a.delaySmallMin,
          "delayBigMin (%d) muss ueber delaySmallMin (%d) liegen",
          a.delayBigMin, a.delaySmallMin);
    check(strcmp(a.oledAddr, "0x3C") == 0, "unbrauchbare oledAddr nicht ersetzt");
    check(a.station[0] != '\0',            "leere Station nicht ersetzt");

    printf(fails ? "\n%d Pruefung(en) fehlgeschlagen.\n" : "\nAlle Pruefungen bestanden.\n", fails);
    return fails ? 1 : 0;
}
