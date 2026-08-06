# Panel-Smoke-Test

Prüft das Web-Panel (`main/spiffs/index.html`) ohne Hardware: ein Mock-Server
liefert `index.html` sowie `/api/config`, `/api/status` und `/api/departures`,
ein Headless-Chromium bedient die Seite.

Abgedeckt sind die Punkte, bei denen ein Fehler teuer ist:

- Speichern bleibt gesperrt, solange `GET /api/config` nicht durchkam — sonst
  schreibt ein Klick die HTML-Vorgaben über die echte Konfiguration.
- Nach dem Aufwachen des Geräts lädt das Panel die Konfiguration selbständig
  nach und gibt den Button frei.
- Status-Seite zeigt Gerätezeit, Restlaufzeit, IP/RSSI, Uptime/Heap und den
  letzten API-Fehler — alles aus `/api/status`, nichts aus der Browser-Uhr.
- Der POST enthält die geladenen Werte, und Lücken in den Ziel-Filtern
  behalten ihre Position.
- Keine JavaScript-Fehler beim Laden (fand einen Zugriff in der Dead Zone).

## roundtrip.test.js

Prüft, dass eine Einstellung den Weg *Formular → POST → Gerät → GET →
Formular* unverändert übersteht — die Fehlerklasse „gespeichert, aber nicht
übernommen":

- Eine Config mit 8 Zeitfenstern und 4 Filtern wird geladen und **ohne
  Änderung** gespeichert; alle 44 Felder müssen identisch zurückkommen.
- Passt das Gerät einen Wert an, muss das Panel warnen und den echten Wert
  anzeigen statt der Eingabe.
- Mit maximal langen Eingaben wird der POST-Body ~1500 Bytes groß und
  überschreitet damit ein TCP-Segment — der Fall, an dem das frühere
  einmalige `httpd_req_recv()` scheiterte.

## Ausführen

```
npm install --no-save playwright
npx playwright install chromium
node test/panel/panel.test.js
node test/panel/roundtrip.test.js
```

Findet Playwright seinen Browser nicht selbst, den Pfad setzen:
`CHROMIUM_PATH=/pfad/zu/chrome node test/panel/panel.test.js`

Exit-Code 0 = alles bestanden. Der Test startet den Mock auf Port 8099 selbst.
