#!/usr/bin/env node
/**
 * NeuralAxe Bitcoin Block Intelligence — deterministic screenshot harness (Phase 2L).
 *
 * Drives the PRODUCTION build with puppeteer-core + the system Edge browser and
 * intercepts BOTH the device /api and the external block-data provider requests
 * (mempool.space / blockstream.info) with deterministic block fixtures — no real
 * network, no real miner identifiers. Every captured screen's text is scanned for
 * the sensitive device identifiers seeded into the device info, proving the block
 * workspace never leaks wallet / worker / SSID / Wi-Fi password / IP.
 */
import puppeteer from 'puppeteer-core';
import http from 'node:http';
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, extname, join } from 'node:path';

const AXE_OS_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DIST = join(AXE_OS_DIR, 'dist', 'axe-os');
const OUT = process.env.NX_SHOT_DIR
  || 'D:/Companys/Neuralshield/Firmware/NeuralAxe Build Artifacts/block-intelligence-v0.1.0-dev-board601/screenshots';
const EDGE = process.env.EDGE_PATH || process.env.CHROME_BIN
  || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const PORT = 4321;
const BASE = `http://localhost:${PORT}`;

if (!existsSync(DIST)) { console.error(`Missing build at ${DIST}. Run: npx ng build --configuration=production`); process.exit(1); }
mkdirSync(OUT, { recursive: true });

// Sensitive identifiers seeded into device info — must NEVER surface on a block screen.
const SECRET = {
  ip: '192.168.7.77', ssid: 'HomeSSID-Private', wifiPass: 'sup3rsecretpass',
  wallet: 'bc1qWALLETsecretxxxxxxxxxxxxxxxx', worker: 'rig-secret-01',
};

// Device info: real pool HOSTS (used host-only for local matching) + seeded secrets.
const baseInfo = (over = {}) => ({
  productName: 'NeuralAxe OS', productVersion: '0.1.0-dev', vendor: 'NeuralShield',
  buildChannel: 'development', upstreamProject: 'ESP-Miner / AxeOS', upstreamVersion: 'v2.14.2',
  targetDevice: 'Gamma', targetBoard: '601', targetAsic: 'BM1370', ASICModel: 'BM1370', boardVersion: '602',
  version: 'v2.14.2-37-gABCDEF0', axeOSVersion: 'v2.14.2-37-gABCDEF0', idfVersion: 'v5.5.3',
  frequency: 625, actualFrequency: 625, coreVoltage: 1150, coreVoltageActual: 1141,
  thermalControlMode: 'curve', temptarget: 60, minFanSpeed: 25, manualFanSpeed: 70, fanCurveHysteresis: 2,
  fanCurve: [{ tempC: 45, fanPercent: 25 }, { tempC: 52, fanPercent: 45 }, { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 }],
  temp: 60, temp2: 59, vrTemp: 45, controlSensor: 'asic', controlSensorValid: 1, effectiveControlTemperature: 60,
  emergencyOverrideActive: 0, overheat_mode: 0, hysteresisHolding: 0, activeCurveSegment: 2,
  requestedFanPercent: 55, appliedFanPercent: 55, autofanspeed: 1, fanspeed: 55, fanrpm: 15230, fan2rpm: 14980,
  miningPaused: false, hashRate: 1290, hashRate_1m: 1288, hashRate_10m: 1285, hashRate_1h: 1280, expectedHashrate: 1275,
  power: 21.4, maxPower: 25, voltage: 5208, current: 2237, nominalVoltage: 5, errorPercentage: 0.4,
  sharesAccepted: 18760, sharesRejected: 92, sharesRejectedReasons: [], responseTime: 34, poolDifficulty: 1000,
  bestDiff: 238214491, bestSessionDiff: 21212121, networkDifficulty: 155970000000000, blockHeight: 870001, blockFound: 0,
  uptimeSeconds: 218520, wifiRSSI: -42, wifiStatus: 'Connected!', freeHeap: 200504, freeHeapInternal: 200504, cpuUsage: 18,
  hostname: 'gamma-lab-01', isUsingFallbackStratum: 0, hashrateMonitor: { asics: [{ total: 1290, domains: [321, 329, 318, 322], errorCount: 2 }], hashrate: 1290 },
  // configured pools — real HOSTS used for local matching (not secret):
  stratumURL: 'public-pool.io', stratumPort: 21496, fallbackStratumURL: 'solo.ckpool.org', fallbackStratumPort: 3333,
  // seeded secrets that MUST NOT surface on a block screen:
  ipv4: SECRET.ip, ipv6: 'fe80::1', macAddr: '2C:54:91:88:C9:E3', ssid: SECRET.ssid, wifiPass: SECRET.wifiPass,
  stratumUser: `${SECRET.wallet}.${SECRET.worker}`, fallbackStratumUser: `${SECRET.wallet}.${SECRET.worker}`,
  ...over,
});

const asicFixture = {
  ASICModel: 'BM1370', deviceModel: 'Gamma', swarmColor: 'purple', asicCount: 1,
  defaultFrequency: 485, frequencyOptions: [400, 425, 450, 475, 485, 500, 525, 550, 575, 600, 625],
  defaultVoltage: 1200, voltageOptions: [1100, 1150, 1200, 1250, 1300],
};

// A realistic coinbase scriptsig hex: binary prefix + printable pool tag + binary suffix.
const cbHex = (tag) => [0x03, 0x87, 0x9a, 0x0e, 0x00, ...Array.from(tag).map(c => c.charCodeAt(0)), 0x00, 0xff]
  .map(b => b.toString(16).padStart(2, '0')).join('');

// ---- deterministic mempool.space-shaped block fixtures -------------------
const BLOCK_SPECS = [
  { tag: 'foundry',   pool: { id: 111, name: 'Foundry USA', slug: 'foundryusa' }, cb: '/Foundry USA Pool/' },
  { tag: 'antpool',   pool: { id: 0, name: 'Unknown', slug: 'unknown' },          cb: 'AntPool/mined' },
  { tag: 'public',    pool: { id: 300, name: 'Public Pool', slug: 'public-pool' }, cb: 'public-pool.io' },
  { tag: 'ckpool',    pool: { id: 400, name: 'Solo CKPool', slug: 'solock' },      cb: 'ckpool.eu/solo.ckpool.org' },
  { tag: 'slush',     pool: { id: 5, name: 'SlushPool', slug: 'slushpool' },       cb: null },
  { tag: 'viabtc',    pool: { id: 6, name: 'ViaBTC', slug: 'viabtc' },             cb: 'viabtc/' },
  { tag: 'unknown',   pool: { id: 0, name: 'Unknown', slug: 'unknown' },           cb: 'random-miner-9931' },
  { tag: 'unattr',    pool: { id: 0, name: 'Unknown', slug: 'unknown' },           cb: null },
  { tag: 'f2pool',    pool: { id: 7, name: 'F2Pool', slug: 'f2pool' },             cb: '?f2pool?' },
  { tag: 'mara',      pool: { id: 8, name: 'MARA Pool', slug: 'marapool' },        cb: 'MARA Pool' },
  { tag: 'luxor',     pool: { id: 9, name: 'Luxor', slug: 'luxor' },               cb: 'Luxor Tech' },
  { tag: 'binance',   pool: { id: 10, name: 'Binance Pool', slug: 'binancepool' }, cb: 'Binance/' },
];

function makeBlocks(tipHeight = 870000, { tipHashSuffix = 'a', tipHeightBump = 0 } = {}) {
  const nowSec = Math.floor(Date.now() / 1000);
  const top = tipHeight + tipHeightBump;
  return BLOCK_SPECS.map((spec, i) => {
    const height = top - i;
    const isTip = i === 0;
    return {
      id: `000000000000000000${spec.tag}${height}${isTip ? tipHashSuffix : ''}`,
      height, version: 0x20000000,
      timestamp: nowSec - 120 - i * 600,
      bits: 386000000, nonce: 123456789 + i, difficulty: 90000000000000,
      merkle_root: 'aa'.repeat(32),
      tx_count: 3200 - i * 37, size: 1500000 - i * 1000, weight: 3990000 - i * 2000,
      previousblockhash: `000000000000000000prev${height - 1}`,
      mediantime: nowSec - 300 - i * 600, stale: false,
      extras: {
        reward: 315000000 - i * 100000, totalFees: 2500000 - i * 90000,
        coinbaseSignatureAscii: spec.cb, coinbaseRaw: spec.cb ? cbHex(spec.cb) : null,
        pool: spec.pool, matchRate: 100,
      },
    };
  });
}

// ---- tiny static server for the production build ----
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png' };
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
const CORS = { 'Access-Control-Allow-Origin': '*', 'Content-Type': 'application/json' };

async function intercept(page, scenario) {
  await page.setRequestInterception(true);
  page.on('request', (req) => {
    const url = req.url();
    const method = req.method();
    const json = (body, headers = { 'Content-Type': 'application/json' }) => req.respond({ status: 200, headers, body: JSON.stringify(body) });
    try {
      if (method === 'OPTIONS') return req.respond({ status: 204, headers: { ...CORS, 'Access-Control-Allow-Methods': 'GET,OPTIONS', 'Access-Control-Allow-Headers': '*' } });
      // External provider — recent blocks
      if (url.includes('mempool.space/api/v1/blocks') || url.includes('blockstream.info/api/blocks')) {
        return scenario.blocksOffline ? req.abort() : req.respond({ status: 200, headers: CORS, body: JSON.stringify(scenario.blocks) });
      }
      // External provider — single block detail
      if (url.includes('mempool.space/api/v1/block/') || url.includes('blockstream.info/api/block/')) {
        const hash = decodeURIComponent(url.split('/').pop());
        const b = (scenario.blocks || []).find(x => x.id === hash);
        return b ? req.respond({ status: 200, headers: CORS, body: JSON.stringify(b) }) : req.respond({ status: 404, headers: CORS, body: '{}' });
      }
      // Device API
      if (url.includes('/api/system/info')) return scenario.deviceOffline ? req.abort() : json(scenario.info);
      if (url.includes('/api/system/asic')) return json(asicFixture);
      if (url.includes('/version.txt')) return req.respond({ status: 200, headers: { 'Content-Type': 'text/plain' }, body: scenario.info.version });
      if (url.includes('/api/system/statistics')) return json({ currentTimestamp: 0, labels: ['hashrate', 'timestamp'], statistics: [] });
      if (url.includes('/api/system')) return json({ message: 'ok' });
      return req.continue();
    } catch { try { req.continue(); } catch { /* handled */ } }
  });
}

async function newPage(browser, scenario, { mobile = false } = {}) {
  const page = await browser.newPage();
  await page.setViewport(mobile ? { width: 390, height: 844 } : { width: 1440, height: 1200 });
  await intercept(page, scenario);
  await page.evaluateOnNewDocument((seed) => {
    try { window.localStorage.clear(); for (const [k, v] of Object.entries(seed)) window.localStorage.setItem(k, v); } catch { /* ignore */ }
  }, scenario.seed || {});
  return page;
}

async function ready(page, selector = '.nx-bi, .nx-command-deck, .nx-panel') {
  await page.waitForSelector(selector, { timeout: 15000 }).catch(() => {});
  await page.addStyleTag({ content: '*,*::before,*::after{animation:none !important;transition:none !important;}' }).catch(() => {});
  await sleep(900);
}

async function shot(page, name) {
  const path = join(OUT, `${name}.png`);
  await page.screenshot({ path, fullPage: true }).catch(async () => { await page.screenshot({ path }); });
  const text = await page.evaluate(() => document.body.innerText).catch(() => '');
  collected.push({ name, text });
  console.log(`  ✓ ${name}.png`);
}

const clickByText = (page, sel, re) => page.evaluate((sel, reSrc) => {
  const rx = new RegExp(reSrc); const el = [...document.querySelectorAll(sel)].find(e => rx.test(e.textContent || ''));
  if (el) { el.click(); return true; } return false;
}, sel, re.source);

// A pre-seeded cache (bounded) so the cached/offline state has data to show.
function seededCache() {
  return JSON.stringify({
    version: 1, providerId: 'mempool.space', savedAtMs: Date.now() - 8 * 60000,
    blocks: makeBlocks(869990).slice(0, 8).map(b => ({
      height: b.height, hash: b.id, timestampMs: (b.timestamp) * 1000, sourceTimestampMs: Date.now() - 8 * 60000,
      txCount: b.tx_count, size: b.size, weight: b.weight, totalFees: b.extras.totalFees, subsidy: 312500000, reward: b.extras.reward,
      attribution: { poolName: b.extras.pool.name === 'Unknown' ? null : b.extras.pool.name, providerLabel: b.extras.pool.name === 'Unknown' ? null : b.extras.pool.name, slug: b.extras.pool.slug, method: 'provider-pool', confidence: 'probable', source: 'mempool.space', evidence: { coinbaseTagAscii: b.extras.coinbaseSignatureAscii, coinbaseTagId: null, providerMatchRate: null, reason: 'cached', aliases: [] } },
      configuredMatch: 'insufficient', source: 'mempool.space',
    })),
  });
}

// =====================================================================
async function main() {
  const browser = await puppeteer.launch({ executablePath: EDGE, headless: 'new', args: ['--no-sandbox', '--disable-gpu', '--window-size=1440,1200'] });
  const WS = `${BASE}/#/bitcoin`;
  const DECK = `${BASE}/#/`;
  const navTo = async (page, url) => { await page.goto(url, { waitUntil: 'domcontentloaded' }).catch(() => {}); await ready(page); await page.waitForSelector('.nx-tl-card, .nx-bi-banner, .nx-bi-skeleton', { timeout: 8000 }).catch(() => {}); await sleep(700); };

  try {
    // 01 landing / fresh — full workspace (timeline, table, attribution, source, privacy, confidence explanation)
    { const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }); await navTo(p, WS); await shot(p, '01-landing-fresh-workspace'); await p.close(); }

    // 02 block-detail drawer (fees / reward / attribution evidence) — open the Foundry tip
    {
      const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() });
      await navTo(p, WS);
      await clickByText(p, '.nx-tl-card', /./); await sleep(1200);
      await p.waitForSelector('.nx-drawer-body', { timeout: 6000 }).catch(() => {});
      await sleep(600); await shot(p, '02-block-detail-drawer'); await p.close();
    }

    // 05 provider unavailable (no cache)
    { const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks(), blocksOffline: true }); await navTo(p, WS); await shot(p, '05-provider-unavailable'); await p.close(); }

    // 06 cached / retrying — seeded cache + provider offline (data shown, retrying)
    { const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks(), blocksOffline: true, seed: { NX_BLOCK_INTEL_CACHE: seededCache() } }); await navTo(p, WS); await sleep(1500); await shot(p, '06-cached-retrying'); await p.close(); }

    // 07 replaced-tip — reload with same height but a different tip hash, then refresh
    {
      const scenario = { info: baseInfo(), blocks: makeBlocks(870000, { tipHashSuffix: 'a' }) };
      const p = await newPage(browser, scenario); await navTo(p, WS);
      scenario.blocks = makeBlocks(870000, { tipHashSuffix: 'REPLACED' });
      await clickByText(p, '.nx-btn', /Refresh/); await sleep(1600);
      await shot(p, '07-replaced-tip'); await p.close();
    }

    // 08 new-block highlight — refresh with the tip advanced by one height
    {
      const scenario = { info: baseInfo(), blocks: makeBlocks(870000) };
      const p = await newPage(browser, scenario); await navTo(p, WS);
      scenario.blocks = makeBlocks(870000, { tipHeightBump: 1 });
      await clickByText(p, '.nx-btn', /Refresh/); await sleep(1200);
      await shot(p, '08-new-block-highlight'); await p.close();
    }

    // 09 red-accent semantic proof — semantic pill colors must not follow the accent
    {
      const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }); await navTo(p, WS);
      await p.evaluate(() => document.documentElement.style.setProperty('--primary-color', '#ef4444'));
      await sleep(400); await shot(p, '09-red-accent-semantic-proof'); await p.close();
    }

    // 10 Command Deck compact card
    { const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }); await p.goto(DECK, { waitUntil: 'domcontentloaded' }).catch(() => {}); await ready(p); await sleep(1500); await shot(p, '10-command-deck-card'); await p.close(); }

    // 11 manual refresh (button state)
    {
      const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }); await navTo(p, WS);
      await clickByText(p, '.nx-btn', /Refresh/); await sleep(300);
      await shot(p, '11-manual-refresh'); await p.close();
    }

    // 21 mobile timeline
    { const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }, { mobile: true }); await navTo(p, WS); await shot(p, '21-mobile-timeline'); await p.close(); }

    // 22 mobile detail drawer
    {
      const p = await newPage(browser, { info: baseInfo(), blocks: makeBlocks() }, { mobile: true });
      await navTo(p, WS); await clickByText(p, '.nx-tl-card', /./); await sleep(1200);
      await p.waitForSelector('.nx-drawer-body', { timeout: 6000 }).catch(() => {});
      await sleep(500); await shot(p, '22-mobile-detail-drawer'); await p.close();
    }

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
    feature: 'Bitcoin Block Intelligence (Phase 2L)',
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
