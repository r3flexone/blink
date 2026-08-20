"""Erzeugt Stub-Implementierungen aus den IDF-Stub-Headern, damit die
Firmware auf dem Host durchlinkt (Symbol-Check nach Umbauten)."""
import re, glob, os, sys
S = os.path.dirname(os.path.abspath(__file__))
hdrs = sorted(glob.glob(S + "/idfstub/**/*.h", recursive=True))
out = ['// Automatisch erzeugt von gen_stubs.py — nicht von Hand editieren.']
out += ['#include "%s"' % os.path.relpath(h, S + "/idfstub") for h in hdrs]
out += ['#include <stdarg.h>', '']

def strip_attrs(text):
    """__attribute__((...)) entfernen — mit Klammerzaehlung, weil ein
    nicht-gieriges .*? bei ((format(printf,3,4))) eine Klammer stehen laesst
    und damit alle folgenden Prototypen zerschiesst."""
    out = []; i = 0
    while True:
        k = text.find('__attribute__', i)
        if k < 0:
            out.append(text[i:]); break
        out.append(text[i:k])
        j = text.find('(', k)
        if j < 0: out.append(text[k:]); break
        depth = 0
        while j < len(text):
            if text[j] == '(': depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0: j += 1; break
            j += 1
        i = j
    return ''.join(out)

CMT  = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
PROTO = re.compile(r'((?:const\s+)?[A-Za-z_][A-Za-z0-9_ ]*?[\s\*]+)([a-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$', re.S)
NORETURN = {'esp_deep_sleep_start', 'esp_restart'}

seen = set()
for h in hdrs:
    text = CMT.sub('', strip_attrs(open(h).read()))
    # Praeprozessor-Zeilen und Typedefs raus, dann an ';' in Statements zerlegen
    # Praeprozessor-Zeilen inklusive ihrer Backslash-Fortsetzungen entfernen —
    # sonst bleiben von mehrzeiligen Makros (IP2STR, HTTPD_DEFAULT_CONFIG) Reste
    # stehen, die den Statement-Splitter aus dem Tritt bringen.
    keep, skipping = [], False
    for l in text.split('\n'):
        if skipping:
            skipping = l.rstrip().endswith('\\')
            continue
        if l.lstrip().startswith('#'):
            skipping = l.rstrip().endswith('\\')
            continue
        keep.append(l)
    text = '\n'.join(keep)
    depth = 0; buf = ''; stmts = []
    for ch in text:                      # Struct-/Enum-Koerper ueberspringen
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
        if ch == ';' and depth == 0:
            stmts.append(buf); buf = ''
        else:
            buf += ch
    for st in stmts:
        st = ' '.join(st.split())
        if not st or st.startswith('typedef') or st.startswith('extern'): continue
        m = PROTO.match(st)
        if not m: continue
        ret, name, args = m.group(1).strip(), m.group(2), (m.group(3).strip() or 'void')
        if name in seen: continue
        seen.add(name)
        if name in NORETURN:
            out.append('__attribute__((noreturn)) %s %s(%s) { for(;;){} }' % (ret, name, args))
        else:
            out.append('%s %s(%s) { %s }' % (ret, name, args, '' if ret == 'void' else 'return 0;'))

out += ['esp_event_base_t WIFI_EVENT = "wifi";',
        'esp_event_base_t IP_EVENT   = "ip";',
        '#ifndef STUB_NO_MAIN', 'int main(void) { return 0; }', '#endif']
open(S + '/idfstub_impl.c', 'w').write('\n'.join(out) + '\n')
print("Stub-Impl:", len(seen), "Funktionen")
