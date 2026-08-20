#pragma once
#include <stdbool.h>
#include "nvs_config.h"

/**
 * Taster-GPIO als Eingang mit Pullup konfigurieren.
 *
 * Der Taster hat zwei Rollen: Deep-Sleep-Wakeup (EXT1, daher muss es ein
 * RTC-GPIO sein) und "jetzt schlafen"-Taste waehrend der Aktiv-Schleife.
 * Wie bei den anderen Modulen wird der Config-Zeiger gehalten, damit ein
 * geaenderter buttonGpio nach einem Reload sofort greift.
 */
void button_init(const blink_config_t *cfg);

/** Entprellt: ein einzelner Stoerimpuls darf das Geraet nicht schlafen legen. */
bool button_pressed(void);

/**
 * Wartet (begrenzt) bis der Taster losgelassen ist.
 *
 * Noetig nach jeder Haltemessung und vor dem Schlafengehen: ein noch
 * gedrueckter Taster wird sonst sofort als naechster Druck gelesen — die
 * Aktiv-Schleife legte das Geraet direkt wieder schlafen, bzw. der Deep Sleep
 * wurde augenblicklich wieder beendet.
 */
void button_wait_release(int timeout_ms);

/**
 * Haltedauer in ms messen, Abbruch nach max_ms. Liefert die gemessene Dauer.
 */
int button_measure_hold(int max_ms);
