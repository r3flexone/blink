# SBB Monitor

ESP32-S3 Abfahrtsmonitor für Schweizer Bahnhöfe (transport.opendata.ch).

Das Gerät wacht nach Zeitplan auf, holt die nächsten Abfahrten vom gewünschten Bahnhof und zeigt sie auf einem OLED-Display an. Ein NeoPixel signalisiert den schlechtesten Status der nächsten vier Züge. Danach kehrt es in den Deep Sleep zurück, um den Akku zu schonen.

## Hardware

| Komponente | Beschreibung |
|---|---|
| **Mikrokontroller** | ESP32-S3 (getestet auf ESP32-S3-DevKitC) |
| **Display** | SSD1306 128×64 OLED (I²C) |
| **Status-LED** | WS2812 NeoPixel |
| **Button** | Taster an GPIO 0 (Wake-up + Langdruck + Sleep-Taste) |

### Verdrahtung (Standardwerte)

| Signal | GPIO |
|---|---|
| NeoPixel DATA | 48 |
| OLED SDA | 4 |
| OLED SCL | 5 |
| OLED I²C Adresse | 0x3C |
| Button | 0 |

Alle Pins sind über das Web-Panel oder NVS konfigurierbar.

## Einrichtung

Es sind **keine Vorbereitungen nötig** — das Repo lässt sich direkt nach dem
Klonen bauen und flashen. Das WLAN wird danach am Gerät eingerichtet.

```bash
# 1. Ziel-Chip setzen
idf.py set-target esp32s3

# 2. Bauen und flashen
idf.py build flash monitor   # Ctrl-] zum Beenden
```

> Beim ersten Mal oder nach Änderung der Partition Table:
> `idf.py fullclean` vor dem Build ausführen.

## Konfiguration

### Web-Panel (empfohlen)

Wenn das Gerät aktiv ist (im Zeitfenster oder per Button geweckt), ist das Konfigurations-Panel unter **http://sbb-monitor.local** erreichbar.

Das Panel zeigt oben rechts an ob der ESP gerade **Online** oder **schläft**. Speichern ist gesperrt, solange die Konfiguration nicht vom Gerät geladen werden konnte — sonst würden die Formular-Vorgaben die echten Einstellungen überschreiben. Sobald das Gerät antwortet, lädt das Panel automatisch nach und gibt den Button frei.

Ein Punkt am Speichern-Button zeigt ungespeicherte Änderungen. Felder, die erst ein Neustart übernimmt (WLAN, GPIOs, I²C-Adresse), sind mit **Neustart nötig** markiert.

Dort lassen sich einstellen:

- **Zeitfenster** — bis zu 8 aktive Zeitfenster (wann das Gerät aktiv sein soll)
- **Tastendruck** — Aktiv-Dauer nach Kurz- und Langdruck
- **Bahnhof & Ziel-Filter** — Station und bis zu 4 Substring-Filter
- **Schlaf** — Deep-Sleep ein/aus, Fallback-Dauer, Max-Schlafdauer, Schlaf nach Fenster
- **Wochenend-Schlaf** — eigener, unabhängig schaltbarer Schlaf-Zeitraum (z. B. Fr 18:00 → Mo 05:00)
- **Nur Wochentage** — Sa/So kein normales Zeitfenster aktiv
- **LED-Farben** — RGB-Farben für alle Zustände (pünktlich, verspätet, Ausfall, Laden) und die Helligkeit
- **Verspätungs-Schwellen** — ab wann Cyan bzw. Lila
- **API & Refresh-Intervalle** — adaptiver Refresh, Retry, Cache-Gültigkeit
- **Hardware** — GPIO-Belegung für LED, OLED, Button
- **OLED Invert** — periodisches Invertieren gegen Einbrennen

Die **Status-Seite** zeigt den Zustand, den das Gerät selbst meldet — Gerätezeit, ob ein Zeitfenster aktiv ist und wie lange noch, IP-Adresse und Signalstärke, Laufzeit und freier Speicher, die zuletzt geholten Abfahrten sowie den Grund eines fehlgeschlagenen Abrufs. Nichts davon wird aus der Browser-Uhr abgeleitet.

Einstellungen werden in NVS gespeichert und überleben Neustarts und Deep Sleep. Die meisten greifen sofort — die Hauptschleife lädt die Konfiguration direkt nach dem Speichern neu. Änderungen an WLAN-Zugangsdaten, GPIO-Belegung und I²C-Adresse brauchen einen Neustart (Button oben rechts im Panel).

### Erste Einrichtung (WLAN)

Ein frisch geflashtes Gerät hat keine Zugangsdaten und startet deshalb sofort
im **AP-Modus**: Es spannt ein offenes WLAN `SBB-Monitor` auf, das Panel ist
unter `http://192.168.4.1` erreichbar. Dort WLAN eintragen, speichern — das
Gerät startet neu und verbindet sich. Die Zugangsdaten liegen danach in NVS und
überleben Neustarts und Deep Sleep. Dasselbe passiert automatisch, wenn das
hinterlegte WLAN später nicht mehr erreichbar ist, etwa nach einem
Router-Wechsel.

### secrets.h (optionaler Fallback)

`main/secrets.h` ist **nicht erforderlich**. Wer die Zugangsdaten trotzdem fest
einkompilieren will, kopiert `main/secrets.h.example` nach `main/secrets.h` und
trägt `WIFI_SSID` / `WIFI_PASS` ein; die Datei wird von Git ignoriert. In NVS
gespeicherte Zugangsdaten haben immer Vorrang.

## Funktionsweise

### Aufwach- und Schlaf-Logik

1. RTC-Zeit prüfen — falls keine gültige Zeit vorhanden, NTP-Sync via WiFi. Die RTC läuft im Deep Sleep weiter, ein Aufwachen kostet also normalerweise keinen erneuten Kaltstart-Sync.
2. Prüfen ob aktueller Zeitpunkt in einem aktiven Zeitfenster liegt.
3. **Außerhalb des Fensters und `sleepEnabled = true`:** Deep Sleep bis zum nächsten Fensterstart (max. `sleepMaxMin` Minuten). Im Wochenend-Schlaf-Fenster (`weekendSleepEnabled = true`) wird direkt bis zum Ende des Wochenend-Fensters geschlafen — dort greift das `sleepMaxMin`-Limit nicht. Ohne gültige Zeit: exakt `sleepFallbackS` Sekunden.
4. **`sleepEnabled = false`:** Gerät bleibt dauerhaft aktiv — auch nach dem Ende eines Zeitfensters —, der Fortschrittsbalken auf dem OLED bleibt voll. Sobald Sleep über das Web-Panel wieder aktiviert wird, startet ein frischer Timer (`buttonActiveMin` Minuten) ab dem Speicherzeitpunkt.
5. **Im Fenster oder per Button geweckt:** WLAN aufbauen, SNTP anstoßen (korrigiert die RTC-Drift), dann Aktiv-Schleife bis Fensterende bzw. bis zum Ablauf der Button-Zeit.
6. **Button während aktivem Betrieb:** Sofortiger Deep Sleep.
7. Nach dem Fenster: Deep Sleep für `sleepAfterS` Sekunden (Standard: 300 s = 5 min).

### Button-Verhalten

| Aktion | Effekt |
|---|---|
| Kurzdruck (Wakeup) | `buttonActiveMin` Minuten aktiv (Standard: 10 min) |
| Langdruck (Wakeup) | `buttonLongActiveMin` Minuten aktiv |
| Druck während Betrieb | Sofort in Deep Sleep |
| Druck im AP-Modus | Deep Sleep für `sleepFallbackS` Sekunden |

Der Taster ist entprellt, und das Gerät wartet nach einem Wakeup auf das Loslassen, bevor die Aktiv-Schleife startet. Ein langer Druck legt das Gerät also nicht versehentlich sofort wieder schlafen.

### Aktiv-Schleife

Pro Iteration:
- Abfahrten von `transport.opendata.ch` abrufen (mit Retry).
- Bei Fehler: gecachte Daten anzeigen, solange sie `< staleMaxMin` Minuten alt sind — erkennbar am `!` vor dem Bahnhofnamen. Danach erscheint stattdessen „API FEHLER", nie wieder abgelaufene Daten ohne Marker.
- NeoPixel: schlechtester Status der nächsten 4 gültigen, nicht-ausgefallenen Züge.
  - Grün = pünktlich · Cyan = leicht verspätet · Lila = stark verspätet · Rot = Ausfall
- OLED: Abfahrtsliste mit Bahnhofname und Uhrzeit in der Kopfzeile.
- Fortschrittsbalken unten: verbleibende Aktiv-Zeit (voll wenn Sleep deaktiviert).
- Adaptiver Refresh: je näher der nächste Zug, desto häufiger wird abgefragt.

### Ziel-Filter

Bis zu 4 Substring-Filter (case-insensitiv) auf Endstation und Zwischenhalte. Leer = alle Züge.

### Robustheit

- **Werte-Grenzen:** Alle Konfigurationswerte werden beim Laden aus NVS und vor jedem Speichern auf plausible Bereiche begrenzt. Weder ein alter NVS-Eintrag noch ein direkter Aufruf von `POST /api/config` kann das Gerät damit lahmlegen.
- **Schreibzugriff nur von der eigenen Seite:** Da das Panel-Passwort standardmäßig leer ist, verlangen `POST /api/config` und `POST /api/restart` einen `Content-Type: application/json` und einen `Origin`, der zum Gerät passt. Ohne das könnte jede beliebige Webseite, die im selben Netz geöffnet wird, die Konfiguration überschreiben. Aufrufe ohne `Origin` (curl, eigene Skripte) bleiben erlaubt.
- **Fehlerdiagnose im Log:** Ein falsch geschriebener Bahnhofname erscheint als HTTP-Status (die API antwortet mit 404), nicht als Parse-Fehler. Zu große Antworten werden als solche gemeldet.
- **Ungültige Hardware-Werte:** Eine unbrauchbare I²C-Adresse fällt auf `0x3C` zurück, ein fehlgeschlagener LED-Init führt nicht zum Boot-Loop.

## Projektstruktur

```
main/
  main.c            — Aufwach-/Schlaf-Ablauf, Hauptschleife, WiFi/NTP
  display.c/.h      — SSD1306-Treiber, Font, fertige Bildschirme
  led.c/.h          — WS2812: Statusfarbe, Helligkeit
  button.c/.h       — Entprellung, Halte-Messung
  sbb.c / sbb.h     — WiFi, HTTP, JSON-Parsing, Filter-Logik
  http_server.c     — Web-Panel (SPIFFS + /api/config, /api/status,
                      /api/departures, /api/restart) und Laufzeitstatus
  nvs_config.c      — Konfiguration in NVS lesen/schreiben
  config_fields.def — Tabelle aller Konfigurationsfelder (Default,
                      Wertebereich, NVS-Key, JSON-Key)
  cJSON.c           — Vendored JSON-Library
  spiffs/
    index.html      — Web-Panel UI (wird auf SPIFFS geflasht)
  secrets.h.example

test/
  native/           — Firmware-Tests ohne ESP-IDF (gcc + python3)
  panel/            — Web-Panel-Tests (Playwright, ohne Hardware)
```

Eine neue Einstellung braucht zwei Zeilen in der Firmware: das Feld in
`blink_config_t` und einen Eintrag in `config_fields.def`. Defaults,
Wertebereiche, NVS-Zugriff und beide HTTP-Handler sind Schleifen über diese
Tabelle.

## Tests

Beide Suiten laufen ohne Hardware:

```
./test/native/run.sh                  # Syntax, nativer Link, Config-Tabelle
node test/panel/panel.test.js         # Web-Panel
node test/panel/roundtrip.test.js
```

Details in `test/native/README.md` und `test/panel/README.md`. Sie ersetzen
weder `idf.py build` noch einen Test auf dem Gerät.

## Build-System

Standard ESP-IDF v6.x Projekt (baut auch ab v5.3, seit der Aufteilung von `driver` in `esp_driver_*`). Ziel: `esp32s3`.

Partition Table (`partitions.csv`):

| Name | Typ | Grösse |
|---|---|---|
| nvs | data/nvs | 24 KB |
| phy_init | data/phy | 4 KB |
| factory | app/factory | 1500 KB |
| storage | data/spiffs | 256 KB |
