"""Native compile/link and regression tests, without touching ESP hardware.

python test/native/run.py
python test/native/run.py --cc path/to/zig.exe cc
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / 'test/native'
OUT = ROOT / 'build/native-tests'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', nargs='+', default=shlex.split(os.environ.get('CC', 'gcc')))
cc = parser.parse_args().cc
os.chdir(ROOT)
OUT.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(HERE / 'gen_stubs.py')], check=True)
flags = ['-std=gnu99', '-I' + str(HERE / 'idfstub'), '-I' + str(ROOT / 'main'),
         '-Wall', '-Wextra', '-Wno-unused-parameter']

def compile_source(source, label, extra=()):
    wrapper = OUT / (label + '.c')
    wrapper.write_text('#include "' + (HERE / 'host_compat.h').as_posix() + '"\n'
                       '#include "' + source.as_posix() + '"\n', encoding='utf-8')
    obj = OUT / (label + '.o')
    subprocess.run(cc + flags + list(extra) + ['-c', str(wrapper), '-o', str(obj)], check=True)
    return obj

def link(objects, name):
    exe = OUT / (name + ('.exe' if os.name == 'nt' else ''))
    subprocess.run(cc + list(map(str, objects)) + ['-o', str(exe)], check=True)
    return exe

sources = sorted((ROOT / 'main').glob('*.c'))
objects = {s.name: compile_source(s, 'fw-' + s.stem) for s in sources}
stub = compile_source(HERE / 'idfstub_impl.c', 'stubs')
link([*objects.values(), stub], 'firmware-link')
print('All firmware sources compiled and linked', flush=True)
stub_no_main = compile_source(HERE / 'idfstub_impl.c', 'stubs-no-main', ['-DSTUB_NO_MAIN'])
for filename, included in [('config_table.test.c', None), ('departures.test.c', 'sbb.c'),
                           ('origin.test.c', 'http_server.c'), ('runtime.test.c', 'main.c')]:
    test = compile_source(HERE / filename, filename[:-2])
    deps = [obj for name, obj in objects.items() if name != included]
    exe = link([test, *deps, stub_no_main], filename[:-2])
    subprocess.run([str(exe)], check=True)
print('All native regressions passed', flush=True)
