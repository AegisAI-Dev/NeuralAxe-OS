# NeuralAxe OS — Phase 2G Operations Report

**Phase:** 2G — Operations Control, Pool Switching, Advanced Tuning Workspace and Command Deck Intelligence
**Branch:** `neuralaxe-v0.1-operations` (parent: `neuralaxe-v0.1-real-device-audit`)
**Commit:** `4cded376a4820e02b7975275208efd9a6b634ea5` — "feat: add NeuralAxe operations console" (owner-committed)
**Build identity:** `v2.14.2-17-g4cded376` (clean pair — verified inside `esp-miner.bin` app descriptor *and* inside `www.bin`)
**Target:** Gamma / board 601 / BM1370 / ESP32-S3 N16R8
**Verdict:** **PASS**

---

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Safe pool swap workflow | PASS — preference flip via existing `useFallbackStratum`, confirmation-gated |
| No unsupported hot switch | PASS — existing PATCH + restart only; no new endpoints |
| All pool configuration fields preserved | PASS — by construction (only the preference flag is written) |
| Tuning workspace distinguishes Current and Pending | PASS — device baseline vs unsaved edits, per-field restart classification |
| No automatic tuning | PASS — presets fill pending controls only; Save / Apply & Restart are explicit |
| Unsupported controls not fabricated | PASS — documented in-page as not supported by the firmware contract |
| Command Deck gains valuable operational information | PASS — session, quality, odds, headroom, drawer, chart ranges/overlays |
| Solo-mining estimates mathematically correct, carefully worded | PASS — tested formulas; labeled "statistical estimate … not a prediction" |
| Semantic colors retain meaning under all themes | PASS — fixed tokens + accent allowlist; DOM-level tests per selectable theme |
| Live web-version state honest | PASS — six-state derivation; boot snapshot always labeled; equality never faked |
| All frontend tests pass | PASS — **252/252** (baseline 134), 3 consecutive runs + final attested run |
| Firmware QEMU remains 61/61 | PASS — **61 Tests, 0 Failures, 0 Ignored** (qemu-xtensa, IDF v5.5.3) |
| Clean matching firmware/web pair | PASS — `v2.14.2-17-g4cded376` in app_desc, in `www.bin`, in `version.txt` |
| No protected hardware behavior changed | PASS — zero firmware C/NVS/partition/OTA source changes in 2G |
| No physical hardware access | PASS — source, mocks, containers, QEMU, dev server only |

## 2. Real-device findings resolution (Stage 1)

The live discrepancy (firmware `v2.14.2-15-g723e61dc`, web reported `v2.14.2-13-g388287da`, yet 2F grouped navigation visibly live) is a **stale boot snapshot**: the Updates page displayed the firmware's boot-time `axeOSVersion` unlabeled. Sequence on the device: firmware OTA to `-15` (restarts; www still `-13` at that boot) → www OTA to `-15` (no restart) → served UI is `-15`, live `/version.txt` is `-15`, boot snapshot stays `-13` until the next restart. Not an artifact mismatch; a restart reconciles it. The Updates page now derives its display from the live `/version.txt` with the boot snapshot separately labeled (§10).

The red-theme fault: the accent system wrote `--progressbar-value-bg`, `--slider-range-bg`, … to the raw accent color, so a red accent painted healthy telemetry bars red; additionally all three theme-apply code paths wrote **arbitrary stored keys** to the document root. Fixed in §9.

## 3. Changed files

All in `main/http_server/axe-os/src/app/` (no firmware sources changed):

**New:** `services/version-state.ts` (+spec), `services/semantic-status.ts` (+spec), `services/theme-semantics.spec.ts`, `components/pool/pool-switch.ts` (+spec), `components/pool/pool.component.spec.ts`, `components/edit/tuning.ts` (+spec), `components/command-deck/deck-intel.ts` (+spec).

**Modified:** `services/theme.service.ts`, `layout/service/app.layout.service.ts`, `components/design/theme-config.component.ts`, `components/command-deck/command-deck.styles.scss`, `components/home/home.component.{ts,html}`, `components/system/system.component.ts`, `components/update/update.component.{ts,html}`, `components/pool/pool.component.{ts,html}`, `components/edit/edit.component.{ts,html,spec.ts}`, `components/command-deck/command-deck.component.{ts,html,spec.ts}`.

## 4. Pool-switch implementation (Stage 2)

A literal primary↔fallback value swap is **impossible to do faithfully from the frontend**: pool passwords are write-only through the API (`stratumPassword` / `fallbackStratumPassword` exist only in the PATCH model, never in any GET), so a value swap would pair each host with the wrong credential. The firmware already provides the safe mechanism: **`NVS_CONFIG_USE_FALLBACK_STRATUM`** (REST `useFallbackStratum`), honored at boot (`system.c`) and treated as an explicit user choice by the failover coordinator (`protocol_coordinator.c` disables the heartbeat auto-return to primary).

**"Switch Active Pool & Restart"** therefore: snapshots a switch plan from live state → confirmation dialog (current active/standby endpoints, resulting active/standby, masked worker, explicit "passwords unchanged — never displayed", reconnect + share-statistics-reset warnings, auto-return caveat per direction, Cancel / Switch & Restart) → PATCH `/api/system` with **exactly** `{"useFallbackStratum": <bool>}` → existing `/api/system/restart`. Gating blocks: unusable fallback or primary config, equivalent pools, unsaved form edits, busy state. Failure paths are non-destructive with explicit messages; nothing runs on page load; no credentials are logged or rendered; keyboard accessible (native buttons, focus-visible styling). Automatic failover logic is untouched and no firmware endpoint was added.

## 5. Tuning workspace (Stage 3)

**Current Operating State** (read-only live): configured/actual frequency, configured/measured core voltage, ASIC temp, VRM temp, thermal delta vs target, target temp, fan % / RPM, min fan, power, efficiency (J/TH), ASIC error rate.

**Supported controls** (all firmware-backed, bounds mirrored from `nvs_config.c`): `frequency` (float ≥ 1; option list + default served by `/api/system/asic`), `coreVoltage` (u16 mV ≥ 1; option list + default from API — 1150 mV is the genuine board-601 default), `autofanspeed`, `temptarget` (35–66 °C), `minfanspeed` (0–99 %), `manualFanSpeed` (0–100 %), `overheat_mode` (clear-only: firmware accepts only 0), `overclockEnabled`, plus display type/rotation/invert/sleep and `statsFrequency`.

**Current/Pending:** the loaded device configuration is kept as an immutable baseline; a "Pending Changes" review lists every diverging field as `current → pending` with **"Applies after Save"** vs **"Restart required"** per the existing no-restart field list. Save re-baselines; **Apply & Restart** is save-then-restart (never silent); **Revert Unsaved Changes** restores the baseline including the slider auxiliary controls.

**Presets** (only when the device serves ≥2 options per axis; built exclusively from those values; exact values always shown on the buttons; nothing auto-saves/auto-restarts; Custom auto-selected on divergence): Eco = lowest option pair, Balanced = the board defaults, Performance = highest option pair. With the current board-601 dev option lists that renders e.g. `Eco · 400 MHz / 1100 mV`, `Balanced · 485 MHz / <device default> mV`, `Performance · 575 MHz / 1300 mV` — values come from the device API at runtime, never hardcoded. No preset is labeled "safe"; no automatic tuning is claimed.

**Validation:** firmware-range validators reject NaN/null/undefined/non-finite, out-of-range and (where integral) fractional values at exactly the firmware's own bounds; Save/Apply disable on invalid forms. Tested for 1150 mV, other valid values, and each bound.

**Unsupported (documented in-page, not fabricated):** fan curves, temperature-limit tuning, hysteresis, power-limit/ASIC-power controls — *NOT SUPPORTED BY CURRENT FIRMWARE CONTRACT*.

## 6. Command Deck additions (Stage 4)

- **Session performance:** uptime, accepted, rejected, reject rate, stability, shares/hour, pool latency, pool difficulty (hero keeps expected/1h/efficiency/power/errors).
- **Hashrate quality:** current-vs-expected %, current-vs-1h %, recent variability (coefficient of variation of the live sample window), with tooltips naming the formulas.
- **Solo Mining Odds** (side panel): average time to block, chance per day, "1 in N days", session/all-time best difficulty as % of network. Explicitly labeled *"Statistical estimate from the current hashrate and network difficulty — not a prediction. A solo block may come far sooner, far later, or never."* Zero/invalid inputs render as "—"; tiny values use scientific notation.
- **Thermal headroom:** transparent state pill (Below target / At target — thermal control stable / Above target / Above safe temperature, with "fan near saturation" when the auto fan is pinned ≥ 95 %), signed delta vs target, and measured-vs-configured core-voltage offset.
- **Pool & network health:** active pool + latency + Wi-Fi RSSI (existing), fallback-configured state with honest wording ("reachability is only known while it is in use"), version-pair insights (mismatch = warning; stale-boot-snapshot = informational restart note).
- **Advanced Telemetry drawer** (collapsed by default): board/ASIC/reset reason, firmware + live web + boot-snapshot revisions, OTA partition, ESP-IDF, memory breakdown, configured tuning values, masked worker.

The hashrate hero remains the dominant element; new content is compact stat rows, one side panel and a collapsed drawer.

## 7. Hero chart ranges and overlays (Stage 5)

Ranges **Live / 5 min / 15 min / 1 h / All**, overlays **ASIC Temp / Power / ASIC Errors** on a second axis. Live mode keeps the rolling in-session series; history ranges load a snapshot from the device's existing rolling statistics buffer (`/api/system/statistics`) **only on explicit selection** — never on page load. Honest empty/warm-up states: "no history", "not enough logged history", and "only N min of history is available so far" when the buffer does not cover the window; nothing is invented. Accessible (`aria-pressed` chips, labeled groups), no added animation, mobile-wrapping chip rows.

## 8. Formula definitions (all in tested pure modules)

| Metric | Formula |
|---|---|
| Expected hashes per block | `networkDifficulty × 2^32` |
| Expected time to block | `expectedHashes / (hashrate_GHs × 10^9)` seconds |
| Daily block probability | `1 − exp(−86400 / expectedSeconds)` |
| "1 in N days" | `expectedSeconds / 86400` |
| Best diff % of network | `bestDiff / networkDifficulty × 100` |
| Shares per hour | `sharesAccepted / uptimeSeconds × 3600` (null under 60 s uptime) |
| Reject rate | `rejected / (accepted + rejected) × 100` (null before any share) |
| Variability | sample std-dev / mean of the recent hashrate window, % (≥ 5 samples) |
| Efficiency | `power_W / (hashrate_GHs / 1000)` J/TH |
| Thermal delta | `temp − temptarget` °C (±2 °C band = "at target"; fixed 70 °C overheat line) |

## 9. Semantic color system (Stage 6)

Fixed semantic tokens `--nx-sem-ok/info/warn/danger/neutral` (green/cyan/amber/red/gray) plus `.nx-meter-*` classes that override the accent-driven progress-bar fill. Telemetry severity comes from tested threshold helpers (`semantic-status.ts`) that mirror the warnings the UI already showed (70 °C ASIC danger, 105 °C VRM danger, low-voltage rule, 90 %/100 % power bands). The user accent keeps navigation highlight, buttons, sliders, checkboxes, focus and decorative chart styling. All three theme-apply paths now pass through an **allowlist** (`ACCENT_COLOR_KEYS`) so a stored theme payload can never override semantic or layout variables. DOM-level tests apply **every selectable theme** (including Red `#F80421`) against the real compiled stylesheet and assert semantic meters keep their computed colors while plain accent bars follow the theme. Screenshot proof: red accent with green/cyan healthy telemetry (`screenshots/09-…`, `10-…`).

## 10. Version-state outcome (Stage 7)

`deriveVersionState(firmware, bootWeb, liveWeb)` is the single rule set consumed by the Home banners, Device Status rows, Updates card and Command Deck insights: installed web = live `/version.txt` **only**; boot snapshot separately labeled; states `match`, `mismatch` (live-verified — warning), `match + restart-pending` (informational), `unverified` (live unavailable — explicit, no faked equality), `unknown`. The Updates page now shows *Web (installed)*, a labeled *Web (at boot)* when stale, and a *Pair Status* pill ("Firmware & web match" / "Match — restart pending" / "Version mismatch" / "Live web version unavailable"). All six required cases are unit-tested (matching pair, genuine mismatch, restart pending, unavailable live, stale boot snapshot, reconciliation after restart) plus screenshot proof of each visual state.

## 11. Tests and builds (Stage 9, Phase B — clean committed pipeline)

| Step | Result |
|---|---|
| `npm ci` | exit 0 |
| Frontend tests ×3 + final attested run | **252/252** each (baseline 134; +118 new) |
| Host production build | clean `v2.14.2-17-g4cded37`, no `-dirty` |
| Firmware build (espressif/idf:v5.5.3, clean tree verified first) | exit 0; **0 warnings from `main/`** |
| App descriptor | `APP DESC OK` — `v2.14.2-17-g4cded376` |
| www/firmware pair | `PAIR OK` — same revision embedded in `www.bin` |
| Merged image | 15,802,368 B |
| QEMU (qemu-xtensa 9.2.2) | **61 Tests, 0 Failures, 0 Ignored** |
| Release export | `EXPORT OK` (4 artifacts) |
| Manifest validation | `VALIDATION OK` (board Gamma/601/BM1370) |
| Release-gates battery | 13 passed, 0 failed |
| Secrets / local-path scan | CLEAN |
| Route privacy scan | CLEAN (12/12 routes) |

Pipeline note: the first container build reported `-dirty` — a CRLF cross-platform false positive (Windows working tree vs container git `autocrlf=false`); fixed via container-side `git config core.autocrlf true` and a full clean rebuild whose describe was verified **before** compiling. `partition-table.bin` and `ota_data_initial.bin` are byte-identical to the 2F baseline; `bootloader.bin` differs only because 2F was built on IDF v5.5.2 vs the CI-pinned v5.5.3 here (embedded version strings confirm; OTA never flashes the bootloader).

## 12. Hashes (SHA-256)

| Artifact | SHA-256 |
|---|---|
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` | `d8429cd6eedc7b4d540249ed0917097caed93f54c44fffe1edd1974ca993b421` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` | `9ae58eb46ae8e36ee752d7802737f0a5dcabfe3bfa63203590fe2ba3ec38d827` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` | `2b251c3730ce3bbf05ca409d757626a171e49848da29eafa3128bf5a349dd8b4` |
| `config-601.cvs` | `3c0f28f6112cf9e12ade941e3f82aa750fa3554a7e84b73340d665a571a55f22` |
| `bootloader.bin` (IDF v5.5.3) | `a16e7a67924cc3037c0db4e3c4319c0526b8a514c0736d6f4ed8e9a46ad2aea2` |

Full sums in `release-export/NeuralAxe-OS-v0.1.0-dev-Gamma-601-SHA256SUMS.txt`; artifacts, logs and 26 sanitized screenshots in
`D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\operations-v0.1.0-dev-board601\`.

## 13. Unresolved hardware validations (owner-gated)

1. **Pool switch on hardware:** flip boots onto the chosen pool; no auto-return while user-preferred; switch-back restores normal failover.
2. **Tuning workspace on hardware:** Save / Apply & Restart / Revert; live-applied fields vs restart fields behave as classified.
3. **Version states on hardware:** restart-pending after a www-only OTA; reconciliation after restart.
4. **Still open from 2C:** recovery-page reachability, factory-wipe confirmation, on-device OTA retention.

## 14. Next OTA recommendation

Install the `v2.14.2-17-g4cded376` pair on the pilot Gamma from `release-export/`: **`www.bin` first** (mining continues), **then `ota.bin`** — its restart also refreshes the boot snapshot, so the device comes up fully reconciled in one reboot. Verify Updates → Pair Status shows **"Firmware & web match"** with no restart-pending note, then exercise the new pool-switch and tuning flows per §13.

## 15. Hardware access confirmation

No COM/USB access, no miner IP contacted, no esptool/bitaxetool against hardware, no flashing, no physical device touched, and the private TCH flashdump never inspected. Phase 2G used source analysis, mock data, Docker containers, QEMU and the local dev server only. Git state was only read from `.git` metadata (plus the sanctioned build/export tooling's own `git describe`); all commits were made by the owner in GitHub Desktop.
