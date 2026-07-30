# NeuralAxe OS — Gate W1: Pure Profile and Climate Policy — Phase B Report

Phase: Weather-Aware Tuning, Gate W1 · Branch: `neuralaxe-v0.1-weather-aware-tuning-policy`
Committed implementation: **397f636** ("feat: add pure weather-tuning profile and climate policy") = **v2.14.2-56-g397f636**, parent a31975a (v2.14.2-55).
Phase B verification date: 2026-07-31 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gate W1 FULLY GREEN.** The committed pure component `components/tuning_profile/`
delivers the W1 contract — stable profile metadata model, validated/compatible
profile contract, climate hysteresis state machine, seven-level policy
precedence, manual-override eligibility, sensor-health input model,
thermal/stability override inputs, pure profile-selection intents, bounded
machine codes, exhaustive unit tests — under the owner's Phase A corrections:
**no production profile is VALIDATED, no voltage ceiling is claimed, and
production descriptors carry no tuning payload.** All Phase B checks ran
against the exact committed HEAD (clean tree before and after every run).

## 2. Committed diff (verified)

`git diff --stat a31975a..397f636` — exactly 9 files, 4,275 insertions, 1 deletion:

| File | Lines |
|---|---|
| `components/tuning_profile/include/tuning_profile.h` | 403 |
| `components/tuning_profile/include/tuning_policy.h` | 491 |
| `components/tuning_profile/tuning_profile.c` | 507 |
| `components/tuning_profile/tuning_policy.c` | 788 |
| `components/tuning_profile/test/test_tuning_profile.c` | 669 |
| `components/tuning_profile/test/test_tuning_policy.c` | 1405 |
| `components/tuning_profile/CMakeLists.txt` / `test/CMakeLists.txt` | 8 / 3 |
| `test/CMakeLists.txt` (root) | 1 line: `tuning_profile` added to TEST_COMPONENTS |

No file under `main/` changed; no existing frequency/voltage validation path
changed; no report document was included in the code commit (two-gate
workflow restored).

## 3. Phase B verification results (all against 397f636)

| Check | Result |
|---|---|
| QEMU unit suite (esp32s3, IDF v5.5.3, QEMU 9.2.2) | **453 Tests, 0 Failures, 0 Ignored** — 80 `tuning_profile` cases + intact 373-test baseline (> 373 requirement met). "FAIL" substrings in the serial log are B1 state names (`TARGET_FAILED`/`RESTORE_FAILED`) inside passing titles; 0 non-title matches |
| Frontend gate (`npm run test:ci`, Brave headless) | **1052 / 1052 SUCCESS**, exit 0 |
| Default firmware build (`GITHUB_ACTIONS=true idf.py build`) | **OK** — `esp-miner.bin` 1,658,272 B (byte-count identical to the pre-W1 baseline; the pure component links nothing into `main/`) |
| Feature-enabled build (`CONFIG_NX_TIMED_SESSIONS=y`) | **OK** — `#define CONFIG_NX_TIMED_SESSIONS 1` confirmed in generated config; same image size (flag wires no runtime behavior, per B1 contract) |
| Strict warnings (`gcc -std=c11 -Wall -Wextra -Werror`, both sources) | **Clean** |
| Forbidden-symbol scan (heap/printf/log/NVS/clock/FreeRTOS/network/restart/driver) | **Clean** — only documentation words match (purity banners) |
| Secret / local-path / URL scan | **Clean** — no credentials, wallets, hostnames, URLs, or machine paths |
| Line endings | No `.gitattributes`; `core.autocrlf=true`; every committed W1 blob is **LF** (0 CR bytes), matching every existing tracked source — repository convention preserved, no normalization or config change was needed |
| Git writes by the assistant | **None** — the code commit was made by the owner via GitHub Desktop; only read-only git commands ran |
| Hardware / LAN / pools / NTP / live weather access | **None** — all builds and tests ran on read-only-mounted container copies |

Runner note (mirrors `unittest.yml` locally): repo mounted read-only into
`espressif/idf:v5.5.3`, tree copied in-container, `test-ci/CMakeLists.txt`
symlink restored from `test/`, stale generated `test-ci/sdkconfig` dropped,
16 MB merged flash image, `qemu-system-xtensa -machine esp32s3`.

## 4. Safety contract of the committed implementation

1. **No production profile is VALIDATED.** All three Gamma 601 registry
   descriptors — `supersink-max` (rank 2), `hot-weather-safe` (rank 1),
   `emergency-thermal-safe` (rank 0) — ship `TUNING_VALIDATION_UNVALIDATED`
   with `evidence_fingerprint 0`. A currently running operating point is not
   validation evidence; browser-local Stability Lab records are not firmware
   evidence. Promotion requires committed, owner-approved physical validation
   evidence supplied by a later validated profile source. Only synthetic test
   fixtures use VALIDATED. Proven by tests:
   - `registry: every production profile is UNVALIDATED with no evidence`
   - `eligible: every production profile is refused for auto-selection`
     (each of the three → `TUNING_ELIGIBLE_ERR_NOT_VALIDATED`)
   - `evaluate: production registry can never emit a profile selection` —
     across sensor-failure, emergency, fail-safe, hot-forecast,
     cool-forecast and manual-override stances, no intent ever carries a
     profile id (only `RETAIN_INHIBIT` / `OPERATOR_RECOVERY` / `NONE`)
   - `eligible: matching the running miner's numbers confers nothing` — a
     synthetic UNVALIDATED 625 MHz/1150 mV profile is refused (voltage
     fail-closed rule and validation state independently)
2. **No voltage ceiling is claimed.** `TuningBoardCapability.voltage_window_known
   == false` for Gamma 601 (no voltage numbers encoded anywhere in production
   code); any voltage-bearing payload fails closed with
   `TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN`. Regulator (TPS546) acceptance
   limits are nowhere described as safe tuning limits. The 625 MHz maximum is
   documented strictly as the **current UI/product option ceiling** (BM1370
   dropdown maximum) — not hardware-safety proof — and 626 MHz payloads are
   rejected. Bounded payload mechanics (frequency window, voltage window,
   fan-curve, thermal-limit ordering) are exercised only through
   `tuning_profile_validate_with_capability` with synthetic injected windows.
3. **Payload separation.** Production descriptors are payload-free
   (`payload_present == false`; `ERR_PAYLOAD_NOT_EMPTY` rejects any covert
   nonzero payload field, tested per field). The arbitrator's
   `TuningSelectionIntent` carries stable profile IDs only — no W1 output
   applies or exposes raw tuning values.
4. **Precedence** (unchanged from the approved design): 1 EMERGENCY_THERMAL →
   2 STABILITY_OR_REBOOT_ROLLBACK → 3 SENSOR_INTEGRITY_FAILURE →
   4 WEATHER_API_OR_TIME_FAIL_SAFE → 5 MANUAL_OVERRIDE_WITHIN_SAFETY_CEILING →
   6 NORMAL_WEATHER_POLICY → 7 CURRENT_VALIDATED_PROFILE. First-active-wins;
   safety sources only strictly downgrade rank or move from an unknown
   current; fail-safe retains Emergency when already there and never falls
   back toward the cool-day profile; upgrades require trusted time, sensors
   upgrade-ok, expired cooldown, STABLE mining, no inhibit, known current,
   auto-eligible target, strictly higher rank (ownership acquisition is the
   W4 runtime gate on top of the intent).
5. **Climate hysteresis**: deci-degree integers; enter HOT at ≥ 300 (30.0 °C),
   leave only at ≤ 280 (28.0 °C); invalid ordering rejected; dead band
   retains the stance; unusable/stale/out-of-band forecasts fail-safe without
   mutating hysteresis state; idempotent under repeated forecasts; custom
   thresholds proven consumed; saturating transition counter; alias-safe.
6. **Sensor-health model** encodes the audited firmware failure modes:
   -1 read sentinel → INVALID; ~127 °C EMC2101 diode faults → IMPLAUSIBLE
   (cap 1200 d°C); frozen TPS546 cached value → STALE via unchanged-streak;
   single-observable-fan reality (tach loss blocks upgrades only; PWM-command
   uncertainty is an integrity failure).
7. **Manual override**: full task-brief rejection list (inactive, actor,
   no-expiration, expiration ordering, expired, untrusted time, unknown /
   not-eligible profile, above effective safety ceiling, ceiling-unknown
   fails closed) plus the conservative cooldown rule (upgrades rejected
   during an active cooldown; safer-or-equal always allowed). A safety-
   preempted active override reports `TUNING_OVERRIDE_NOT_EVALUATED`, never
   "inactive". Weather-only upgrade gates deliberately do not bind overrides;
   the ceiling and safety sources always do.
8. **Purity + persistence pins**: no I/O, heap, clock reads, logging,
   FreeRTOS, NVS or networking anywhere in the component (scans clean;
   includes are exactly `string.h`/`stdint.h`/`stdbool.h`/`stddef.h` + own
   headers). Every persistence-facing enum value and count is
   `_Static_assert`-pinned — the Gate W2 encoding contract. No authenticated
   human identity is claimed anywhere (request-source actor classes only).

## 5. Engineering notes for later gates

1. `TUNING_REASON_WEATHER_RETAIN_BAND` and `TUNING_REASON_OVERRIDE_REJECTED`
   are declared, string-mapped and count-pinned but not yet emitted by the
   arbitrator — reserved for the W2+ status/audit surfaces (documented here
   to preempt a "declared but unused" review note).
2. The W1 test suites contain the only VALIDATED profiles in the tree
   (synthetic fixtures, clearly commented). Any future promotion mechanism
   for production profiles belongs to the W2 persistence design plus an
   owner-approved validation-evidence source — never a code-default change.
3. `tuning_profile_validate` (static table) and
   `tuning_profile_validate_with_capability` (injected window) are the same
   validator; W2 persistence must reject records whose stored profiles fail
   either path exactly as tests do today.
4. Generated `sdkconfig.h` expresses Kconfig booleans as `#define X 1` — grep
   for `=y` inside it always returns 0 (harmless verification gotcha hit in
   Phase A/B build checks).

## 6. Scope confirmations

No network, NVS, scheduler, API, frontend, profile application, frequency- or
voltage-limit change; no edits outside the component + one test-registry
line; no Git write operations by the assistant in either phase; no hardware,
owner-LAN, real-pool, NTP, OTA or live weather access; the private TCH
recovery dump was not touched.

## 7. Owner action (second gate)

Commit this report (suggested: `docs: add Phase W1 pure profile and climate
policy report`). After that, Gate W1 is closed; reply `continue W2` to begin
Gate W2 — persistence and the crash-safe profile transaction model (B3
dual-slot pattern in a new namespace, versioned settings, last-known-safe
record, boot-recovery decision, fake power-loss + QEMU NVS tests).
