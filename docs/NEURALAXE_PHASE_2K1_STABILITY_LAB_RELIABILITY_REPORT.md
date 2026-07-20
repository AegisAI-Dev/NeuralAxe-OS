# NeuralAxe OS — Phase 2K.1 Stability Lab Reliability Report

**Product:** NeuralAxe OS 0.1.0-dev
**Branch:** `neuralaxe-v0.1-stability-lab-reliability`
**Release pair:** **v2.14.2-35-gf6e6a15** (firmware app-desc == embedded www == version.txt)
**Primary target:** Gamma · board 601 · BM1370
**Verdict:** ✅ **PASS** — Phase A (implementation, at commit gate) and Phase B (build/release pipeline) both green. No firmware source changed; no hardware or live network accessed.

---

## 1. Objective

Harden the Stability Lab so real-world browser scheduling, visibility changes, telemetry gaps and sample jitter cannot silently produce misleading results — the failure the real Gamma-601 pilot exposed (Session A finished a 600 s window with only 10 accepted samples, 8.3 % coverage, yet was presented as complete). This is a focused reliability phase: no autonomous optimization, no automatic voltage search, no background services.

## 2. Root cause of the 10-samples-in-600 s (8.3 %) session

Evidence collection was driven by a `setInterval`-based timer, not by telemetry arrival. The only thing that built a measurement sample was `onSessionTick()`, fired by `interval(HEARTBEAT_MS = 5000)` — RxJS `interval` schedules on `setTimeout`. When the tab is not foreground-visible, Chromium clamps `setInterval`/`setTimeout` to roughly once per minute, so a 600 s hidden window fired the heartbeat ~10 times → ~10 samples → 8.3 %.

Freshness never aborted it because `LiveDataService` keeps telemetry flowing while hidden (HTTP poll drops to 60 s; WS updates buffer through `bufferTime(500)`), so at each throttled heartbeat `lastFreshMonoMs` had just been refreshed → age < 15 s → "online". The freshness contract (built to detect a *dead* device) worked correctly; it never enforced *coverage*.

## 3. Capability / firmware conclusion

**No firmware or backend change.** The entire fix is browser-side, over existing APIs (`GET /api/system/asic`, the shared telemetry stream, `PATCH /api/system`, `POST /api/system/restart`). Every protected firmware behavior (supported-device gate, tuning endpoints, hard thermal protection, emergency override, sensor-validity handling, Target/Curve/Manual modes, OTA, Fleet, restore-original) is untouched. Verified: zero `.c/.h/CMakeLists` modified; the firmware QEMU unit suite passes 83/0/0 unchanged.

## 4. Reliability architecture

### 4.1 Event-driven, arrival-identity sampling
- **`TelemetryArrivalService`** (`src/app/services/telemetry-arrival.service.ts`) mints a **monotonic `arrivalId`** at a single shared boundary: `map(++seq)` above `shareReplay({ bufferSize: 1, refCount: false })`. `refCount: false` guarantees exactly one upstream subscription for the app lifetime, so the counter advances once per genuine emission and every replay / late-subscriber observes the *same* id (with `refCount: true`, a re-subscription would re-run the map and mint a new id for a replayed arrival — the dedup would break).
- **`stability-intake.ts`** deduplicates on **arrival identity, not payload content** (`considerArrival`). Content-difference dedup is unsafe: a genuinely new reading may be byte-identical to the previous one (stable temps/power/fan/counters) and must still count. Rules: accept iff the payload carries core telemetry, `arrivalId > lastAcceptedArrivalId`, and at least the cadence (minus jitter) of **monotonic** time has elapsed.
  - Two byte-identical genuine arrivals → two samples · a replay / re-subscription / reconnect replay → one sample · a re-render → no emission → no sample · HTTP-fallback and WebSocket use the same contract · a gap payload is never evidence.
- Freshness re-stamps only on a **new** arrival carrying core telemetry — genuine arrivals, not payload changes. The heartbeat now only supervises deadlines/staleness; it builds no evidence.

### 4.2 Visibility contract (`stability-visibility.ts`)
Explicit Page Visibility tracking via `@HostListener('document:visibilitychange')`: visibility state, interruption count, total & longest hidden duration, and whether warm-up / measurement was affected — all on the monotonic clock. Becoming visible never refreshes freshness and never resets a timer. A hidden, low-coverage run can never be labelled Completed.

### 4.3 Monotonic time model
All runtime calculations are monotonic (`performance.now`): phase elapsed (`phaseStartMono` + `phaseDurationMs`, replacing the old wall-clock `phaseDeadline`), telemetry freshness, reconnect grace, hidden-duration, gap duration, and the **maximum session-duration cap** (`maxSessionDurationMs`, `mono − sessionStartMono > cap`, checked before the phase-deadline logic so it wins). A late supervise callback crosses at most one deadline per call — never skipping states. `Date.now()` is used **only** for human-readable history `startedAt`/`finishedAt` and timeline timestamps. A system wall-clock jump (forward or backward) can neither trip the cap early nor extend it.

**Maximum wall-clock contract:** `min(6 h, plannedMs × 2 + 10 min)` — the tested profile is never left active indefinitely under throttling; on breach the session aborts and restores the original configuration.

### 4.4 Coverage semantics (`stability-coverage.ts`)
Target = `round(windowMs / cadence)`; coverage = `min(100, valid / target × 100)`, capped; plus median sample interval, max gap, total gap. Presented as **"121 valid samples · target 120"** — never "121/120". **No valid sample is ever discarded to force an exact ratio.**

### 4.5 Completion / partial classification (`stability-results.ts`)
Full window + coverage ≥ **90 %** ⇒ Completed. Below ⇒ **Partial with an explicit reason** (e.g. *"Partial — telemetry coverage 8.3 % (10 valid samples, target 120). Page hidden ~9 min during measurement…"*). The badge itself carries the coverage — never a bare "Partial". Aborted / Failed / Insufficient each state their reason. Comparison badges are awarded **only to Completed** (sufficient-coverage) results — a Partial or Aborted result is never crowned "best".

### 4.6 Conservative board-601 defaults & honest starters
- Operator-session stop defaults: ASIC 68 °C, **VRM 70 °C** (was 100 °C), error 5 %, reject 8 %, debounce 3, fan-saturation off. `migrateThresholds` bumps only an **all-legacy-default** stored config (VRM 100 → 70); any owner-customised field (including a deliberate VRM value) is preserved. Firmware hard thermal protection is unchanged and never replaced.
- Starter profiles never offer same-frequency/higher-voltage (the 625/1250 case) — omitted with an explicit note; Performance requires a genuine frequency increase or is omitted with a reason; labels describe the real diff. The owner's 625 MHz pilot result is **not** encoded as a universal recommendation.

### 4.7 History / export evidence + migration
Sanitized history & exports add cadence, valid/target, coverage, median interval, max/total gap, visibility (interruptions / total / longest / hidden-during-measurement), the exact status reason, max session-duration contract, and restore-verified. No IP/SSID/pool/wallet/worker/credentials/raw payloads; privacy mode still strips hostname. Legacy Phase 2K records load safely via `normalizeRecord`/`normalizeResult`.

## 5. Regression fixtures (sanitized, `stability-fixtures.ts`)
- **A** — 10 valid / target 120 → Partial, reason states 8.3 % coverage, no false Completed.
- **B** — 121 valid / target 120 → Completed, "121 valid samples · target 120", coverage capped 100 %, no off-by-one.
- **C** — visibility interruption with fresh telemetry continuing → hidden recorded, no fabricated gaps.
- **D** — visibility interruption then stale telemetry → abort only after the reconnect grace.
- **E** — same-frequency / higher-voltage → no Performance starter.

## 6. Phase A verification

| Check | Result |
|---|---|
| Frontend tests (`npm run test:gate`) | **667 / 667, exit 0** (was 594) |
| Production build (AOT / strictTemplates) | exit 0 (pre-existing NG8102 warnings only) |
| Deterministic screenshots (Puppeteer + Edge) | 28 captured |
| Screenshot privacy scan | **PASS** — 0 leaks |
| Bundle (initial) | 2.54 MB < 3 MB error budget (2 MB warning pre-existing) |
| Firmware sources | unchanged |

Key in-browser proof: with byte-identical harness polls, the live coverage panel accumulates ("5 valid samples · target 6, median 5.0 s") instead of sticking at 1 — the arrival-identity fix working end-to-end.

## 7. Phase B verification (build & release pipeline)

Owner committed the change set as `f6e6a156` ("feat: Stability Lab reliability — arrival-identity sampling, monotonic timing, honest coverage").

| Step | Result |
|---|---|
| Clean commit | `version.txt` = `v2.14.2-35-gf6e6a15`, no `-dirty` |
| `npm ci` | exit 0 (1016 packages) |
| Frontend tests ×3 (`test:gate`) | 667 / 667 each, exit 0 |
| Production frontend build | exit 0 |
| ESP-IDF v5.5.3 build (`GITHUB_ACTIONS=true idf.py build`) | exit 0; `esp-miner.bin` 1,658,272 B, `www.bin` 3,145,728 B |
| App-descriptor validation (`--check-bin`) | **APP DESC OK** — v2.14.2-35-gf6e6a15 |
| Firmware/web pair (`--check-pair`) | **PAIR OK** — firmware and web both v2.14.2-35-gf6e6a15 |
| Merged factory image (`merge_bin.sh`) | 15,802,368 B |
| Release export (`export_release.py`) | **EXPORT OK** — 4 artifacts + manifest + SHA256SUMS |
| Manifest validation (`validate_manifest.py`) | **VALIDATION OK** — Gamma/601/BM1370 |
| Release-gate battery (`test_release_gates.py`) | **13 passed, 0 failed** |
| QEMU (`esp32s3`, Unity) | **83 Tests / 0 Failures / 0 Ignored** (`report.xml tests="83" failures="0"`) |
| Secrets / local-path scan | clean (2 env-overridable dev-tool path defaults in the screenshot harness only) |
| Screenshot privacy scan | **PASS** (28 screens) |

Release artifacts (`sourceRevision == firmwareRevision == webRevision == v2.14.2-35-gf6e6a15`):
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` (3,145,728 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` (1,658,272 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` (15,802,368 B)
- `config-601.cvs` (975 B)
- `…-manifest.json`, `…-SHA256SUMS.txt`

## 8. Change set (24 files, all under `main/http_server/axe-os/`)

**New (10):** `src/app/services/telemetry-arrival.service.ts` (+spec); `src/app/components/stability-lab/` — `stability-intake.ts` (+spec), `stability-visibility.ts` (+spec), `stability-coverage.ts` (+spec), `stability-fixtures.ts` (+spec).

**Modified (14):** `stability-preflight.ts`, `stability-stop.ts` (+spec), `stability-profile.ts` (+spec), `stability-results.ts` (+spec), `stability-history.ts` (+spec), `stability-lab.component.ts` (+spec), `stability-lab.component.html`, `stability-lab.styles.scss`, `scripts/capture-screenshots.mjs`.

## 9. Real-hardware pilot plan (owner-run, post-merge)

On the Gamma 601, repeat the A/B/C comparison keeping the tab foregrounded, and confirm: (i) a foreground run reaches ~100 % coverage → Completed; (ii) a deliberately-backgrounded run reads Partial with the coverage/hidden reason, never Completed; (iii) the stop panel shows VRM 70 °C; (iv) no misleading Performance starter appears at max frequency; (v) restore-original is verified. Also confirm byte-identical steady-state telemetry still accumulates full coverage (the arrival-identity fix on real hardware).

## 10. Limitations

This is one controlled reliability phase. It does not add autonomous tuning, and it does not guarantee permanent hardware stability. Firmware hard thermal protection remained active throughout and was never replaced. The pre-existing NG8102 template warnings and the ~2 MB bundle-size warning are unchanged and out of scope.

---

*Evidence (screenshots, privacy scan, build/QEMU logs, release artifacts, manifest, SHA256SUMS) archived outside git at `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\stability-lab-reliability-v0.1.0-dev-board601`. No hardware, COM/USB, live network, esptool/bitaxetool-against-hardware, or private TCH dump was accessed.*
