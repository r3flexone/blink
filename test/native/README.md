# Native Tests

Prüft Firmware-Code ohne ESP-IDF und ohne Hardware:

```sh
python test/native/run.py
# Linux/macOS alternativ:
./test/native/run.sh
# Windows mit portablem Zig-C-Compiler:
python test/native/run.py --cc path/to/zig.exe cc
```

Python 3 und GCC (Standard, alternativ CC-Umgebungsvariable) oder Zig werden
benötigt. Der Runner verwendet Argumentlisten und unterstützt Projektpfade
mit Leerzeichen. Ausgaben liegen unter dem ignorierten `build/native-tests/`.
Unter Windows übersetzen Test-Wrapper `localtime_r` und `setenv` auf die
entsprechenden Host-Funktionen; die Firmware bleibt davon unberührt.

Der Lauf kompiliert alle `main/*.c` mit `-Wall -Wextra`, linkt die gesamte
Firmware gegen generierte IDF-Stubs und führt folgende Regressionstests aus:

- `config_table.test.c`: Defaults, eindeutige NVS-/JSON-Keys, Key-Längen,
  Sanitize-Idempotenz, Grenzen und vollständige OLED-Adressvalidierung.
- `departures.test.c`: echte Parser-/Auswahllogik mit kontrollierter
  HTTP-Antwort; Verspätung, Reihenfolge, ausgeschlossene vergangene Züge,
  Datum, Mitternacht, mehr als zwölf Stunden Abstand, Zeitzonen, Sommerzeit,
  Schaltjahr und ungültige Zeitangaben.
- `origin.test.c`: fehlender, passender, fremder und überlanger Origin;
  vorhandene, nicht lesbare Header dürfen keinen Zugriff erlauben.
- `runtime.test.c`: Cache-Abfrageschlüssel und echte Retry-Wartefunktion mit
  kontrollierten Ticks; Taster, Konfigurationswechsel, Deadline und Tick-Wrap.

Exit-Code 0 bedeutet, dass alle Prüfungen bestanden wurden. Fehlende oder
doppelte Symbole scheitern beim Linken, Regressionen an Assertions.

Die Tests ersetzen weder `idf.py build` noch Hardwaretests. IDF-Stubs bilden
Schnittstellen ab, kein reales Treiberverhalten. Neue IDF-Funktionen benötigen
passende Deklarationen in `idfstub/`; `gen_stubs.py` erzeugt Implementierungen.
Die Windows-Zeitwrapper sind keine Simulation der ESP-Zeitzonenbehandlung.

Paneltests stehen unter `test/panel/`. Die gesonderte Hardwareprüfung
`python test/hardware/regressions.py --host IP` benötigt einen aktiven ESP,
ändert Filter/Retry-Einstellungen vorübergehend und stellt den gelesenen
Ausgangsstand am Ende wieder her. Sie gehört nicht zum nativen Standardlauf.
