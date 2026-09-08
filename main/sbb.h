#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Anzahl Abfahrten, die geholt, angezeigt und im Panel gespiegelt werden.
// Steckte frueher als nackte 4 in sbb.c, main.c und http_server.h.
#define DEP_COUNT 4

// Eine Abfahrt ab dem konfigurierten Bahnhof
typedef struct {
    char time[6];           // "HH:MM"
    char destination[32];   // Endziel, z.B. "Basel SBB"
    char platform[6];       // Gleis, z.B. "3" oder "" wenn unbekannt
    time_t expectedDeparture; // Abfahrt inkl. Verspaetung, als UTC-Zeitstempel
    int  delay;             // Verspätung in Minuten (0 = pünktlich)
    bool cancelled;         // true = Zug fällt aus
    bool valid;             // true = Eintrag enthält Daten
} SbbDeparture;

// WiFi verbinden (einmal aufrufen). Bei Fehler: AP-Modus starten.
void sbb_wifi_init(const char *ssid, const char *password);

// true wenn WiFi-Connect fehlschlug und Gerät im AP-Konfigurationsmodus läuft
bool sbb_wifi_is_ap_mode(void);

// true wenn die STA-Verbindung wirklich steht (IP bezogen). Nicht dasselbe wie
// !sbb_wifi_is_ap_mode(): bricht die Verbindung im Betrieb weg, bleibt der
// AP-Modus aus, das Gerät ist aber trotzdem offline.
bool sbb_wifi_is_connected(void);

// Reconnect anstossen (no-op wenn schon verbunden oder im AP-Modus) und
// getrennt darauf warten. Die Aufteilung erlaubt es dem Aufrufer, waehrend der
// Wartezeit weiter den Taster zu pollen — ein einzelner 15-s-Block liesse das
// Geraet so lange auf den Sleep-Knopf nicht reagieren.
void sbb_wifi_reconnect_start(void);
bool sbb_wifi_wait_connected(int timeout_ms);

// Aktuelle STA-IP als "a.b.c.d". Schreibt "" und liefert false wenn nicht
// verbunden. Für die Status-Anzeige im Panel, wenn mDNS nicht funktioniert.
bool sbb_wifi_get_ip(char *out, size_t len);

// Signalstärke der aktuellen Verbindung in dBm (0 = unbekannt).
int sbb_wifi_get_rssi(void);

// Grund des letzten fehlgeschlagenen Abrufs, "" nach einem erfolgreichen.
// Zeigt im Panel, warum "API FEHLER" auf dem Display steht.
const char *sbb_last_error(void);

// Nächste DEP_COUNT Abfahrten ab jetzt holen.
//   station:      Bahnhof-Name wie auf sbb.ch (z.B. "Gelterkinden")
//   dest_filters: Array von Ziel-Teilstrings (case-insensitive)
//   filter_count: Anzahl Einträge (0 = alle Züge, kein Filter)
bool sbb_get_departures(const char *station, SbbDeparture results[DEP_COUNT],
                        const char *dest_filters[], int filter_count);
