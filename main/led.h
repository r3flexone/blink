#pragma once
#include <stdint.h>
#include "nvs_config.h"
#include "sbb.h"

/**
 * WS2812 an cfg->ledGpio initialisieren.
 *
 * Der Zeiger wird gehalten, nicht kopiert: main.c laedt die Config nach einem
 * Web-Panel-Save in dieselbe Struktur nach, und die LED soll danach ohne
 * weiteres Zutun die neuen Farben und die neue Helligkeit verwenden.
 *
 * Schlaegt die Initialisierung fehl (z.B. unbrauchbarer GPIO aus NVS), bleiben
 * alle weiteren led_*-Aufrufe wirkungslos, statt einen Panic-Boot-Loop
 * auszuloesen, der nur per Flash-Erase endet.
 */
void led_init(const blink_config_t *cfg);

void led_set(uint8_t r, uint8_t g, uint8_t b);
void led_off(void);

/** Bequemer Aufruf fuer die cfg.ledXxxRgb-Tripel. */
void led_set_rgb(const uint8_t rgb[3]);

/** Schlimmster Status aller gueltigen Zuege: Ausfall > gross > klein > OK. */
void led_show_worst_status(const SbbDeparture deps[DEP_COUNT]);
