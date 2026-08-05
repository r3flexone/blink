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

## Ausführen

```
npm install --no-save playwright
npx playwright install chromium
node test/panel/panel.test.js
```

Findet Playwright seinen Browser nicht selbst, den Pfad setzen:
`CHROMIUM_PATH=/pfad/zu/chrome node test/panel/panel.test.js`

Exit-Code 0 = alles bestanden. Der Test startet den Mock auf Port 8099 selbst.
