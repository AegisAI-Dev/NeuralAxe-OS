# NeuralAxe OS — Phase 2I.1 Fleet Master-Detail Report

**Phase:** 2I.1 — Fleet Master-Detail UX and Operational Signal Refinement
**Branch:** `neuralaxe-v0.1-fleet-master-detail` (parent: `neuralaxe-v0.1-fleet-operations`)
**Commit:** `930facda58663f943a1d8ed8b38a3cb3065dcc0b` — "feat: Fleet master-detail workspace and health sample confidence" (owner-committed)
**Build identity:** `v2.14.2-25-g930facda` (clean pair — verified inside `esp-miner.bin` app descriptor *and* inside `www.bin`)
**Target:** Gamma / board 601 / BM1370 / ESP32-S3 N16R8
**Verdict:** **PASS** — frontend and product architecture only; discovery, per-device API calls, control behavior, classification semantics and all firmware sources are unchanged.

---

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Master-detail is the default desktop Fleet experience | PASS — split navigator (~37%) + persistent tabbed workspace (~63%) |
| No page-level horizontal scrollbar | PASS — verified at 1600 px desktop, 900 px tablet, 390 px mobile |
| Device navigator concise and readable | PASS — health, hostname, model/board, class badge, hashrate, ASIC temp, power/efficiency, primary alert, offline/last-seen |
| Selected-device intelligence rich but structured | PASS — Overview/Thermal/Mining/Software/Tuning tabs, progressive disclosure |
| Packed action icons gone from primary view | PASS — Settings + one consolidated Actions menu |
| Table mode remains for fleet-wide comparison | PASS — optional 7-column view with sorting + density |
| Summary metrics have clear hierarchy | PASS — 5 primary tiles + subordinate classification chip strip |
| One startup reject does not create noisy Attention | PASS — sample-confidence rule; 1/26 at 2 min stays Healthy with a warming-up note |
| Genuine persistent rejection still triggers Attention | PASS — ≥3 rejects or ≥100 total then >2% (tested) |
| Board 702 explicitly unsupported but monitorable | PASS — unsupported notice + monitoring data; health independent |
| AxeOS telemetry remains honest | PASS — reduced-telemetry note; no fabricated NeuralAxe thermal fields |
| Privacy masking complete | PASS — navigator, workspace, table, cards, drawer (DOM-asserted) |
| All frontend tests pass | PASS — **379/379** ×3 consecutive clean-`npm ci` runs (+18 over 361) |
| Firmware behavior unchanged | PASS — zero firmware sources; QEMU **83 Tests 0 Failures 0 Ignored** |
| Matching clean firmware/web pair | PASS — `v2.14.2-25-g930facda` in app_desc, in `www.bin`, in `version.txt` |
| No hardware or live-network access | PASS — source, synthetic intercepted fleet, tests, local dev server only |

## 2. Audit findings

The 2I default was a 10-column table (horizontal scroll at common widths), a click-transient drawer, six packed icon buttons per row, and an eight-box equal-weight summary. **The pilot false positive was root-caused in code:** `rejectRatePct` applied a flat 2% threshold with no sample floor, so 1 reject / 26 shares (3.85%) at ~2 minutes uptime raised Attention during startup. The Command Deck glance showed `0/N online` for a stored-but-never-refreshed fleet without distinguishing pending from an outage.

## 3. Changed files (9, all frontend)

`components/swarm/fleet-intel.ts` (+spec) — sample-confidence rule and `shareSampleNote`. `components/swarm/swarm.component.{ts,html,spec.ts}` — view modes, selection model, workspace tabs, action menu, summary hierarchy. `components/swarm/fleet.styles.scss` — split layout, workspace, action menu, summary. `components/command-deck/command-deck.component.{ts,html,spec.ts}` — pending-vs-outage glance.

## 4. Health sample-confidence contract (Stage 5)

Reject-rate Attention becomes actionable only when the share sample is confident:

```
rejectSampleConfident = (sharesAccepted + sharesRejected >= 100)
                        OR (sharesRejected >= 3)
then apply the existing rejectRatePct > 2%
```

Documented constants: `REJECT_MIN_TOTAL_SHARES = 100`, `REJECT_MIN_REJECTED = 3`. While the naive rate exceeds 2% but the sample is not yet confident, a neutral display-only note is shown — "Share sample still warming up (N rejected of M — too few shares to judge)" — never a health state. Tested cases: 1/26 → Healthy + note; 1/100 → below 2%, no Attention, no note; 3/100 → Attention; 3/30 → Attention (repeated rejection); zero-share startup → no signal, no note; a genuine reason (66 °C) remains authoritative during a small sample; Critical always overrides. ASIC (65/70 °C), VRM (85/105 °C), fan saturation (95%), sensor validity, emergency override, power fault and pair mismatch are unchanged.

## 5. Master-detail workspace (Stages 2/7/9)

**Navigator (left):** one compact row per device — health dot, masked hostname, class badge, live hashrate, ASIC temperature, efficiency (or power when efficiency is unavailable), the primary alert reason, and offline/last-seen. Selected state is highlighted; Enter/click and Arrow Up/Down navigate; **selection performs no remote action** (spy-tested). **Workspace (right):** persistent header (health dot, hostname, model, classification, online/last-seen, Settings + Actions), always-visible health explanation with the warming-up note when relevant, and tabs Overview / Thermal / Mining / Software / Tuning. Overview leads with hashrate-vs-expected, power, efficiency, ASIC temp, shares + error state, active pool, uptime and the firmware pair line — not every value at equal weight. The Thermal tab shows the full 2H contract for NeuralAxe devices and "not reported by this device" otherwise; a compatible AxeOS device carries a "reduced telemetry" note; a board-702 device carries the unsupported notice. `selectedIp` resolves against the live list each render, and `ensureSelection()` (called after filter, refresh, scan and remove) reselects the first visible device when the current one is filtered out/removed, or shows an explicit empty selection.

## 6. Summary hierarchy (Stage 4)

Five primary tiles — Devices (online / total with offline + pending split), Fleet Hashrate, Fleet Power (both "from N of M online" under partial coverage), Fleet Efficiency ("needs power + hashrate" when unavailable), and Alerts (attention / critical, tile border highlighted only when non-zero). Below them a subordinate chip strip: NeuralAxe Managed · AxeOS Compatible · Unsupported Target · Unknown · Pools. No duplicated counts; semantic colours independent of the accent.

## 7. Optional table view, actions, responsive (Stages 3/6/8/10)

**Table view:** reduced to seven columns (Health, Device, Hashrate, ASIC Temp, Power, Uptime, Software), sorting + density retained, secondary data moved into Details, one Details action per row, persisted via `FLEET_VIEW_MODE`, no horizontal page overflow at common desktop widths. **Actions:** Settings button plus one labelled Actions menu — Pause/Resume, Restart…, Identify, separator, Remove (danger, isolated); tooltips + aria labels; restart and remove confirmation contracts unchanged (opening the menu performs nothing; restart routes through its confirmation; remove is local-list only — spy-tested). **Responsive:** desktop split with the selected device always visible; tablet (992–1280 px) narrows the navigator; mobile (< 992 px) shows single-column cards and opens the full-screen sheet from Details — no side-by-side, no icon clusters, alerts visible, offline devices honest with dashes. **Visual quality:** the single giant bordered rectangle is gone; panels are separated, the card count is restrained, and semantic states stay legible under both the default and red accents (proven in the red-accent capture).

## 8. Command Deck fleet glance (Stage 11)

The compact glance now renders "N awaiting first refresh" when every stored device is pending, instead of "0/N online" reading as an outage before any data exists; it keeps its explicit snapshot age, follows the refined health model for its alert count, and is unchanged in size.

## 9. Pipeline results (Stage 13)

| Step | Result |
|---|---|
| Clean commit verified | `930facda…`, exactly the 9-file frontend set, tree clean |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **379 / 379 / 379** |
| Production web build | clean |
| Full ESP-IDF build (v5.5.3 container) | clean — APP DESC OK, not dirty |
| Release pair | firmware app_desc == revision inside `www.bin` — **PAIR OK `v2.14.2-25-g930facda`** |
| Merged factory image | 15 802 368 B |
| QEMU regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean |
| Screenshot privacy scan | CLEAN (16 captures; gate enforced by the exporter) |

**Artifacts** — `NeuralAxe Build Artifacts/fleet-master-detail-v0.1.0-dev-board601/`:

```
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin      3 145 728 B  f67481db…6b15d8bf
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin      1 658 272 B  31c02d0c…a623d962b
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin 15 802 368 B  080416da…d59a7693
release/config-601.cvs                                       975 B  3c0f28f6…a55f22
release/…-manifest.json, …-SHA256SUMS.txt
screenshots/ (16 sanitized captures) · screenshot-privacy-scan.md · pipeline-evidence/
```

## 10. Unresolved / owner-gated

Board 702's reduced-telemetry honesty is expressed via its unsupported notice plus per-field "not reported by this device" (correct by classification) rather than the AxeOS-compatible note. The deck glance remains an explicitly-aged snapshot by design. Validation against the owner's real mixed network remains owner-gated; the standing 2H hardware caveats are unchanged.

## 11. Hardware and network access confirmation

No physical miner IPs were contacted, no network scan was run, no COM/USB access, no restart/pause/identify/configure of hardware, no flashing, no esptool/bitaxetool, and the private TCH recovery dump was never inspected. The screenshot fleet was synthetic fixture data served through browser request interception on the local dev server.
