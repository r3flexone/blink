#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "sbb.h"

/**
 * Laufzeitstatus, den das Web-Panel spiegelt.
 *
 * Geschrieben ausschliesslich vom Main-Task, gelesen vom httpd-Task. Der
 * Abfahrts-Cache ist mehrere hundert Byte gross — ohne Sperre kann das
 * memcpy des Main-Tasks mitten in die Serialisierung des httpd-Tasks fallen
 * und das Panel zeigt die Zeit des einen Zuges mit dem Ziel des naechsten.
 * Auch time_t ist auf ESP-IDF 64 Bit und damit auf einem 32-Bit-Kern nicht
 * atomar lesbar. Beides liegt deshalb hinter einem Mutex, und die Daten
 * stehen als static in http_server.c statt als globale Variablen.
 *
 * http_status_init() muss vor dem ersten Setter laufen.
 */
void http_status_init(void);

/** Letzte erfolgreich geholte Abfahrten. */
void http_status_set_departures(const SbbDeparture deps[DEP_COUNT], time_t when);

/**
 * Zustand der Aktiv-Schleife.
 *   in_window   — die aktuelle Uhrzeit liegt in einem konfigurierten Fenster
 *   run_forever — Schleife ohne Zeitlimit (sleepEnabled = false)
 *   active_end  — Wanduhr-Zeitpunkt des geplanten Schlafens (0 = unbekannt)
 */
void http_status_set_active(bool in_window, bool run_forever, time_t active_end);

/**
 * Startet den HTTP-Server.
 * Aufrufen NACH sbb_wifi_init() und NTP-Sync.
 */
esp_err_t http_server_start(void);

/**
 * Stoppt den HTTP-Server (optional beim Schlafen).
 */
void http_server_stop(void);
