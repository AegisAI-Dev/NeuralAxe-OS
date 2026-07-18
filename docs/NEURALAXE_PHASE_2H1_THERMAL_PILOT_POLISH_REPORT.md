# NeuralAxe OS — Phase 2H.1 Thermal Pilot-Polish Report

**Phase:** 2H.1 — Thermal Hardware-Pilot Closure, Mode-Aware UX and OTA Filename Compatibility
**Branch:** `neuralaxe-v0.1-thermal-pilot-polish` (parent: `neuralaxe-v0.1-thermal-control`)
**Commit:** `6b4e7c74989746b426a8475d5c5561d26e3ccdaa` — "feat: mode-aware thermal status and OTA filename compatibility" (owner-committed)
**Build identity:** `v2.14.2-21-g6b4e7c74` (clean pair — verified inside `esp-miner.bin` app descriptor *and* inside `www.bin`)
**Target:** Gamma / board 601 / BM1370 / ESP32-S3 N16R8
**Verdict:** **PASS** — UX, compatibility, documentation and release hardening only; the hardware-proven fan-control firmware is bit-identical to the pilot build's algorithm.

---

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Curve mode no longer presents Target-PID warnings as active control failures | PASS — the deck pill derives from the reported mode; the pilot state renders "Curve control active — hysteresis hold", never "Above target" |
| Mode-aware thermal messaging accurate | PASS — tested pure derivation (`modeAwareThermalStatus`) with 12 state tests incl. the exact pilot observation |
| Hysteresis hold visible and understandable | PASS — pill + detail "Holding 70 % while the curve requests 68 % · segment P2 → P3", plus the existing statrow State column |
| Release-export filenames upload directly | PASS — `*-www.bin` / `*-ota.bin` accepted alongside the legacy names; staged flow with explicit Install |
| Dangerous image types remain rejected | PASS — factory/merged/bootloader/partition-table/ota_data/config rejected with stated USB-recovery reasons; dangerous markers outrank acceptance suffixes |
| No thermal-control algorithm change | PASS — zero firmware sources in the commit (10 frontend files only); QEMU thermal suite unchanged |
| Frontend tests pass (> 286) | PASS — **321/321** ×3 consecutive clean-`npm ci` runs (+35 new) |
| QEMU thermal tests green (≥ 83/83) | PASS — **83 Tests, 0 Failures, 0 Ignored**, rerun from the clean commit |
| Matching clean firmware/web pair | PASS — `v2.14.2-21-g6b4e7c74` in app_desc, inside `www.bin`, and in `version.txt` |
| No hardware access | PASS — source, tests, mocks, QEMU and owner-supplied pilot evidence only |

## 2. Hardware-pilot evidence (Stage 3 — owner-observed, recorded verbatim)

Pilot configuration: firmware `v2.14.2-19-g88ccb619`, Fan Curve mode, curve 45→25 / 52→45 / 58→70 / 64→100 %, hysteresis 2 °C, 625 MHz, 1150 mV. Observation after ≈16 minutes: ASIC 57 °C, VRM 52 °C, requested fan 68 %, applied fan 70 %, ≈6967 RPM, state "Hysteresis hold", segment P2 → P3, 1.23 TH/s, 0.00 % ASIC errors, 109 ms pool latency, 35 accepted / 0 rejected shares.

**Recorded as PASSED on hardware:**

- Legacy TARGET-mode migration (device retained target semantics after the 2H OTA until curve was explicitly enabled).
- CURVE mode opt-in (explicit owner save required; nothing switched automatically).
- Curve interpolation at 57 °C producing ≈65 % — matches the deterministic segment P2→P3 math (45 % + (57−52)/6 × 25 ≈ 65.8 %).
- Interpolation + hysteresis at 58 °C: curve request ≈68–69 %, applied 70 %, state "Hysteresis hold" — the rising-instant / falling-gated ratchet behaving exactly as the QEMU tests predict.
- Real PWM/RPM response (≈6967 RPM at 70 % duty on the stock Gamma fan).
- Stable ASIC/VRM temperatures (57 / 52 °C sustained at 625 MHz / 1150 mV).
- Stable mining (1.23 TH/s, 0.00 % ASIC errors, healthy share flow) and pool operation (109 ms latency).
- NVS setting retention across the OTA (Wi-Fi, pools, tuning, thermal settings preserved).

**Explicitly NOT claimed as physically tested — OWNER-GATED OR NON-DESTRUCTIVE TEST NOT YET PERFORMED:**

failed temperature sensor · emergency overheat override · hard shutdown · VRM emergency threshold · destructive rollback · factory reset.

## 3. Changed files (10, all frontend)

**New:** `components/update/update-file-check.ts` (+spec) — pure filename classifier.
**Modified:** `components/command-deck/deck-intel.ts` (+spec) — `modeAwareThermalStatus`; `command-deck.component.{ts,html}` — mode-aware pill, mode-aware ASIC-gauge amber threshold, Target-Temp bar → Control-Temp bar outside target mode; `components/edit/edit.component.html` — live strip swaps Thermal-Delta/Target tiles for Control-Temp/Thermal-Mode tiles outside target mode; `components/update/update.component.{ts,html,spec.ts}` — staged install flow and copy.

## 4. Mode-aware thermal status (Stage 1)

One tested pure helper (`modeAwareThermalStatus`, deck-intel.ts) now drives the Command Deck thermal pill. Precedence: emergency override (red) → fixed 70 °C overheat line (red) → curve-invalid fallback (amber) → degraded sensor (amber) → per-mode status:

- **TARGET** (configured target authoritative): "Below target" / "At target — PID tracking" / "Above target" (amber), with "fan near saturation" at ≥95 % duty.
- **CURVE** (target never presented as objective): "Curve control stable" with control temperature, fan % and segment; "Curve control active — hysteresis hold" with requested vs applied; "Fan near saturation" (amber) only at ≥95 % applied.
- **MANUAL:** "Manual fan active" (cyan) with set/applied percentages and "thermal protection remains active"; "High temperature — manual fan" (amber) from 65 °C — 5 °C before the overheat line, because manual mode has no automatic response.

Semantic rules preserved: green/cyan = normal, amber = saturation/degraded/fallback, red = genuine emergency or overheat only; the user accent cannot alter these. The configured target remains visible in the tuning form as configuration, and the deck/tuning live strips show the effective control temperature instead of target-based deltas outside target mode. The 2H thermal statrow (mode, control temp, requested/applied, segment, sensor, state) already carried the Stage 4 diagnostics and is unchanged in scope — no new card walls.

## 5. OTA filename compatibility (Stage 2)

Pure classifier `update-file-check.ts`: **accepted** — `www.bin` / `*-www.bin` (web uploader), `esp-miner.bin` / `*-ota.bin` (firmware uploader), case-insensitive; **rejected with stated reasons** — factory, merged, bootloader, partition-table, ota_data images, `.cvs`/`.csv` configs, wrong-uploader submissions (told which uploader to use), and arbitrary/unknown `.bin` names. Dangerous markers take precedence over acceptance suffixes (`factory-www.bin` → factory).

Selecting a file now **stages** it — filename, detected type and reason are shown, and the upload starts only on the explicit "Install Web Interface" / "Install Firmware" button (Cancel discards). No automatic upload, no automatic install, endpoint separation and backend OTA mechanics untouched. Page copy lists the NeuralAxe release-export names as first-class. 23 new tests cover the full accept/reject matrix and the staged flow (spy-verified: selection never triggers an upload).

## 6. Pipeline results (Phase B)

| Step | Result |
|---|---|
| Clean commit verified | `6b4e7c74…`, exactly the 10-file set, tree clean |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **321 / 321 / 321** |
| Production web build | clean |
| Full ESP-IDF build (v5.5.3 container) | clean |
| App descriptor | `v2.14.2-21-g6b4e7c74`, not dirty — APP DESC OK |
| Release pair | firmware app_desc == revision inside `www.bin` — PAIR OK |
| Merged factory image | 15 802 368 B |
| QEMU thermal regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean |
| Screenshot privacy scan | CLEAN (12 captures, mock data, masking on) |

**Artifacts** — `NeuralAxe Build Artifacts/thermal-pilot-polish-v0.1.0-dev-board601/`:

```
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin      3 145 728 B  2b294657…68b2c184
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin      1 658 272 B  7c83bb5a…92690192
release/NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin 15 802 368 B  54af2a02…dd03217e3
release/config-601.cvs                                       975 B  3c0f28f6…a55f22
release/…-manifest.json, …-SHA256SUMS.txt
screenshots/ (12 sanitized captures) · screenshot-privacy-scan.md · pipeline-evidence/
```

The staged `…-www.bin` and `…-ota.bin` are exactly the filenames the updated Update page accepts directly — no manual renaming remains in the OTA path.

## 7. Remaining hardware validations

Unchanged from §2: sensor-failure response, emergency overheat override, hard shutdown, VRM emergency threshold, destructive rollback and factory reset remain owner-gated. The Phase 2G pair remains the deep rollback pair; the 2H pair (`v2.14.2-19-g88ccb619`) is the proven pilot baseline this build supersedes for OTA.

## 8. Hardware access confirmation

No COM/USB access, no miner IP contact, no esptool/bitaxetool, no flashing or rebooting of hardware, and the private TCH recovery dump was never inspected. All hardware facts in this report are the owner's supplied pilot observations, recorded without extrapolation.
