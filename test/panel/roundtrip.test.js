// Rundlauf-Test: prueft, dass eine Einstellung den Weg
//   Formular -> POST -> Geraet -> GET -> Formular
// unveraendert uebersteht. Deckt genau die Klasse Fehler ab, bei der
// gespeicherte Werte still verlorengehen. Siehe README.md.
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path');
const PANEL = path.join(__dirname, '..', '..', 'main', 'spiffs', 'index.html');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0;
const check = (n,c,x) => { console.log((c?'  OK   ':'  FAIL ')+n+(c?'':'  → '+x)); if(!c) fails++; };

const LONG = 'x'.repeat(31);   // Firmware-Grenze fuer destFilters
function makeCfg(long) {
  return {
  timeWindows: [
    {startH:5,startM:5,endH:5,endM:35},   {startH:6,startM:15,endH:6,endM:50},
    {startH:7,startM:20,endH:7,endM:55},  {startH:11,startM:5,endH:11,endM:40},
    {startH:13,startM:25,endH:13,endM:45},{startH:16,startM:35,endH:17,endM:5},
    {startH:18,startM:10,endH:18,endM:40},{startH:22,startM:50,endH:23,endM:15}],
  buttonActiveMin:17, buttonLongPressMs:4500, buttonLongActiveMin:33, buttonGpio:9,
  ssid: long ? 'W'.repeat(63) : 'Netz-äöü', ntpTimeoutS:23,
  station: long ? 'S'.repeat(63) : 'Zürich Flughafen', panelAuthEnabled:false,
  destFilters: long ? [LONG,LONG,LONG,LONG]
                    : ['Delémont','Basel SBB','Olten','Biel/Bienne'],
  destFilterCount:4,
  sleepEnabled:false, sleepFallbackS:1234, sleepAfterS:987, sleepMaxMin:456,
  ledGpio:38, sdaGpio:17, sclGpio:18, oledAddr:'0x3D', oledInvertMin:47,
  ledOkColor:'#123456', ledDelaySmallColor:'#ABCDEF', ledDelayBigColor:'#0F0F0F',
  ledCancelledColor:'#FEDCBA', ledLoadingColor:'#778899', ledErrorBlinkMs:4321,
  delaySmallMin:7, delayBigMin:19,
  refreshNearSec:41, refreshMidSec:222, refreshFarSec:333, refreshVeryfarSec:1444,
  refreshNearMin:9, refreshMidMin:19, refreshFarMin:59,
  apiRetryCount:7, apiRetryDelayS:29, staleMaxMin:77, weekdaysOnly:false,
  weekendSleepEnabled:false, weekendStartDay:6, weekendStartH:21, weekendStartM:45,
  weekendEndDay:0, weekendEndH:9, weekendEndM:15 };
}

let cfg, saved, savedLen, clampMode = false;
const srv = http.createServer((q,s)=>{
  const j=o=>{s.writeHead(200,{'Content-Type':'application/json'});s.end(JSON.stringify(o));};
  if(q.url==='/'){s.writeHead(200,{'Content-Type':'text/html'});return s.end(fs.readFileSync(PANEL));}
  if(q.url==='/api/status') return j({wifi:true,apMode:false,ntp:true,ip:'10.0.0.5',rssi:-44,heapKb:130,
      uptimeS:60,time:'08:00:00',weekday:2,inWindow:false,runForever:true,activeUntilS:-1,lastError:''});
  if(q.url==='/api/departures') return j({ageS:-1,departures:[]});
  if(q.url==='/api/config'&&q.method==='GET') return j(cfg);
  if(q.url==='/api/config'&&q.method==='POST'){
    let b=''; q.on('data',c=>b+=c);
    q.on('end',()=>{
      saved=b; savedLen=Buffer.byteLength(b);
      let got; try { got=JSON.parse(b); } catch(e){ s.writeHead(400); return s.end('Invalid JSON'); }
      // wie handler_config_post: uebernehmen, dann sanitize
      Object.assign(cfg, got); delete cfg.password; delete cfg.panelPass;
      cfg.destFilterCount = (cfg.destFilters||[]).length;
      if (clampMode) { cfg.sleepMaxMin = 720; cfg.buttonGpio = 9; }   // Firmware klemmt ab
      j({ok:true});
    }); return;
  }
  s.writeHead(404); s.end();
}).listen(8098);

(async()=>{
  const exe=process.env.CHROMIUM_PATH;
  const br=await chromium.launch(exe?{executablePath:exe}:{});

  // --- A: Rundlauf ohne Berührung ---
  console.log('\n[A] Speichern ohne Änderung — bleibt alles erhalten?');
  cfg = makeCfg(false); saved=null; clampMode=false;
  let p=await br.newPage(); const errs=[]; p.on('pageerror',e=>errs.push(e.message));
  await p.goto('http://127.0.0.1:8098/'); await sleep(1500);
  const before = JSON.parse(JSON.stringify(cfg));
  await p.locator('#save-btn').click(); await sleep(1200);
  check('keine JS-Fehler', errs.length===0, errs.join(' | '));
  console.log('       POST-Body: '+savedLen+' Bytes');
  const got=JSON.parse(saved);
  const norm=v=>typeof v==='string'&&/^#[0-9a-fA-F]{6}$/.test(v)?v.toLowerCase():v;
  let lost=[];
  for(const k of Object.keys(before)){
    if(['panelAuthEnabled','destFilterCount'].includes(k)) continue;
    if(JSON.stringify(norm(before[k]))!==JSON.stringify(norm(got[k]))) lost.push(k+': '+JSON.stringify(before[k])+' → '+JSON.stringify(got[k]));
  }
  check('alle '+(Object.keys(before).length-2)+' Felder unverändert im POST', lost.length===0, lost.join(' | '));
  const toastA = await p.locator('#toast').textContent();
  check('meldet "gespeichert", keine Warnung', /gespeichert/i.test(toastA), toastA);
  await p.close();

  // --- B: Firmware klemmt Werte ab ---
  console.log('\n[B] Gerät passt Werte an — sieht man das?');
  cfg = makeCfg(false); saved=null; clampMode=true;
  p=await br.newPage(); await p.goto('http://127.0.0.1:8098/'); await sleep(1500);
  await p.locator('#save-btn').click(); await sleep(1500);
  const toastB = await p.locator('#toast').textContent();
  check('warnt vor angepassten Feldern', /angepasst/.test(toastB) && /sleepMaxMin/.test(toastB), toastB);
  check('Formular zeigt den echten Wert (720)',
        await p.locator('#sleepMaxMin').inputValue()==='720',
        await p.locator('#sleepMaxMin').inputValue());
  await p.close();

  // --- C: Body über der TCP-Segmentgrenze ---
  console.log('\n[C] Maximal langer Body (8 Fenster, lange Strings)');
  cfg = makeCfg(true); saved=null; clampMode=false;
  p=await br.newPage(); await p.goto('http://127.0.0.1:8098/'); await sleep(1500);
  await p.locator('#save-btn').click(); await sleep(1200);
  console.log('       POST-Body: '+savedLen+' Bytes');
  check('Body überschreitet ein TCP-Segment (~1460 B)', savedLen>1460, savedLen+' B');
  const gotC=JSON.parse(saved);
  check('Eingaben nicht laenger als die Firmware-Puffer',
        gotC.station.length<=63 && gotC.ssid.length<=63 &&
        gotC.destFilters.every(f=>f.length<=31),
        'station '+gotC.station.length+', ssid '+gotC.ssid.length);
  check('vollständig angekommen (8 Fenster, 4 Filter)',
        gotC.timeWindows.length===8 && gotC.destFilters.length===4,
        JSON.stringify(gotC.timeWindows.length)+'/'+JSON.stringify((gotC.destFilters||[]).length));
  check('lange Station unverkürzt', gotC.station===cfg.station, gotC.station);
  await p.close();

  await br.close(); srv.close();
  console.log(fails===0?'\nAlle Prüfungen bestanden.':'\n'+fails+' Prüfung(en) fehlgeschlagen.');
  process.exit(fails?1:0);
})();
