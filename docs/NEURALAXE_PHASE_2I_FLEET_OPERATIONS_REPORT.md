# NeuralAxe OS — Phase 2I Fleet Operations Report

**Phase:** 2I — Fleet Operations and Device Intelligence
**Branch:** `neuralaxe-v0.1-fleet-operations` (parent: `neuralaxe-v0.1-thermal-pilot-polish`)
**Commit:** `a71f59267296655e1dd8c077745fc92c0eddf4a2` — "feat: rebuild Fleet as the NeuralAxe Fleet Command Center" (owner-committed)
**Build identity:** `v2.14.2-23-ga71f5926` (clean pair — verified inside `esp-miner.bin` app descriptor *and* inside `www.bin`)
**Target:** Gamma / board 601 / BM1370 / ESP32-S3 N16R8
**Verdict:** **PASS** — frontend and product architecture only; discovery, fleet communication, remote-control behavior and all firmware sources are unchanged.

---

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Fleet no longer resembles a sparse upstream table | PASS — summary bar, health/classification model, operations table, cards, drawer, confirmation-gated actions |
| Every value clearly labeled with units | PASS — labeled columns/tiles; unavailable values render "—" or an explicit note, never invented |
| Meaningful fleet summary | PASS — online/offline/pending split, partial-coverage-labeled hashrate/power, paired-only efficiency, class counts, alert counts, pool distribution |
| Deterministic health state | PASS — five states from fixed tested thresholds; every fired rule is shown as the explanation |
| Honest device classification | PASS — NeuralAxe managed / AxeOS compatible / unsupported target / unknown, all tested |
| Board 702 never represented as supported | PASS — "Board 702" pill + drawer notice: not a release target, NeuralAxe firmware must not be installed (tested + screenshot) |
| Desktop table readable | PASS — sticky header, 9 sortable columns, density modes, truncation with tooltips, hover/focus |
| Mobile cards intentional | PASS — dedicated cards < 992 px, no horizontal overflow, touch-sized Details, alerts unmistakable |
| Detail drawer adds real value | PASS — six sections rendering only device-reported data, incl. the full 2H thermal contract for NeuralAxe devices |
| Actions understandable and safe | PASS — labeled buttons, restart confirmation, stronger isolated remove confirmation, row click = details only (all spy-tested) |
| Privacy masking complete | PASS — hostname/IP/pool masked in summary, table, cards, drawer (DOM-asserted); no MAC/SSID/worker/credential rendered |
| All frontend tests pass | PASS — **361/361** ×3 consecutive clean-`npm ci` runs (baseline 321; +40) |
| No protected firmware behavior change | PASS — zero firmware sources in the commit; QEMU **83 Tests 0 Failures 0 Ignored** re-run from the clean commit |
| Matching clean firmware/web pair | PASS — `v2.14.2-23-ga71f5926` in app_desc, inside `www.bin`, and in `version.txt` |
| No hardware or live-network access | PASS — source, mocks, intercepted fixture fleet, tests, local dev server only |

## 2. Audit findings (Stage 1)

The Fleet page (`SwarmComponent`) scans the local /24 (per-IP `GET /api/system/info` + `/api/system/asic`, 5 s timeout, 128 concurrent), supports manual add-by-IP, persists to localStorage and refreshes every 5–30 s. Each device entry is the full merged SystemInfo+ASIC payload plus `IP` — so **NeuralAxe devices expose everything including the Phase 2H thermal contract, upstream AxeOS devices expose the standard field set, and no device gets values it does not report** (latency, VRM temperature, thermal mode, efficiency all render as unavailable when absent). Pre-2I gaps: offline devices manifested only as zeroed telemetry (no reachable flag, no last-seen), restart/remove executed on a single icon click with no confirmation, totals were four naive numbers, and no detail view existed.

## 3. Architecture

All derivation lives in the pure, fully tested `components/swarm/fleet-intel.ts`; the component keeps every network/discovery/action code path from before and adds only reachability bookkeeping (`nxReachable` / `nxLastSeenMs` written in the existing success/error handlers), presentation state, and confirmation gating. Fleet styles live in `fleet.styles.scss`, imported globally (the command-deck pattern) to stay outside Angular's per-component style budget.

**Health model** — offline (refresh failed; reason carries last-seen age) · unknown (never refreshed, or reachable without core telemetry — *absence of telemetry is never healthy*) · critical (overheat mode, emergency override, power fault, ASIC ≥ 70 °C, VRM ≥ 105 °C) · attention (ASIC 65–70 °C, VRM 85–105 °C, fan ≥ 95 %, ASIC errors > 2 %, reject rate > 2 %, fallback pool, paused, invalid curve fallback, invalid sensor, firmware/web pair mismatch) · healthy. Reasons list every fired rule; classification never affects health (an unsupported board can be perfectly healthy — tested).

**Classification** — `neuralaxe` (board 601 + NeuralAxe identity), `compatible` (board 601 AxeOS), `unsupported` (any other board; wins even over a NeuralAxe identity), `unknown` (insufficient data). The board-702 tooltip/notice states the device is monitorable as an AxeOS miner but that NeuralAxe firmware must not be installed on it.

**Summary** — total = online + offline + pending-first-contact exactly; class counts sum to total; hashrate/power totals include online devices only and are labeled "from N of M online" under partial coverage; fleet efficiency uses only devices pairing power with hashrate ("needs power + hashrate" otherwise); attention/critical counts; per-pool device distribution (masked).

## 4. Presentation

**Desktop table:** sticky header; columns Health · Device (hostname, model/ASIC, IP, class pill) · Hashrate (+expected, power) · Efficiency · Errors · Thermal (ASIC/VRM, fan %/RPM, mode) · Mining (pool, shares, latency) · Uptime · Software (firmware, web, pair pill) · Actions. Nine sortable columns (health severity, IP octets and derived efficiency sort correctly; null efficiency sorts last), comfortable/compact density, long values ellipsized with tooltips, row hover, keyboard focus with Enter-to-open. **Filters:** search (hostname/model/board/IP), health, classification, online state, pool (when >1), Clear-filters; persisted via the page's existing localStorage convention. **Mobile (< 992 px):** cards only — health dot + first alert reason, hostname/model, class pill, hashrate/ASIC-temp/power/efficiency grid, pool, one full-width Details button. **States:** scanning, none-found, filtered-empty (with clear action), mid-refresh indicator, offline rows with "seen N min ago" and dashes — stale data is never presented as live. **Detail drawer:** health explanation + compatibility notice, Overview, Performance, Thermal (full 2H contract where reported; "not reported by this device" otherwise), Pool & Network, Software (pair state, OTA partition, reset reason), Tuning, and footer actions with Remove isolated.

**Actions (Stage 10):** Details always non-destructive; Restart posts nothing until its confirmation ("mining stops for the reboot and resumes; no settings change"); Remove has a stronger, visually separated confirmation stating it only edits the local list; cancel paths tested; no bulk destructive actions, no invented fleet-wide controls; row click never triggers a device action (spy-tested). One documented presentation change: per-cycle refresh-failure toasts were replaced by the visible Offline row state (manual add keeps its explicit failure toast).

**Command Deck linkage (Stage 11):** a compact "Fleet" side panel (online/total, fleet hashrate, alert count, link) appears only when the stored fleet contains devices beyond this one, labeled with an explicit data age ("as of N min ago — last Fleet refresh") because it is a snapshot, not a live stream.

## 5. Privacy (Stage 12)

`sensitive-data` masking covers hostnames, IPs and pool hosts across the summary bar, table, cards and drawer — DOM-asserted for all three surfaces. MAC addresses, SSIDs, worker/wallet identifiers, credentials and serials are never rendered on the page. Screenshots use an entirely synthetic intercepted fleet; privacy scan: **CLEAN**.

## 6. Pipeline results (Stage 14)

| Step | Result |
|---|---|
| Clean commit verified | `a71f5926…`, exactly the 11-file frontend set, tree clean |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **361 / 361 / 361** |
| Production web build | clean |
| Full ESP-IDF build (v5.5.3 container) | clean — APP DESC OK, not dirty |
| Release pair | firmware app_desc == revision inside `www.bin` — **PAIR OK `v2.14.2-23-ga71f5926`** |
| Merged factory image | 15 802 368 B |
| QEMU regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean |
| Screenshot privacy scan | CLEAN (11 captures; gate enforced by the exporter) |

**Artifacts** — `NeuralAxe Build Artifacts/fleet-operations-v0.1.0-dev-board601/`:

```
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin      3 145 728 B  c003b9ef…e562021c
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin      1 658 272 B  f3f5ce7b…c2cae4ee
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin 15 802 368 B  e09d6cfd…4a8e9f52
release/config-601.cvs                                       975 B  3c0f28f6…a55f22
release/…-manifest.json, …-SHA256SUMS.txt
screenshots/ (11 sanitized captures) · screenshot-privacy-scan.md · pipeline-evidence/
```

## 7. Unresolved / owner-gated

No central "Fleet API error" state exists because there is no central fleet API — failures are per-device and render as Offline rows. The deck fleet panel is an explicitly-aged snapshot by design. Validation of the rebuilt page against the owner's real mixed network (including the physical SupraHex board 702) remains owner-gated; the 2H hardware caveats (sensor failure, emergency override, hard shutdown, VRM threshold, destructive rollback, factory reset) are unchanged from the 2H.1 report.

## 8. Hardware and network access confirmation

No COM/USB access, no live miner IPs contacted, no discovery on the owner's network, no flashing/restarting/configuring of physical miners, no esptool/bitaxetool, and the private TCH flashdump was never inspected. The screenshot fleet was synthetic fixture data served through browser request interception on the local dev server.
