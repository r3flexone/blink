#!/bin/sh
# Firmware-Pruefungen ohne ESP-IDF und ohne Hardware.
#
#   1. Syntax-Check jeder Quelle gegen die Stub-Header (-Wall -Wextra)
#   2. Nativer Link der gesamten Firmware — findet fehlende und doppelte
#      Symbole, die ein reiner Syntax-Check pro Datei nicht sieht
#   3. Nativer Test der Konfigurationstabelle
#
# Ersetzt keinen echten Build und keinen Hardware-Test, faengt aber Tippfehler,
# falsche Typen und fehlende Includes in allen main/*.c.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/sbb-native-test
CC=${CC:-gcc}
CFLAGS="-std=gnu99 -I$HERE/idfstub -I$ROOT/main"
rm -rf "$OUT"; mkdir -p "$OUT"

# secrets.h faellt unter .gitignore und fehlt im frischen Clone. Fuer die Tests
# genuegt ein Platzhalter — und weil idfstub/ vor main/ im Include-Pfad steht,
# geraten echte Zugangsdaten nie in einen Testlauf.
[ -f "$HERE/idfstub/secrets.h" ] || cat > "$HERE/idfstub/secrets.h" <<'SECRETS'
#pragma once
#define WIFI_SSID "test-ssid"
#define WIFI_PASS "test-pass"
SECRETS

echo "== 1. Syntax-Check =="
rc=0
for f in "$ROOT"/main/*.c; do
  case "$f" in */cJSON.c) continue;; esac          # Fremdcode, unveraendert
  if out=$($CC -fsyntax-only $CFLAGS -Wall -Wextra -Wno-unused-parameter "$f" 2>&1) && [ -z "$out" ]; then
    :
  else
    echo "### $(basename "$f")"; echo "$out"; rc=1
  fi
done
[ $rc -eq 0 ] && echo "  alle Dateien sauber"
[ $rc -eq 0 ] || exit 1

echo "== 2. Nativer Link =="
python3 "$HERE/gen_stubs.py" > /dev/null
$CC -c -w $CFLAGS "$HERE/idfstub_impl.c" -o "$OUT/idfstub_impl.o"
for f in "$ROOT"/main/*.c; do
  $CC -c -w $CFLAGS "$f" -o "$OUT/$(basename "$f" .c).o"
done
$CC "$OUT"/*.o -o "$OUT/fw"
echo "  alle Symbole aufgeloest, keine Duplikate"

echo "== 3. Konfigurationstabelle =="
$CC -w -DSTUB_NO_MAIN $CFLAGS "$HERE/config_table.test.c" "$ROOT/main/nvs_config.c" \
    "$HERE/idfstub_impl.c" -o "$OUT/config_test"
"$OUT/config_test"
