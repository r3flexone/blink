#pragma once

#include <stdbool.h>
#include <stddef.h>

// Eine Abfahrt ab dem konfigurierten Bahnhof
typedef struct {
    char time[6];           // "HH:MM"
    char destination[32];   // Endziel, z.B. "Basel SBB"
    char platform[6];       // Gleis, z.B. "3" oder "" wenn unbekannt
    int  delay;             // Verspätung in Minuten (0 = pünktlich)
    bool cancelled;         // true = Zug fällt aus
    bool valid;             // true = Eintrag enthält Daten
} SbbDeparture;

// WiFi verbinden (einmal aufrufen). Bei Fehler: AP-Modus starten.
void sbb_wifi_init(const char *ssid, const char *password);

// true wenn WiFi-Connect fehlschlug und Gerät im AP-Konfigurationsmodus läuft
bool sbb_wifi_is_ap_mode(void);

// WiFi neu verbinden falls Verbindung verloren (no-op wenn schon verbunden)
bool sbb_wifi_reconnect(void);

// Aktuelle STA-IP als "a.b.c.d". Schreibt "" und liefert false wenn nicht
// verbunden. Für die Status-Anzeige im Panel, wenn mDNS nicht funktioniert.
bool sbb_wifi_get_ip(char *out, size_t len);

// Signalstärke der aktuellen Verbindung in dBm (0 = unbekannt).
int sbb_wifi_get_rssi(void);

// Grund des letzten fehlgeschlagenen Abrufs, "" nach einem erfolgreichen.
// Zeigt im Panel, warum "API FEHLER" auf dem Display steht.
const char *sbb_last_error(void);

// Nächste 4 Abfahrten ab jetzt holen.
//   station:      Bahnhof-Name wie auf sbb.ch (z.B. "Gelterkinden")
//   dest_filters: Array von Ziel-Teilstrings (case-insensitive)
//   filter_count: Anzahl Einträge (0 = alle Züge, kein Filter)
bool sbb_get_departures(const char *station, SbbDeparture results[4],
                        const char *dest_filters[], int filter_count);
