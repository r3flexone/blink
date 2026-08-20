# Native Tests

Prüft die Firmware ohne ESP-IDF und ohne Hardware.

```
./test/native/run.sh
```

Exit-Code 0 = alles bestanden. Gebraucht werden nur `gcc` und `python3`.

## Was geprüft wird

1. **Syntax-Check** jeder Datei in `main/` gegen die Stub-Header in `idfstub/`,
   mit `-Wall -Wextra`. Fängt Tippfehler, falsche Typen und fehlende Includes.
2. **Nativer Link** der gesamten Firmware gegen automatisch erzeugte
   Stub-Implementierungen (`gen_stubs.py`). Findet fehlende und doppelte
   Symbole — genau die Fehlerklasse, die beim Verschieben von Code zwischen
   Übersetzungseinheiten entsteht und die ein Syntax-Check pro Datei nicht
   sieht.
3. **`config_table.test.c`** prüft `config_fields.def`: Defaults innerhalb
   ihres eigenen Wertebereichs, NVS-Keys ≤ 15 Zeichen und eindeutig, JSON-Keys
   eindeutig, `nvs_config_sanitize()` idempotent und die Defaults unverändert
   lassend, sowie das Klemmen der Werte, die das Gerät sonst unbrauchbar machen
   (`apiRetryCount = 0`, `refreshNearSec = 0`, unbrauchbare `oledAddr`).

## Grenzen

Das ist **kein** Ersatz für `idf.py build` und erst recht keiner für einen Test
auf dem Gerät. Die Stub-Header bilden die IDF-APIs nur so weit ab, wie dieses
Projekt sie benutzt; Verhalten wird nicht simuliert. Wer eine neue IDF-Funktion
benutzt, trägt sie in `idfstub/` nach — `gen_stubs.py` erzeugt die passende
leere Implementierung daraus selbst.

Für das Web-Panel gibt es die Playwright-Tests unter `test/panel/`.
