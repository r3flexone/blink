// Panel-Smoke-Test gegen den Mock-ESP — siehe README.md in diesem Ordner.
const { chromium } = require('playwright');
const { spawn } = require('child_process');

const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0;
function check(name, cond, extra) {
  console.log((cond ? '  OK   ' : '  FAIL ') + name + (cond ? '' : '  → ' + extra));
  if (!cond) fails++;
}

async function withServer(mode, fn) {
  const srv = spawn('node', ['mock-esp.js'], { env: {...process.env, MOCK_MODE: mode}, cwd: __dirname });
  await sleep(600);
  try { await fn(); } finally { srv.kill(); await sleep(200); }
}

(async () => {
  // CHROMIUM_PATH nur nötig, wenn Playwright seinen Browser nicht selbst findet
  const exe = process.env.CHROMIUM_PATH;
  const browser = await chromium.launch(exe ? { executablePath: exe } : {});

  // ---- Fall 1: Gerät erreichbar, Config lädt ----
  await withServer('ok', async () => {
    console.log('\n[1] Gerät erreichbar');
    const page = await browser.newPage();
    const errs = [];
    page.on('pageerror', e => errs.push(e.message));
    page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
    await page.goto('http://127.0.0.1:8099/');
    await sleep(1200);

    check('keine JS-Fehler', errs.length === 0, errs.join(' | '));
    check('Speichern freigegeben', !(await page.locator('#save-btn').isDisabled()));
    check('Station geladen', await page.locator('#station').inputValue() === 'Basel SBB');
    check('2 Zeitfenster gerendert', await page.locator('.tw-entry').count() === 2);
    check('Filter-Lücke erhalten', (await page.locator('#filt0').inputValue()) === ''
                                && (await page.locator('#filt1').inputValue()) === 'Liestal');

    await page.locator('.nav-btn').nth(8).click();   // Status
    await sleep(600);
    const clock = await page.locator('#clock').textContent();
    check('Uhr kommt vom Gerät (06:52:xx)', /^06:52:\d\d$/.test(clock), clock);
    const until = await page.locator('#active-until').textContent();
    check('Rest-Aktivzeit angezeigt', /8 min/.test(until), until);
    const net = await page.locator('#status-net').textContent();
    check('IP + RSSI angezeigt', /192\.168\.1\.42/.test(net) && /-58 dBm/.test(net), net);
    const dev = await page.locator('#status-dev').textContent();
    check('Uptime + Heap angezeigt', /1 h 2 min/.test(dev) && /142 KB/.test(dev), dev);
    const banner = await page.locator('#err-banner').textContent();
    check('Fehlergrund als Banner', /falsch geschrieben/.test(banner)
          && await page.locator('#err-banner').isVisible(), banner);
    check('Abfahrten gerendert', (await page.locator('#oled-rows .oled-row').count()) === 2);

    // Dirty-Marker
    check('anfangs nicht dirty', !(await page.locator('#save-btn').evaluate(e => e.classList.contains('dirty'))));
    await page.locator('.nav-btn').nth(3).click();
    await page.locator('#station').fill('Bern');
    await sleep(200);
    check('nach Eingabe dirty', await page.locator('#save-btn').evaluate(e => e.classList.contains('dirty')));

    // Speichern
    await page.locator('#save-btn').click();
    await sleep(600);
    const body = await (await fetch('http://127.0.0.1:8099/_saved')).json();
    // Der Mock lehnt wie die Firmware ohne application/json bzw. mit fremdem
    // Origin ab (CSRF-Schutz). body.body bleibt dann null.
    check('POST erfüllt den CSRF-Riegel des Geräts', body.body !== null && !body.reject,
          'abgelehnt wegen: ' + body.reject);
    const sent = JSON.parse(body.body);
    check('POST enthält echte Werte', sent.station === 'Bern' && sent.sleepMaxMin === 90
          && sent.timeWindows.length === 2, JSON.stringify(sent).slice(0,160));
    check('Filterpositionen im POST erhalten',
          JSON.stringify(sent.destFilters) === '["","Liestal"]', JSON.stringify(sent.destFilters));
    check('nach Speichern nicht mehr dirty',
          !(await page.locator('#save-btn').evaluate(e => e.classList.contains('dirty'))));
    await page.close();
  });

  // ---- Fall 2: /api/config schlägt fehl (Gerät schlief beim Laden) ----
  await withServer('cfgfail', async () => {
    console.log('\n[2] Config-Laden schlägt fehl');
    const page = await browser.newPage();
    const dialogs = [];
    page.on('dialog', d => { dialogs.push(d.message()); d.dismiss(); });
    await page.goto('http://127.0.0.1:8099/');
    await sleep(1200);

    check('Speichern gesperrt', await page.locator('#save-btn').isDisabled());
    await page.locator('#save-btn').click({ force: true }).catch(()=>{});
    await sleep(400);
    const saved = await (await fetch('http://127.0.0.1:8099/_saved')).json();
    check('kein POST abgesetzt', saved.body === null, String(saved.body).slice(0,120));
    await page.close();
  });

  // ---- Fall 3: Gerät wacht nach fehlgeschlagenem Laden wieder auf ----
  await withServer('cfgfail', async () => {
    console.log('\n[3] Gerät wird nachträglich erreichbar');
    const page = await browser.newPage();
    const errs = [];
    page.on('pageerror', e => errs.push(e.message));
    await page.goto('http://127.0.0.1:8099/');
    await sleep(800);
    check('zunächst gesperrt', await page.locator('#save-btn').isDisabled());
    await fetch('http://127.0.0.1:8099/_mode?m=ok');
    await page.waitForFunction(() => !document.getElementById('save-btn').disabled, null, {timeout: 12000});
    check('nach Aufwachen automatisch entsperrt', !(await page.locator('#save-btn').isDisabled()));
    check('Config nachgeladen', await page.locator('#station').inputValue() === 'Basel SBB');
    check('keine JS-Fehler', errs.length === 0, errs.join(' | '));
    await page.locator('.nav-btn').nth(8).click();
    await sleep(700);
    await page.close();
  });

  await browser.close();
  console.log(fails === 0 ? '\nAlle Prüfungen bestanden.' : `\n${fails} Prüfung(en) fehlgeschlagen.`);
  process.exit(fails === 0 ? 0 : 1);
})();
