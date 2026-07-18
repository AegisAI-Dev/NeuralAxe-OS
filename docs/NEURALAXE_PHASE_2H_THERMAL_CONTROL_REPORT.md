# NeuralAxe OS — Phase 2H Thermal Control Report

**Phase:** 2H — Adaptive Fan Curve, Hysteresis and Thermal-Control Foundation
**Branch:** `neuralaxe-v0.1-thermal-control` (parent: `neuralaxe-v0.1-operations`)
**Commit:** `88ccb619d1371fb4c21d2a3ed8b80a15ec2bd2c0` — "feat: add adaptive fan-curve thermal control with hysteresis" (owner-committed)
**Build identity:** `v2.14.2-19-g88ccb619` (clean pair — verified inside `esp-miner.bin` app descriptor *and* inside `www.bin`)
**Target:** Gamma / board 601 / BM1370 / ESP32-S3 N16R8
**Verdict:** **CONDITIONAL PASS** — every software-verifiable criterion passes; the conditional qualifier covers only the controlled-hardware validations listed in §12, which this phase deliberately did not perform.

---

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Existing TARGET mode behavior preserved | PASS — the PID branch of `fan_controller_task.c` is the original code path, verbatim; setpoint/EMA/min-fan semantics unchanged |
| Existing MANUAL mode behavior preserved | PASS — original manual branch, verbatim; emergency logic still overrides it |
| CURVE mode opt-in only | PASS — an unset/corrupt `thermalmode` resolves from `autofanspeed` to target/manual; curve requires an explicit owner save (tested) |
| Valid bounded curve contract | PASS — exactly 4 points, 20–70 °C strictly ascending, 0–100 % non-decreasing; versioned `"v1;…"` storage |
| Deterministic interpolation | PASS — pure integer-rounded linear interpolation with first/last-point plateaus; identical results host + QEMU |
| Hysteresis prevents oscillation | PASS — rising-instant / falling-gated ratchet; rapid-alternation test pins duty at the local maximum |
| Emergency response never delayed | PASS — overheat branch precedes all modes at 100 ms cadence; upward duty changes bypass hysteresis (tested) |
| Sensor failure fails safe | PASS — invalid/zero/missing temperature → the proven 70 % fallback; > 120 °C reading → 100 % fail-high (tested) |
| Hard thermal protection unchanged | PASS — overheat entry/recovery, VRM limits, watchdogs untouched (the only addition stamps `thermalmode="manual"` beside the existing forced writes) |
| Additive NVS/API migration | PASS — 3 new keys, none renamed/reinterpreted; API additions additive; OpenAPI/models/mocks aligned by compile |
| Invalid persisted config rejected | PASS — parse/validate rejects every malformed form; control loop falls back to TARGET with a `fanCurveError` diagnostic |
| Frontend cannot submit invalid curves | PASS — per-field + cross-field validation mirrors the firmware exactly; Save/Apply disable on invalid curves (tested + screenshot) |
| No unsupported controls fabricated | PASS — every control maps to a firmware key; the former "not supported" note was replaced by the real contract |
| Frontend tests pass | PASS — **286/286** (baseline 252; +34), three consecutive clean-`npm ci` runs |
| Firmware/QEMU tests pass | PASS — **83 Tests, 0 Failures, 0 Ignored** (baseline 61; +22 thermal), rerun from the clean commit |
| Matching clean artifact pair | PASS — `v2.14.2-19-g88ccb619` in app_desc, inside `www.bin`, and in `version.txt` |
| No hardware access | PASS — source analysis, host gcc, containers, QEMU, mock dev server only |

## 2. Existing fan-control audit (Stage 1)

The stock loop (100 ms poll) prioritizes: `overheat_mode` → 100 % · paused → 30 % · pools unavailable → 30 % · then `autofanspeed` picks PID or manual. TARGET is a reverse-acting PID (P=5, I=0.1, D=2) on an EMA-filtered (α=0.2) `max(chip_temp, chip_temp2)`, output-limited to `[minFanSpeed, 100]`, setpoint re-read each cycle; invalid temperature (≤ 0, e.g. ASIC uninitialized) → fixed 70 %. VRM temperature never participates in normal fan control — it belongs exclusively to hard protection (`power_management_task.c`: 105 °C VRM / 75 °C ASIC throttle → persisted `autofanspeed=false` + `manualfanspeed=100` + `overheat_mode=true`, mining stop, cooling loop, reduced-settings restart). Fan output is a float percent → EMC2101 duty (`63 × fraction`); RPM is telemetry only; min fan is enforced only in TARGET mode (manual may go to 0 — preserved). Board 601: EMC2101 external ASIC diode, single BM1370 (`chip_temp2` always −1), TPS546 VRM. Other writers of the fan keys: the BAP UART handlers and the overheat emergency path — both now stamp the new mode key to stay consistent.

## 3. Thermal-control modes (Stage 2)

Three explicit modes carried by the new NVS string `thermalmode` (`""` | `target` | `curve` | `manual`):

- **`""` (unset / legacy / corrupt)** → resolved from `autofanspeed`: true → TARGET, false → MANUAL. Every existing installation therefore keeps its exact semantics after OTA; **no installation can silently enter CURVE**.
- **TARGET** — the unchanged PID branch. **MANUAL** — the unchanged manual branch, with the UI stating explicitly that manual mode does not disable thermal protection. **CURVE** — the new opt-in behavior (§4).
- Consistency stamps keep the mode field and the legacy flag agreeing everywhere the legacy flag is written: PATCH `thermalControlMode` syncs `autofanspeed` (manual→false, else→true), BAP fan writes stamp the mode, and the overheat-emergency entry stamps `manual` beside its existing forced writes — so post-recovery behavior (manual 100 % until the owner reconfigures) is identical in every mode.

## 4. Fan-curve contract and hysteresis (Stages 3–4)

**Storage:** `fancurve` = `"v1;45:25;52:45;58:70;64:100"` (version-prefixed; strict parser). **Bounds:** exactly 4 points; temperatures 20–70 °C strictly ascending (the 70 °C cap keeps ≥ 5 °C margin under the 75 °C hard throttle); fan 0–100 % non-decreasing. Rejected: duplicate/descending temperatures, descending percentages, out-of-range values, wrong point counts, malformed/NaN input — at PATCH time (HTTP 400, nothing persisted) *and* again at load (corrupt NVS → board-default curve is **not** silently used for control; the loop runs the TARGET fallback and reports `fanCurveError`).

**Evaluation:** below the first point → first point's percent; linear interpolation (rounded to integer percent) between points; at/above the final point → final percent. The configured minimum fan is an authoritative floor under the entire curve; nothing can block the emergency 100 %.

**Hysteresis** (`fanhyst`, 0–8 °C, default 2): interpolation alone still lets ±0.5 °C flutter step the duty audibly, so downward transitions are gated: after any duty change the fan only slows once the control temperature has cooled ≥ H °C below the temperature recorded at that change. **Upward transitions always apply in the same 100 ms cycle** — a rising temperature is never rate-limited or delayed, including while a hold is active. This ratchet is deterministic and needs no additional ramp limiter or configurable update interval (deliberately not added). Default documented; QEMU-tested for rising, falling, alternating, staircase-descent and zero-hysteresis cases.

**Defaults** (`45→25 %, 52→45 %, 58→70 %, 64→100 %`): first point matches the stock 25 % minimum-fan default; 100 % is reached at 64 °C — below the 66 °C maximum target temperature and 11 °C below the throttle.

## 5. Temperature-source policy (Stage 5)

Effective control temperature = **EMA-smoothed maximum of the valid ASIC temperatures** — byte-identical input to the proven TARGET PID. VRM temperature deliberately remains exclusive to hard protection, as in stock firmware. Zero, negative, missing or implausible (> 120 °C) readings are never treated as valid-cool: invalid → 70 % fallback with hysteresis-state reset; implausible-high → 100 % fail-high. The UI shows ASIC temp, VRM temp, the effective control temperature and the driving sensor (`asic` / `asic2` / `none`).

## 6. NVS and API contract (Stage 6)

**NVS (additive only):** `thermalmode` (str, default `""`), `fancurve` (str, default `""` = built-in default curve), `fanhyst` (u16, 0–8, default 2). No key renamed, retyped or reinterpreted; factory provisioning (`config-601.cvs`) untouched; board 702 not claimed.

**System API GET:** configuration `thermalControlMode` (resolved), `fanCurve[]` (`{tempC, fanPercent}`), `fanCurveHysteresis`; live decision telemetry `effectiveControlTemperature`, `requestedFanPercent`, `appliedFanPercent`, `activeCurveSegment` (−1 none / 0 below / 1–3 interpolating / 4 above), `thermalControlReason`, `emergencyOverrideActive`, `hysteresisHolding`, `controlSensor`, `controlSensorValid`, and `fanCurveError` (present only while a persisted curve is rejected).

**PATCH:** `thermalControlMode` (strict enum), `fanCurve` (structured array only — never free-form text), `fanCurveHysteresis` (0–8). The whole request fails on any invalid field; a mode save syncs `autofanspeed` for rollback safety. `openapi.yaml`, the generated Angular models and the dev mocks stay aligned (the TypeScript build fails on divergence).

## 7. Control-loop implementation (Stage 7)

All decision logic lives in the new pure component **`components/thermal_control/`** (no heap, no RTOS, no hardware — host- and QEMU-testable): mode resolution, curve parse/validate/serialize, stateless evaluation, and the stateful hysteresis step. `fan_controller_task.c` only dispatches: emergency/paused/no-pool branches unchanged at 100 ms; thermal configuration (mode/curve/hysteresis) refreshes from NVS at a 1 s cadence because string reads allocate and the hot loop must stay allocation-free (a config save applies within ≤ 1 s; emergency inputs are still checked every cycle). CURVE reuses the same EMA filter as TARGET; PWM writes go through the existing `update_fan_speed()` (clamps, fault flagging). Build note: `main` declares explicit `PRIV_REQUIRES`, which disables ESP-IDF's implicit main-requires-all — `thermal_control` is therefore listed there.

**22 new QEMU test cases** cover: exact points, every interpolation segment, below-first/above-final plateaus, min-fan floor (including clamping), full-fan final point, rising/falling hysteresis, rapid alternation, immediate upward response mid-hold, gated downward staircase, zero and clamped hysteresis, invalid curves (every reject class), serialize round-trip, mode parsing, legacy migration, missing sensor, sensor loss mid-run, impossible temperature, and startup before telemetry. TARGET/MANUAL regressions are covered by the preserved code paths plus the passing legacy frontend suites.

## 8. Tuning & Thermal UI (Stages 8–9)

Explicit three-mode selector (nothing switches modes automatically). TARGET: target-temperature and minimum-fan sliders as before. CURVE: numeric per-point editor (keyboard-accessible, immediate per-field and cross-field validation, no free-form JSON), SVG curve preview with the firmware's plateaus and a live control-temperature marker (blanked for invalid curves instead of drawing a misleading shape), hysteresis slider with a plain-language explanation ("speed increases are never delayed"), minimum-fan floor, and the live decision row (control temp, requested vs applied fan, segment, driving sensor, state with EMERGENCY/ fallback warnings). MANUAL: the existing slider plus an explicit "does not disable thermal protection" note. Current→Pending review lists every change (all thermal fields correctly labeled "Applies after Save" — the firmware applies them live), Revert restores the device baseline, Save/Apply & Restart stay explicit, and Save is blocked while the pending curve is invalid.

**Curve templates** (pending-editor fills only; no auto-save, no auto-restart; Custom appears the moment any value diverges): Quiet `48/25 55/40 60/60 64/100`, Balanced = the firmware default, Aggressive Cooling `40/35 48/60 54/85 60/100`. Every template still reaches 100 % fan at or before 64 °C — none trades protection for silence, and none is labeled "safe". Frequency/voltage presets remain a separate, unchanged system.

## 9. Command Deck integration (Stage 10)

Thermal & Power panel gains a compact decision statrow (mode, control temperature, requested/applied fan, curve segment in curve mode, sensor, state — red only for genuine EMERGENCY). The Advanced Telemetry drawer shows the configured thermal mode, curve summary and hysteresis. Neural Insights adds one transparent line from tested logic: "Curve control stable" (with hold detail), "Target control active", "Manual fan active", "Waiting for valid sensor data", "Curve configuration invalid — safe fallback active" (warning), or "Emergency thermal override" (the only red state; suppressed when the existing overheat insight already owns it). The hashrate hero is untouched.

## 10. Compatibility and rollback (Stage 11)

- **OTA 2G → 2H:** all devices keep TARGET or MANUAL exactly; curve is opt-in.
- **Rollback 2H → 2G:** the Phase 2G pair (`v2.14.2-17-g4cded376`) remains the immediate OTA rollback pair. Old firmware ignores the three additive keys (harmless unknown NVS entries; non-destructive in both directions). Because `autofanspeed` is kept in sync, a device rolled back while in CURVE mode lands in automatic PID control — never an unattended manual state.
- **Re-upgrade** restores the stored curve/mode/hysteresis unchanged. No destructive migration exists anywhere in the path.

## 11. Phase B pipeline results (Stages 12–13)

| Step | Result |
|---|---|
| Clean commit verified | `88ccb619…`, exactly the 26-file Phase A set, tree clean |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **286 / 286 / 286** (Edge headless, regenerated API each run) |
| Production frontend build | clean; 2.32 MB initial / 430.7 kB gz |
| Full ESP-IDF build (v5.5.3 container) | clean; `esp-miner.bin` 1 658 272 B, 60 % app-partition free |
| App descriptor | `v2.14.2-19-g88ccb619`, not dirty — APP DESC OK |
| Release pair | firmware app_desc == revision embedded in `www.bin` — PAIR OK |
| Merged factory image | 15 802 368 B (bootloader+partition table+app+www+otadata) |
| QEMU unit tests | **83 Tests 0 Failures 0 Ignored** (rerun from the clean commit) |
| Release export | 4 artifacts staged, EXPORT OK |
| Manifest validation | VALIDATION OK (board Gamma/601/BM1370; sizes and SHA-256 verified) |
| Secrets / local-path scan | clean (no machine paths, no dump patterns, no credentials) |
| Screenshot privacy scan | CLEAN (mock data only, sensitive-data masking on; gate enforced by the export tool) |

Pipeline note: host git abbreviates this commit to 7 hex chars while container git uses 8; the release pair must carry one identity, so `version.txt` is regenerated inside the build container (the same `git describe` the version generator runs) before `www.bin` is packed.

**Artifacts** — `NeuralAxe Build Artifacts/thermal-control-v0.1.0-dev-board601/`:

```
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin      3 145 728 B  7d7c99fe…f767a9f
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin      1 658 272 B  6a039a9d…79761ffb
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin 15 802 368 B  937a5c9a…1ecdf693
release/config-601.cvs                                       975 B  3c0f28f6…a55f22
release/…-manifest.json, …-SHA256SUMS.txt
screenshots/ (10 sanitized captures) · screenshot-privacy-scan.md · pipeline-evidence/
```

## 12. Unresolved hardware validations

This build is **not declared generally safe or final** until controlled tests on the tuned Gamma prove: actual fan/PWM response and RPM behavior per curve segment, acoustic stability of the hysteresis ratchet, closed-loop temperature stability, real sensor-failure response, emergency override on hardware, and OTA migration plus 2G rollback on the device.

## 13. Hardware access confirmation

No COM/USB access, no miner IP contact, no esptool/bitaxetool against hardware, no flashing/reboot/identify/monitoring of a physical device, and the private TCH flashdump was never inspected. All validation used source analysis, host-side tests, containers, QEMU and the mock dev server.
