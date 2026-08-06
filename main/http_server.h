#pragma once
#include <time.h>
#include "esp_err.h"
#include "sbb.h"

/**
 * Letzte erfolgreich geholte Abfahrten, von main.c geschrieben und vom
 * HTTP-Server für GET /api/departures gelesen (reine Status-Anzeige,
 * daher ohne Lock — Schreiber ist allein der Main-Task).
 * g_last_deps_time == 0 bedeutet: noch keine erfolgreiche Abfrage.
 */
extern SbbDeparture g_last_deps[4];
extern time_t g_last_deps_time;

/**
 * Laufzeit-Status der Aktiv-Schleife, ebenfalls von main.c geschrieben und
 * von GET /api/status gelesen. Damit zeigt das Panel den echten Zustand des
 * Geräts statt ihn aus der Browser-Uhr zu erraten.
 *   g_in_window       — das Aufwachen lag in einem konfigurierten Zeitfenster
 *   g_run_forever     — Aktiv-Schleife ohne Zeitlimit (sleepEnabled = false)
 *   g_active_end_time — Wanduhr-Zeitpunkt des geplanten Schlafens (0 = unbekannt)
 */
extern volatile bool g_in_window;
extern volatile bool g_run_forever;
extern time_t        g_active_end_time;

/**
 * Startet den HTTP-Server.
 * Aufrufen NACH sbb_wifi_init() und NTP-Sync.
 */
esp_err_t http_server_start(void);

/**
 * Stoppt den HTTP-Server (optional beim Schlafen).
 */
void http_server_stop(void);
