#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "nvs_config.h"
#include "sbb.h"

/**
 * I2C-Bus und SSD1306 aufsetzen. Eine unbrauchbare oledAddr aus NVS faellt auf
 * 0x3C zurueck; scheitert die Initialisierung ganz, bleiben alle weiteren
 * display_*-Aufrufe wirkungslos, statt das Geraet lahmzulegen.
 *
 * Haelt den Config-Zeiger (nicht kopieren): display_departures() zieht den
 * Stationsnamen daraus und soll nach einem Config-Reload den neuen zeigen.
 */
void display_init(const blink_config_t *cfg);
bool display_ready(void);

void display_on(void);
void display_off(void);
void display_clear(void);

/** Burn-in-Schutz: Anzeige invertieren. */
void display_set_inverted(bool on);

/** Kopfzeile + bis zu drei Textzeilen. NULL-Zeilen werden ausgelassen. */
void display_message(const char *title, const char *l1, const char *l2, const char *l3);

void display_departures(const SbbDeparture deps[DEP_COUNT], bool stale);
void display_error(void);

/** "SCHLAFE / BIS ... / (n MIN)" und zwei Sekunden stehen lassen. */
void display_sleep_info(int sleep_min);

/**
 * Countdown-Balken in der untersten Zeile aktualisieren. Uebertraegt nur
 * Page 7, laeuft also auch im Sekundentakt guenstig.
 * Bei run_forever bleibt der Balken voll.
 */
void display_countdown_bar(bool run_forever, TickType_t active_start, TickType_t active_end);
