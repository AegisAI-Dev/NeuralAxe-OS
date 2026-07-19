# NeuralAxe OS — Phase 2K Stability Lab Report

**Product:** NeuralAxe OS 0.1.0-dev · **Target:** Gamma / board 601 / BM1370
**Branch:** `neuralaxe-v0.1-stability-lab` · **Commit:** `84dbdd2` (clean, no `-dirty`)
**Web build identity:** `version.txt = v2.14.2-33-g84dbdd2`

**Verdict:** **FINAL PASS** — an opt-in, controlled profile-benchmarking Stability
Lab that tests owner-selected tuning profiles with measured evidence. No blind or
autonomous overclocking, no automatic voltage escalation, no automatic promotion.
Every capability reuses existing APIs; **zero firmware sources changed.**

### Release-gate summary (all PASS)

| Gate | Result |
|---|---|
| Clean owner commit | `84dbdd22` — *feat: Stability Lab — controlled profile benchmarking with measured evidence* |
| Expected revision | **`v2.14.2-33-g84dbdd2`** (no `-dirty`) |
| Frontend tests | **594 / 594 / 594** (×3) via `test:gate`, exit 0 each |
| Production web build | PASS |
| Full ESP-IDF v5.5.3 build | PASS |
| App descriptor | **`v2.14.2-33-g84dbdd2`** |
| Firmware/web pair | **MATCH** |
| Merged factory image | **15 802 368 B** |
| Release artifacts | www **3 145 728 B** · ota **1 658 272 B** · factory **15 802 368 B** · config 975 B · manifest · SHA256SUMS |
| Manifest validation | **PASS** (Gamma / board 601 / BM1370) |
| **QEMU regression** | **83 Tests · 0 Failures · 0 Ignored · exit 0** |
| Secrets / local-path scan | clean |
| Screenshot privacy scan | **PASS** (23 captures) |
| Firmware sources | **unchanged** (zero `.c/.h/CMake/Kconfig/sdkconfig/partition` edits) |
| Hardware / live network | **none accessed** |

---

## 1. What the Stability Lab does

An owner queues 1–5 tuning profiles, the Lab validates them, runs each through a
transparent state machine, samples real telemetry during warm-up and measurement,
stops safely when configured limits are crossed, restores the original
configuration by default, and lets the owner promote a tested profile only through
an explicit, separate action. Results use transparent per-metric badges — never a
single opaque "stability score". History and exports are sanitized; nothing is
cloud-synced.

The owner, not NeuralAxe, remains responsible for choosing a profile.

## 2. Capability conclusion — no firmware/backend change

Every function runs through existing endpoints:

| Need | Existing API |
|---|---|
| Apply a profile | `PATCH /api/system` (`SystemApiService.updateSystem`) |
| Telemetry | `LiveDataService.info$` (WS + 5 s poll) — snapshotted on a timer; **no second polling loop** |
| Restart (only if ever needed) | `POST /api/system/restart` |
| Frequency/voltage option truth | `GET /api/system/asic` |

**No new backend endpoint. Firmware sources are byte-identical to the parent
(2J.1).**

## 3. Supported-device gate

Executes only on a **NeuralAxe-Managed Gamma / board 601 / BM1370**, gated on the
DECLARED build target (`targetBoard`/`targetDevice`/`targetAsic` + `productName`),
**not** the runtime `boardVersion` string (a Gamma-601 reports physical PCB "602").
Stock AxeOS, board 702, BM1368, undeclared targets, offline devices, invalid
sensors, emergency state and pair mismatches are all shown a read-only explanation
and cannot execute a session.

## 4. Key safety mechanisms (all pure and unit-tested)

- **Telemetry freshness contract** (`stability-freshness.ts`): "online" means a
  genuinely-new sample arrived within **15 s** on a **monotonic** receipt clock
  (`performance.now()`, age clamped ≥ 0 so a clock change can't fabricate a fresh
  or negative age). Only non-gap samples re-stamp the receipt time; a gap payload
  and a bare re-render never do. Stale telemetry **blocks preflight**; mid-session
  staleness past the limit + a **30 s reconnect grace aborts** the run, and a fresh
  sample within grace resumes it.
- **Preflight** (`stability-preflight.ts`): 11 named checks, each with its own
  explanation — no opaque score. A run starts only when every applicable check
  passes.
- **State machine** (`stability-machine.ts`): pure 14-state FSM, valid transitions
  only, timestamped timeline, no silent skipping; an interrupted browser session
  becomes **"operator session interrupted", never "complete"**; route navigation
  during a run is guarded.
- **Stop conditions** (`stability-stop.ts`): emergency override and invalid sensor
  abort immediately; ASIC/VRM over-temp, sustained mining pause, opt-in fan
  saturation, ASIC error and reject rate (after the ≥100-total-or-≥3-rejected
  confidence gate) use a debounce. Conservative defaults; **tighten-only**, clamped
  to safe ceilings (ASIC ≤ 70 °C, VRM ≤ 105 °C). A telemetry-loss **gap sample can
  no longer fabricate** a "sensor invalid" abort (the gap is short-circuited before
  the immediate checks). Firmware hard protection is never replaced.
- **Restore-original** (`stability-lab.component.ts`): the captured baseline is
  re-applied by default after completion/abort/recoverable-failure, with reported
  success/failure and an owner retry; the last tested profile is never left active.
- **Results** (`stability-results.ts`): transparent aggregates + badges
  (Completed / Partial / Aborted:<reason> / Insufficient samples / comparative
  Highest-hashrate / Lowest-efficiency / Lowest-temp / Lowest-variability) and
  qualified statements ("evidence from one session, not a lifetime guarantee").
- **Promotion**: owner-only, explicit dialog with the diff, evidence and
  one-session caveat; **aborted/partial/failed are never promotable.**
- **History/export** (`stability-history.ts`): bounded to 20 sessions; stores only
  target labels, hostname and sanitized configs/thresholds/results/timeline —
  **never IP/SSID/pool/wallet/worker/credentials/raw responses**; JSON/CSV/Markdown
  export with the required disclaimer; privacy mode removes the hostname.

## 5. Real planned-restart count (audited)

Every field `profileToSettings()` emits was audited against repo truth
(`edit.component` `noRestartFields` + `fan_controller_task.c`'s ≤ 1 s re-read):
`frequency, coreVoltage, thermalControlMode, autofanspeed, temptarget, minFanSpeed,
manualFanSpeed, fanCurve, fanCurveHysteresis` — **all apply live**. The session plan
computes the restart count from the **real diff** (`sessionRestartCount`), not an
assumption: **0** for supported profiles, and the state machine routes
`APPLY_RESTART` only if a restart-only field ever changes.

## 6. Phase B pipeline results

| Step | Result |
|---|---|
| Clean commit verified | `84dbdd2`; `version.txt = v2.14.2-33-g84dbdd2` (no `-dirty`) |
| `npm ci` | clean from committed lockfile (exit 0) |
| Frontend tests ×3 | **594 / 594 / 594**, gate **exit 0** each (`npm run test:gate`) |
| Production web build | **exit 0** (pre-existing budget + NG8102 warnings only); 2.52 MB / ~469 kB transfer |
| Identity cross-check | `neuralaxe_identity.h` == `src/app/neuralaxe.ts` (NeuralAxe OS / 0.1.0-dev / NeuralShield / v2.14.2 / 601 / Gamma / BM1370) |
| Release-gate battery (`test_release_gates.py`) | **13 / 13** — pair match, dirty/none/multi-revision rejection, stale build-order detection, manifest mismatch/dirty/missing-web/local-path rejection |
| Secrets / local-path scan | shipped `src/` **clean** (no host paths, no tokens, no secrets); 2 env-overridable path *defaults* in the dev-only screenshot harness (noted §9) |
| Screenshot privacy scan | **PASS** — 23 captures, 0 leaks (`screenshots/privacy-scan.json`) |
| Full ESP-IDF build (v5.5.3 container) | **clean, exit 0** — `git describe = v2.14.2-33-g84dbdd2` (7-char, not dirty); `esp-miner.bin`, `www.bin`, `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` generated |
| App-descriptor validation | `esp-miner.bin` app_desc == **`v2.14.2-33-g84dbdd2`** (`export_release.py --check-bin` → APP DESC OK) |
| Matching firmware/web pair | **PAIR OK `v2.14.2-33-g84dbdd2`** — app_desc == revision embedded inside `www.bin` (`--check-pair`) |
| Merged factory image | **15 802 368 B** (`merge_bin.sh` → `esp-miner-merged.bin`) |
| Release export | **EXPORT OK** — 4 artifacts (`www` 3 145 728 B, `ota` 1 658 272 B, `factory` 15 802 368 B, `config` 975 B) + manifest + SHA256SUMS |
| Manifest validation | **VALIDATION OK** (Gamma/601/BM1370, 4 artifacts) |
| **QEMU firmware regression** | **83 Tests · 0 Failures · 0 Ignored · exit 0** — actually run (see §7) |

## 7. QEMU regression — actually executed (83/0/0)

The full release pipeline (build → app-desc → pair → merge → export → manifest)
and the **QEMU firmware regression** were **actually executed and passed** in ESP-IDF
containers once Docker Desktop's Linux engine came up. Two build notes: the main
firmware container was mounted at `/workspace` to reuse the existing build cache,
and `git config --global core.abbrev 7` was set so the firmware's git-describe
matched the 7-char `version.txt`, giving a coherent pair.

The QEMU run reproduced the repository's official process
(`.github/workflows/unittest.yml` / `neuralaxe-release.yml`): `test-ci` built with
`espressif/idf:v5.5.3`, then merged and run in the official action's image
(`espressif/idf:v5.5.4` + `esp-develop-9.2.2` Xtensa QEMU + Unity 2.6.1 + Ruby),
built locally from `bitaxeorg/esp32-qemu-test-action`:

```
esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o flash_image.bin @flash_args
timeout 5m qemu-system-xtensa -machine esp32s3 -nographic -no-reboot \
  -watchdog-action shutdown -drive file=flash_image.bin,if=mtd,format=raw -m 4 -serial file:output.log
ruby /opt/Unity-2.6.1/auto/parse_output.rb -xml output.log        # → report.xml
```

Result — `report.xml`: `<testsuite name="Unity" tests="83" failures="0" skips="0">`;
`output.log`: `83 Tests 0 Failures 0 Ignored`; the CI failure-check gate exited 0.
No result was normalized, suppressed or reinterpreted.

**Windows symlink handling (tracked tree byte-identical):** `test-ci/CMakeLists.txt`
is a POSIX symlink to `../test/CMakeLists.txt` materialised as a text file on
Windows. It was **not edited**; a byte-identical copy of the target's content was
**bind-mounted over** the path in the container only, and the build wrote to the
gitignored `test-ci/build/`. The owner's tracked working tree is unchanged. Raw
evidence: `pipeline-evidence/qemu_output.log`, `report.xml`, `qemu-run.log`,
`qemu-environment.md`.

## 8. Changed files (33 total — 23 new + 10 modified; zero firmware)

**New (23):** `src/app/components/stability-lab/` — `stability-freshness`,
`stability-history`, `stability-machine`, `stability-preflight`, `stability-profile`,
`stability-results`, `stability-stop`, `stability-telemetry` (each `.ts` + `.spec.ts`),
`stability-lab.component.ts/.html/.spec.ts`, `stability-lab.styles.scss`;
`scripts/test-gate.mjs`, `scripts/capture-screenshots.mjs`;
`src/app/guards/stability-lab.guard.ts`.
**Modified (10):** `package.json`, `package-lock.json` (puppeteer-core devDep +
`test:gate`), `app-routing.module.ts`, `app.module.ts`, `layout/app.menu.component.ts`,
`layout/app.menu.component.spec.ts`, `layout/app.sidebar.component.spec.ts`,
`components/command-deck/command-deck.component.ts` + `.html`, `styles.scss`.

## 9. Unresolved / owner-gated

- **Firmware pipeline** — build, app-desc, pair, merged image, release export,
  manifest validation, **and QEMU (83/0/0)** all **ran and passed** for
  `v2.14.2-33-g84dbdd2`. No outstanding release gates.
- **Dev-tooling path defaults** — `scripts/capture-screenshots.mjs` has two
  host-path *default fallbacks* (`NX_SHOT_DIR`, `EDGE_PATH`/`CHROME_BIN`),
  env-overridable and never shipped in firmware or release artifacts. Benign; may
  be relativized in a follow-up if preferred.
- **Real-hardware pilot** (owner-gated): queue Current + one conservative variant on
  the tuned Gamma 601 (625 MHz / 1150 mV, Fan Curve), warm-up 3 min / measure 10 min,
  ASIC stop 68 °C; confirm live-apply (0 restarts), watch warm-up→measure→restore,
  verify restore matches baseline, export the report, confirm no stop tripped at the
  normal reject rate.
- Pre-existing initial-bundle budget warning is unchanged; no 3 MB error budget
  breached.

## 10. Hardware and network access confirmation

No hardware or live network was accessed: no miner IPs contacted, no network scans,
no COM/USB, no esptool/bitaxetool against a device, no TCH dump. All evidence used
source, mocked/intercepted HTTP, deterministic synthetic telemetry, unit tests, a
local static server driving headless Edge, and read-only `.git` metadata.

---

Release/evidence artifacts (outside git) in
`…/NeuralAxe Build Artifacts/stability-lab-v0.1.0-dev-board601/`:
`release/` (www 3 145 728 B, ota 1 658 272 B, factory 15 802 368 B, config,
manifest, SHA256SUMS — pair `v2.14.2-33-g84dbdd2`), `screenshots/` (23 +
`privacy-scan.json`), `VISUAL_REVIEW_PHASE_A.md`, `PHASE_B_PIPELINE_EVIDENCE.md`.
