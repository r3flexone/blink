// Mock-ESP für den Panel-Test: liefert index.html und die API-Endpunkte,
// damit sich das Web-Panel ohne Hardware prüfen lässt.
const http = require('http'), fs = require('fs'), path = require('path');
const PANEL = path.join(__dirname, '..', '..', 'main', 'spiffs', 'index.html');

let mode = process.env.MOCK_MODE || 'ok';   // ok | cfgfail
let saved = null;

const cfg = {
  timeWindows: [{startH:6,startM:45,endH:7,endM:0},{startH:17,startM:0,endH:17,endM:30}],
  buttonActiveMin:15, buttonLongPressMs:3000, buttonLongActiveMin:20, buttonGpio:0,
  ssid:'MeinWLAN', ntpTimeoutS:7, station:'Basel SBB', panelAuthEnabled:false,
  destFilters:['','Liestal'], destFilterCount:2,
  sleepEnabled:true, sleepFallbackS:300, sleepAfterS:300, sleepMaxMin:90,
  ledGpio:48, sdaGpio:4, sclGpio:5, oledAddr:'0x3C', oledInvertMin:5,
  ledOkColor:'#00FF00', ledDelaySmallColor:'#00FFFF', ledDelayBigColor:'#8000FF',
  ledCancelledColor:'#FF0000', ledLoadingColor:'#FF8000', ledErrorBlinkMs:500,
  delaySmallMin:3, delayBigMin:8,
  refreshNearSec:30, refreshMidSec:120, refreshFarSec:300, refreshVeryfarSec:600,
  refreshNearMin:5, refreshMidMin:10, refreshFarMin:30,
  apiRetryCount:3, apiRetryDelayS:5, staleMaxMin:10, weekdaysOnly:true,
  weekendSleepEnabled:true, weekendStartDay:5, weekendStartH:18, weekendStartM:0,
  weekendEndDay:1, weekendEndH:5, weekendEndM:0,
};

const status = {
  wifi:true, ntp:true, ip:'192.168.1.42', rssi:-58, heapKb:142, uptimeS:3725,
  time:'06:52:07', weekday:3, inWindow:true, runForever:false,
  activeUntilS:480, lastError:'HTTP 404 — Station \'Basle SBB\' falsch geschrieben?',
};

http.createServer((req, res) => {
  const j = o => { res.writeHead(200,{'Content-Type':'application/json'}); res.end(JSON.stringify(o)); };
  if (req.url === '/' )              { res.writeHead(200,{'Content-Type':'text/html'}); return res.end(fs.readFileSync(PANEL)); }
  if (req.url === '/api/status')     return j(status);
  if (req.url === '/api/departures') return j({ageS:42, departures:[
      {time:'06:55',destination:'Basel SBB',platform:'3',delay:0,cancelled:false},
      {time:'07:10',destination:'Liestal',platform:'2',delay:4,cancelled:false}]});
  if (req.url === '/api/config' && req.method === 'GET') {
    if (mode === 'cfgfail') { res.writeHead(503); return res.end('schlaeft'); }
    return j(cfg);
  }
  if (req.url === '/api/config' && req.method === 'POST') {
    let b=''; req.on('data',c=>b+=c); req.on('end',()=>{ saved=b; j({ok:true}); });
    return;
  }
  if (req.url.startsWith('/_mode')) { mode = req.url.split('=')[1]; return j({mode}); }
  if (req.url === '/_saved')  return j({body: saved});
  res.writeHead(404); res.end();
}).listen(8099, () => console.log('mock auf 8099, mode=' + mode));
