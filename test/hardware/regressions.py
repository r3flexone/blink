"""Opt-in hardware regressions; temporarily changes filters/retry settings.
Restores the original GET /api/config in finally; never changes passwords.
Run while the ESP is active: python test/hardware/regressions.py --host 192.168.0.14
"""
import argparse
import json
from pathlib import Path
import socket
import time
import urllib.error
import urllib.request

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', required=True)
args=parser.parse_args()
base='http://'+args.host

def call(path, data=None, headers=None):
    req=urllib.request.Request(base+path, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=5) as r: return r.status, r.read()
    except urllib.error.HTTPError as e: return e.code, e.read()

def get(path):
    code,body=call(path); assert code==200, (path,code)
    return json.loads(body)

def post(config):
    code,body=call('/api/config',json.dumps(config).encode(),{'Content-Type':'application/json'})
    assert code==200, (code,body)

def wait_for(fn, timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=fn()
        if result:return result
        time.sleep(.25)
    raise AssertionError('Timed out waiting for device state')

original=get('/api/config')
out=Path(__file__).resolve().parents[2]/'build'
out.mkdir(exist_ok=True)
(out/'hardware-original-config.json').write_text(json.dumps(original,indent=2),encoding='utf-8')
assert get('/api/status')['wifi']
assert wait_for(lambda:get('/api/departures')['departures'])
print('Live departures and WiFi OK',flush=True)
long_origin='http://'+'a'*63+'.'+'b'*63+'.example'
for origin in ['http://example.invalid',long_origin]:
    assert call('/api/restart',b'',{'Origin':origin})[0]==403
    assert call('/api/config',b'{}',{'Origin':origin,'Content-Type':'application/json'})[0]==403
print('Short and 142-character foreign Origins rejected on both endpoints',flush=True)
assert call('/api/config',b'{}',{'Content-Type':'text/plain'})[0]==415
assert call('/api/config',b'{',{'Content-Type':'application/json'})[0]==400
try:
    body=json.dumps(original).encode();cut=len(body)//2
    with socket.create_connection((args.host,80),timeout=5) as sock:
        head=f'POST /api/config HTTP/1.1\r\nHost: {args.host}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n'.encode()
        sock.sendall(head+body[:cut]);time.sleep(.3);sock.sendall(body[cut:])
        assert sock.recv(4096).startswith(b'HTTP/1.1 200')
    assert get('/api/config')==original
    print('Split TCP POST and complete configuration roundtrip OK',flush=True)
    post({'destFilters':['ZZZ_REPAIR_NO_MATCH_ZZZ'],'apiRetryCount':3,'apiRetryDelayS':300})
    wait_for(lambda:'Filter' in get('/api/status')['lastError'])
    cache=get('/api/departures')
    assert cache['departures']==[] and cache['ageS']==-1,cache
    print('Filter change invalidated old departure cache',flush=True)
    started=time.monotonic()
    post(original)
    wait_for(lambda:not get('/api/status')['lastError'] and get('/api/departures')['departures'])
    elapsed=time.monotonic()-started
    assert elapsed<15,elapsed
    print(f'300-second retry wait interrupted by config save; live data back after {elapsed:.2f}s',flush=True)
finally:
    post(original)
    assert get('/api/config')==original
    print('Original configuration restored and verified',flush=True)
print('All hardware regressions passed',flush=True)
