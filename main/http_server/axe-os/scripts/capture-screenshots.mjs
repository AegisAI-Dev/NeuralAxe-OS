#!/usr/bin/env node
/**
 * NeuralAxe Stability Lab — deterministic screenshot harness (Phase 2K).
 *
 * Drives the PRODUCTION build with puppeteer-core + the system Edge browser.
 * It does NOT wait for network-idle (the app polls continuously). Instead:
 *   - the synthetic /api and /version.txt responses are intercepted per scenario;
 *   - the live WebSocket has no upgrade handler and simply fails (app polls);
 *   - animations are disabled via an injected stylesheet;
 *   - navigation waits on 'domcontentloaded' + an explicit selector, then a
 *     short bounded delay before the capture.
 *
 * Every scenario's rendered text is collected and scanned for the sensitive
 * device identifiers seeded into the fixtures — proving the Lab never surfaces
 * an IP / SSID / pool / wallet / Wi-Fi password.
 */
import puppeteer from 'puppeteer-core';
import http from 'node:http';
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, extname, join } from 'node:path';

const AXE_OS_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DIST = join(AXE_OS_DIR, 'dist', 'axe-os');
const OUT = process.env.NX_SHOT_DIR
  || 'D:/Companys/Neuralshield/Firmware/NeuralAxe Build Artifacts/stability-lab-v0.1.0-dev-board601/screenshots';
const EDGE = process.env.EDGE_PATH || process.env.CHROME_BIN
  || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const PORT = 4319;
const BASE = `http://localhost:${PORT}`;

if (!existsSync(DIST)) { console.error(`Missing build at ${DIST}. Run: npx ng build --configuration=production`); process.exit(1); }
mkdirSync(OUT, { recursive: true });

// ---- sensitive identifiers seeded into fixtures (must never render) ----
const SECRET = {
  ip: '192.168.7.77', ssid: 'HomeSSID-Private', wifiPass: 'sup3rsecretpass',
  pool: 'pool.secret-example.com', wallet: 'bc1qWALLETsecretxxxxxxxxxxxxxxxx',
};

const baseInfo = (over = {}) => ({
  productName: 'NeuralAxe OS', productVersion: '0.1.0-dev', vendor: 'NeuralShield',
  buildChannel: 'development', upstreamProject: 'ESP-Miner / AxeOS', upstreamVersion: 'v2.14.2',
  targetDevice: 'Gamma', targetBoard: '601', targetAsic: 'BM1370', ASICModel: 'BM1370', boardVersion: '602',
  version: 'v2.14.2-31-g1c411d52', axeOSVersion: 'v2.14.2-31-g1c411d52', idfVersion: 'v5.5.1',
  frequency: 625, actualFrequency: 625, coreVoltage: 1150, coreVoltageActual: 1141,
  thermalControlMode: 'curve', temptarget: 60, minFanSpeed: 25, manualFanSpeed: 70, fanCurveHysteresis: 2,
  fanCurve: [{ tempC: 45, fanPercent: 25 }, { tempC: 52, fanPercent: 45 }, { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 }],
  temp: 60, temp2: 59, vrTemp: 45, controlSensor: 'asic', controlSensorValid: 1, effectiveControlTemperature: 60,
  emergencyOverrideActive: 0, overheat_mode: 0, hysteresisHolding: 0, activeCurveSegment: 2,
  requestedFanPercent: 55, appliedFanPercent: 55, autofanspeed: 1, fanspeed: 55, fanrpm: 15230, fan2rpm: 14980,
  miningPaused: false, hashRate: 1290, hashRate_1m: 1288, hashRate_10m: 1285, hashRate_1h: 1280, expectedHashrate: 1275,
  power: 21.4, maxPower: 25, voltage: 5208, current: 2237, nominalVoltage: 5, errorPercentage: 0.4,
  sharesAccepted: 18760, sharesRejected: 92, sharesRejectedReasons: [], responseTime: 34, poolDifficulty: 1000,
  bestDiff: 238214491, bestSessionDiff: 21212121, networkDifficulty: 155970000000000, blockHeight: 842763, blockFound: 0,
  uptimeSeconds: 218520, wifiRSSI: -42, wifiStatus: 'Connected!', freeHeap: 200504, freeHeapInternal: 200504, cpuUsage: 18,
  hostname: 'gamma-lab-01', isUsingFallbackStratum: 0, hashrateMonitor: { asics: [{ total: 1290, domains: [321, 329, 318, 322], errorCount: 2 }], hashrate: 1290 },
  // sensitive fields that MUST NOT surface anywhere in the Lab:
  ipv4: SECRET.ip, ipv6: 'fe80::1', macAddr: '2C:54:91:88:C9:E3', ssid: SECRET.ssid, wifiPass: SECRET.wifiPass,
  stratumURL: SECRET.pool, stratumUser: SECRET.wallet, stratumPort: 3333,
  fallbackStratumURL: '', fallbackStratumUser: '',
  ...over,
});

const asicFixture = {
  ASICModel: 'BM1370', deviceModel: 'Gamma', swarmColor: 'purple', asicCount: 1,
  defaultFrequency: 485, frequencyOptions: [400, 425, 450, 475, 485, 500, 525, 550, 575, 600, 625],
  defaultVoltage: 1200, voltageOptions: [1100, 1150, 1200, 1250, 1300],
};

// ---- tiny static server for the production build ----
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.pdf': 'application/pdf' };
const server = http.createServer((req, res) => {
  try {
    let p = decodeURIComponent(req.url.split('?')[0]);
    if (p === '/' || !extname(p)) p = '/index.html';
    const file = join(DIST, p);
    if (!file.startsWith(DIST) || !existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(readFileSync(file));
  } catch { res.writeHead(500); res.end('err'); }
});
await new Promise(r => server.listen(PORT, r));

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const collected = [];

/** Wire per-scenario request interception. */
async function intercept(page, { info, offline, versionTxt }) {
  await page.setRequestInterception(true);
  page.on('request', (req) => {
    const url = req.url();
    const json = (body) => req.respond({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    try {
      if (url.includes('/api/system/info')) return offline ? req.abort() : json(info);
      if (url.includes('/api/system/asic')) return json(asicFixture);
      if (url.includes('/version.txt')) return req.respond({ status: 200, contentType: 'text/plain', body: versionTxt ?? info.version });
      if (url.includes('/api/system/statistics')) return json({ currentTimestamp: 0, labels: ['hashrate', 'timestamp'], statistics: [] });
      if (url.includes('/api/system')) return json({ message: 'ok' }); // PATCH/restart/pause/resume
      return req.continue();
    } catch { try { req.continue(); } catch { /* already handled */ } }
  });
}

async function newPage(browser, scenario, { mobile = false } = {}) {
  const page = await browser.newPage();
  await page.setViewport(mobile ? { width: 390, height: 844 } : { width: 1440, height: 900 });
  await intercept(page, scenario);
  // Reset localStorage (the browser origin is shared across scenarios, so a
  // leftover NX_STABILITY_ACTIVE flag would otherwise leak an "interrupted"
  // banner) and seed BEFORE the app boots.
  await page.evaluateOnNewDocument((seed) => {
    try {
      window.localStorage.clear();
      for (const [k, v] of Object.entries(seed)) window.localStorage.setItem(k, v);
    } catch { /* ignore */ }
  }, scenario.seed || {});
  return page;
}

async function ready(page, selector = '.nx-page-title, .nx-lab-card') {
  await page.waitForSelector(selector, { timeout: 15000 }).catch(() => {});
  await page.addStyleTag({ content: '*,*::before,*::after{animation:none !important;transition:none !important;} .nx-live-dot{animation:none !important;}' }).catch(() => {});
  await sleep(700);
}

async function shot(page, name) {
  const path = join(OUT, `${name}.png`);
  await page.screenshot({ path, fullPage: true }).catch(async () => { await page.screenshot({ path }); });
  const text = await page.evaluate(() => document.body.innerText).catch(() => '');
  collected.push({ name, text });
  console.log(`  ✓ ${name}.png`);
}

// ---- a seeded, sanitized history record (completed + partial + aborted) ----
function seededHistory() {
  const mk = (id, name, status, over = {}) => ({
    profileId: id, profileName: name, status, thermalControlMode: 'curve',
    requestedMeasureMs: 1200000, measuredMeasureMs: status === 'partial' ? 300000 : 1200000,
    validSamples: status === 'partial' ? 60 : 240, missingSamples: 0, expectedSamples: 240,
    coveragePct: status === 'partial' ? 25 : 100, restartOccurred: false, countersReset: false, abortReason: over.abortReason || null,
    avgHashrate: over.hr ?? 1290, medianHashrate: over.hr ?? 1288, hashrateVariabilityPct: 1.4,
    avgPower: over.pw ?? 21.4, avgEfficiency: over.eff ?? 16.6, peakAsicTemp: over.temp ?? 62, avgAsicTemp: 60,
    peakVrmTemp: 47, avgVrmTemp: 45, avgRequestedFan: 55, avgAppliedFan: 56, fanSaturationMs: 0, avgErrorRate: 0.4,
    acceptedShareDelta: 420, rejectedShareDelta: 2, rejectRatePct: 0.47, poolLatencyAvg: 34, poolLatencyPeak: 61,
    badges: over.badges || [{ kind: 'completed', label: 'Completed', severity: 'ok' }],
  });
  const record = {
    id: 'nx-lab-demo-0001', startedAt: Date.now() - 3900000, finishedAt: Date.now() - 300000,
    device: { productName: 'NeuralAxe OS', productVersion: '0.1.0-dev', targetDevice: 'Gamma', targetBoard: '601', targetAsic: 'BM1370', firmware: 'v2.14.2-31-g1c411d52' },
    hostname: 'gamma-lab-01',
    original: { frequency: 625, coreVoltage: 1150, thermalControlMode: 'curve', minFanSpeed: 25, fanCurveHysteresis: 2, fanCurve: [{ tempC: 45, fanPercent: 25 }, { tempC: 52, fanPercent: 45 }, { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 }] },
    profiles: [
      { name: 'Current Configuration', config: { frequency: 625, coreVoltage: 1150, thermalControlMode: 'curve' }, warmupSec: 180, measureSec: 1200, cooldownSec: 0 },
      { name: 'Performance', config: { frequency: 650, coreVoltage: 1200, thermalControlMode: 'curve' }, warmupSec: 180, measureSec: 1200, cooldownSec: 0 },
    ],
    thresholds: { asicC: 68, vrmC: 100, errorPct: 5, rejectPct: 8, fanSaturationStop: false, fanSaturationPct: 100, debounceSamples: 3 },
    results: [
      mk('p1', 'Current Configuration', 'completed', { hr: 1290, eff: 16.6, temp: 61, badges: [{ kind: 'completed', label: 'Completed', severity: 'ok' }, { kind: 'lowest-temp', label: 'Lowest temperature', severity: 'ok' }] }),
      mk('p2', 'Performance', 'completed', { hr: 1355, eff: 17.1, temp: 64, badges: [{ kind: 'completed', label: 'Completed', severity: 'ok' }, { kind: 'highest-hashrate', label: 'Highest hashrate', severity: 'ok' }] }),
      mk('p3', 'Aggressive', 'partial', { hr: 1360, eff: 17.8, temp: 66, badges: [{ kind: 'partial', label: 'Partial', severity: 'warn' }] }),
    ],
    timeline: [{ state: 'idle', at: 0, note: 'Session not started' }, { state: 'complete', at: 1, note: 'Original configuration restored' }],
    finalState: 'complete', reason: null, restoreResult: 'ok',
  };
  return record;
}

// ---- drive helpers (DOM only; production build has no ng debug tools) ----
const clickByText = (page, sel, re) => page.evaluate((sel, reSrc) => {
  const rx = new RegExp(reSrc); const el = [...document.querySelectorAll(sel)].find(e => rx.test(e.textContent || ''));
  if (el) { el.click(); return true; } return false;
}, sel, re.source);

async function addStarters(page) {
  await page.evaluate(() => {
    const b = [...document.querySelectorAll('.nx-lab-starter')];
    b.find(x => /Conservative/.test(x.textContent))?.click();
    b.find(x => /Performance/.test(x.textContent))?.click();
    b.find(x => /Current Configuration/.test(x.textContent))?.click();
  });
  await sleep(300);
}

/** Fill the editor (name + short durations) and add one profile to the queue. */
async function fillEditorAndQueue(page, { name, warmup, measure, cooldown }) {
  await page.evaluate((vals) => {
    const set = (el, v) => { if (!el) return; el.value = String(v); el.dispatchEvent(new Event('input', { bubbles: true })); };
    const fields = [...document.querySelectorAll('.nx-lab-field')];
    const byLabel = (re) => fields.find(f => re.test(f.querySelector('label')?.textContent || ''));
    set(document.querySelector('.nx-lab-editor input[type="text"]'), vals.name);
    set(byLabel(/Warm-up/)?.querySelector('input'), vals.warmup);
    set(byLabel(/Measurement/)?.querySelector('input'), vals.measure);
    set(byLabel(/Cooldown/)?.querySelector('input'), vals.cooldown);
  }, { name, warmup, measure, cooldown });
  await sleep(300);
  await clickByText(page, '.nx-lab-actions button, button', /Add to queue/);
  await sleep(300);
}

/** Add a queued profile, run preflight, acknowledge, and start the session. */
async function startSession(page) {
  await clickByText(page, 'button', /Run preflight/); await sleep(500);
  await page.evaluate(() => document.querySelectorAll('.nx-lab-acks .p-checkbox-box').forEach(b => b.click()));
  await sleep(200);
  await clickByText(page, '.p-dialog button', /START STABILITY SESSION/);
  await sleep(1000);
}

// =====================================================================
async function main() {
  const browser = await puppeteer.launch({ executablePath: EDGE, headless: 'new', args: ['--no-sandbox', '--disable-gpu', '--window-size=1440,900'] });
  const LAB = `${BASE}/#/stability-lab`;
  const nav = async (page) => { await page.goto(LAB, { waitUntil: 'domcontentloaded' }).catch(() => {}); await ready(page); };

  try {
    // 1. landing / supported preflight PASS
    { const p = await newPage(browser, { info: baseInfo() }); await nav(p); await shot(p, '01-landing-supported-preflight-pass'); await p.close(); }

    // 3. blocked unsupported board 702
    { const p = await newPage(browser, { info: baseInfo({ targetBoard: '702', boardVersion: '702' }) }); await nav(p); await shot(p, '03-blocked-board-702'); await p.close(); }

    // 4. blocked stock AxeOS
    { const p = await newPage(browser, { info: baseInfo({ productName: undefined, vendor: undefined, targetBoard: undefined, targetAsic: undefined, targetDevice: undefined }) }); await nav(p); await shot(p, '04-blocked-stock-axeos'); await p.close(); }

    // invalid sensor / emergency / pair mismatch / offline (blocked preflight states)
    { const p = await newPage(browser, { info: baseInfo({ controlSensorValid: 0 }) }); await nav(p); await addStarters(p); await shot(p, '05a-blocked-invalid-sensor'); await p.close(); }
    { const p = await newPage(browser, { info: baseInfo({ emergencyOverrideActive: 1 }) }); await nav(p); await addStarters(p); await shot(p, '05b-blocked-emergency'); await p.close(); }
    { const p = await newPage(browser, { info: baseInfo(), versionTxt: 'v2.14.2-30-gDEADBEEF' }); await nav(p); await addStarters(p); await shot(p, '05c-blocked-pair-mismatch'); await p.close(); }
    { const p = await newPage(browser, { info: baseInfo(), offline: true }); await nav(p); await shot(p, '05d-offline-no-telemetry'); await p.close(); }

    // 5. profile queue + 6. diff
    { const p = await newPage(browser, { info: baseInfo() }); await nav(p); await addStarters(p); await shot(p, '06-profile-queue-and-diff'); await p.close(); }

    // 12. thresholds
    { const p = await newPage(browser, { info: baseInfo() }); await nav(p); await addStarters(p); await shot(p, '12-stop-thresholds'); await p.close(); }

    // 7. confirmation dialog
    {
      const p = await newPage(browser, { info: baseInfo() }); await nav(p); await addStarters(p);
      await clickByText(p, 'button', /Run preflight/); await sleep(600);
      await p.evaluate(() => document.querySelectorAll('.nx-lab-acks .p-checkbox-box').forEach(b => b.click()));
      await sleep(300); await shot(p, '07-confirmation-dialog'); await p.close();
    }

    // 10/11/13/14. live session: warm-up, then automatic reject abort → aborted result
    {
      const p = await newPage(browser, { info: baseInfo({ sharesAccepted: 5, sharesRejected: 40 }) }); // ~89% reject → will auto-abort
      await nav(p); await addStarters(p);
      await clickByText(p, 'button', /Run preflight/); await sleep(500);
      await p.evaluate(() => document.querySelectorAll('.nx-lab-acks .p-checkbox-box').forEach(b => b.click()));
      await clickByText(p, '.p-dialog button', /START STABILITY SESSION/); await sleep(1200);
      await shot(p, '10-applying-warmup'); // first seconds → applying/warm-up + live telemetry + charts
      // owner abort confirmation
      await clickByText(p, '.nx-lab-active button', /Abort/); await sleep(500);
      await shot(p, '13-abort-confirmation');
      await p.evaluate(() => { const b = [...document.querySelectorAll('.p-dialog button')].find(x => /Keep running/.test(x.textContent)); b && b.click(); });
      await sleep(1000);
      // let the reject-rate stop fire (debounce 3 × 5 s)
      await sleep(17000);
      await shot(p, '14-automatic-abort-result'); await p.close();
    }

    // 11/12. measuring window with charts + ASIC temp near the stop threshold
    {
      const p = await newPage(browser, { info: baseInfo({ temp: 66, temp2: 65, vrTemp: 55, sharesRejected: 40, sharesAccepted: 18760 }) });
      await nav(p); await fillEditorAndQueue(p, { name: 'Near-threshold', warmup: 30, measure: 300, cooldown: 0 });
      await startSession(p); await sleep(33000); // past warm-up → measuring
      await shot(p, '11-measuring-with-charts-threshold'); await p.close();
    }

    // 15/18. a real healthy completed session → results + promote confirmation
    {
      const p = await newPage(browser, { info: baseInfo({ sharesRejected: 12, sharesAccepted: 18760 }) });
      await nav(p); await fillEditorAndQueue(p, { name: 'Completed run', warmup: 30, measure: 60, cooldown: 0 });
      await startSession(p);
      // warm-up 30 + measure 60 + restore; poll until the results grid appears.
      for (let i = 0; i < 30 && !(await p.$('.nx-lab-result')); i++) await sleep(4000);
      await sleep(800);
      await shot(p, '15-completed-live-results');
      await clickByText(p, '.nx-lab-result-actions button', /Promote/); await sleep(700);
      await shot(p, '18-promote-confirmation'); await p.close();
    }

    // 21b. mobile active session (warm-up)
    {
      const p = await newPage(browser, { info: baseInfo({ sharesRejected: 12, sharesAccepted: 18760 }) }, { mobile: true });
      await nav(p); await fillEditorAndQueue(p, { name: 'Mobile run', warmup: 300, measure: 300, cooldown: 0 });
      await startSession(p); await sleep(1500);
      await shot(p, '21b-mobile-active-session'); await p.close();
    }

    // 16/18/17/19/20. seeded history → view (completed comparison + partial), export buttons
    {
      const p = await newPage(browser, { info: baseInfo(), seed: { NX_STABILITY_SESSIONS: JSON.stringify([seededHistory()]) } });
      await nav(p); await shot(p, '19-history');
      await clickByText(p, '.nx-lab-hitem .nx-chip', /View/); await sleep(500);
      await shot(p, '16-completed-comparison-and-partial'); await p.close();
    }

    // 23. privacy mode ON (default) shows masked identifiers only
    { const p = await newPage(browser, { info: baseInfo(), seed: { SENSITIVE_DATA_HIDDEN: 'true', NX_STABILITY_SESSIONS: JSON.stringify([seededHistory()]) } }); await nav(p); await shot(p, '23-privacy-mode'); await p.close(); }

    // 24. red-accent semantic proof (accent red, preflight PASS checks stay green)
    {
      const p = await newPage(browser, { info: baseInfo() }); await nav(p); await addStarters(p);
      await p.evaluate(() => document.documentElement.style.setProperty('--primary-color', '#ef4444'));
      await sleep(300); await shot(p, '24-red-accent-semantic-proof'); await p.close();
    }

    // 21/22. mobile active + mobile results
    { const p = await newPage(browser, { info: baseInfo() }, { mobile: true }); await nav(p); await addStarters(p); await shot(p, '21-mobile-queue'); await p.close(); }
    { const p = await newPage(browser, { info: baseInfo(), seed: { NX_STABILITY_SESSIONS: JSON.stringify([seededHistory()]) } }, { mobile: true }); await nav(p); await clickByText(p, '.nx-lab-hitem .nx-chip', /View/); await sleep(500); await shot(p, '22-mobile-result-comparison'); await p.close(); }

  } finally {
    await browser.close();
    server.close();
  }

  // ---- privacy scan across every captured screen ----
  const leaks = [];
  for (const { name, text } of collected) {
    for (const [k, v] of Object.entries(SECRET)) {
      if (v && text.includes(v)) leaks.push({ name, field: k, value: v });
    }
  }
  const report = {
    generatedAt: new Date().toISOString(),
    screenshots: collected.map(c => c.name),
    sensitiveValuesSearched: SECRET,
    leaks,
    privacyScan: leaks.length === 0 ? 'PASS — no sensitive identifier surfaced in any screen' : 'FAIL',
  };
  writeFileSync(join(OUT, 'privacy-scan.json'), JSON.stringify(report, null, 2));
  console.log(`\nCaptured ${collected.length} screens → ${OUT}`);
  console.log(`Privacy scan: ${report.privacyScan}${leaks.length ? ' ' + JSON.stringify(leaks) : ''}`);
  process.exit(leaks.length ? 2 : 0);
}

main().catch((e) => { console.error(e); server.close(); process.exit(1); });
