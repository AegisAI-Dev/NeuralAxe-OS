# NeuralAxe OS Phase 2W — Gate W6/W6.1
## Weather Recommendation Pilot Preparation and Authoritative Mutation Observability Report

Phase: Weather-Aware Tuning, Gates W6 and W6.1 · Branch: `neuralaxe-v0.1-weather-recommendation-pilot`
Committed implementation: **4fab888** ("feat: add authoritative weather-pilot diagnostics and mutation observability") = **v2.14.2-77-g4fab888f**, parent `f4aee75` (the committed Gate W5 report). 37 files changed, 6,286 insertions, 22 deletions.
Phase B verification date: 2026-08-04 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gates W6 + W6.1 FULLY GREEN. The Gate W6 software verdict is READY.**

Gate W6 built the pilot's eyes: a pure, bounded diagnostic vocabulary and a read-only invariant monitor whose every input carries explicit provenance. Its own audit then found that **six of eleven required safety facts had no authority to read**, and it refused to invent counters inside the observer — self-reported evidence is not authority. That produced an honest **NOT READY**.

Gate W6.1 closes the gap at the boundaries that own each mutation, and replaces the tuning-equality decision with an exact canonical comparison after a 32-bit digest was found unfit to carry a safety decision.

The commit authorizes nothing. No frequency, voltage, fan or thermal change; no pool or protocol change; no timed session, lease, B5 ownership or B7 execution; no SNTP start; no HTTP route; no new task, timer or queue; no persistence; no restart. Every claim below is measured on the committed tree, not inferred.

| Check | Result |
|---|---|
| Committed files | exactly **37** — 23 added + 14 modified |
| Ancestry | `4fab888` → `f4aee75` (W5 report) → `a4a0d6b` (W5); `74a6178` (W4) and `d256deb` (B10) are ancestors |
| `test-ci/` in the commit | **0 files**; stub 22 bytes, `sdkconfig.defaults` unchanged |
| Frontend / OpenAPI in the commit | **0 files** |
| W1–W5 and B3–B10.2 components in the commit | **0 files** |
| Split QEMU — baseline (B1–B10.2) | **851 Tests, 0 Failures, 0 Ignored** |
| Split QEMU — weather (W1–W5 + W6 + W6.1) | **368 Tests, 0 Failures, 0 Ignored** |
| **Deduplicated total** | **1219 / 0 failures / 0 ignored** |
| Frontend `npm ci` + test gate | exit 0 · **1270 / 1270** |
| Production frontend build | exit 0 |
| Build-helper tests | **36 / 36** |
| Firmware postures A–H | all exit 0 (§7) |
| Strict compile `-Wall -Wextra -Werror` | **15 / 15 translation units**, 0 failures |
| Default image (posture A) | **0 symbols, 0 bytes** of static RAM from the new component |
| Canonical release pair | firmware app_desc = web `version.txt` = `git describe` = **v2.14.2-77-g4fab888f** |
| Privacy / coordinate / credential / owner-path scans | clean |
| Working tree after verification | clean apart from this report |

## 2. What the two gates deliver

**Gate W6 — `components/weather_pilot_diag/` (9 files).** Twenty bounded event tokens, edge-triggered so a multi-hour wait produces one line, plus a summary rate-limited to once per 60 s on monotonic time. Twenty invariant codes reporting the first violation in a fixed order. The load-bearing property is the provenance model: every posture field carries `ABSENT` / `OBSERVED` / `STRUCTURAL` / `UNAVAILABLE` / `STALE`, and the checker evaluates **provenance before values** — a zeroed structure reports `WX_INV_FACT_ABSENT` rather than sailing through on convenient zeros.

**Gate W6.1 — `components/mutation_observability/` (8 files) + `main/nx_mutation_adapter.{c,h}`.** Nine saturating monotonic counters incremented **inside the subsystem that performs each mutation**, a coherent single-lock snapshot, pure delta arithmetic, a canonical tuning snapshot with exact comparison, and a RAM-only pilot baseline behind a fourteen-part readiness predicate.

Only **273 of the 6,286 inserted lines** land in existing production files; the rest is new, independently testable code and its tests.

## 3. The mutation boundaries

`nvs_config_set_*` is the single persistent-configuration writer, so it is the only place a stored configuration change can occur. All **7 setters** are instrumented immediately **after** each one's existing no-op equality guard, which yields the "accepted effective change" semantic for free — verified in source, not asserted:

```
set_string          guard < note < xQueueSend    OK
set_string_indexed  guard < note < xQueueSend    OK
set_u16             guard < note < xQueueSend    OK
set_i32             guard < note < xQueueSend    OK
set_u64             guard < note < xQueueSend    OK
set_float           guard < note < xQueueSend    OK
set_bool            guard < note < xQueueSend    OK
```

**All 8 `esp_restart` sites in the image** are recorded, not merely the operator-initiated routes — 2 HTTP, 2 BAP, 2 self-test, 1 stratum retry, 1 in `components/stratum/stratum_api.c` — each immediately **before** the irreversible call. Both OTA acceptance points are recorded before their irreversible step (`esp_ota_begin` for firmware, `esp_partition_erase_range` for the web image).

**Configuration mutation is not runtime activity.** Fan PWM, the boot frequency ramp, the initial voltage application, pool reconnects on unchanged configuration, protocol transitions, share submission and trusted-time synchronisation never reach the writer. `nvs_config_init` loads the cache through the raw `nvs_get_*` API and bypasses the instrumented writer entirely, so a boot is not a mutation.

**A counter proves THAT, never WHO.** The overheat-protection path writes a permanently derated frequency, voltage, fan mode and overheat flag; that is a genuine mutation and it is counted, so the pilot fails loudly and the owner investigates. The invariant proven is "nothing changed at all" — the stronger claim, and the only one these counters support.

**Key classification: 41 assignments over 40 distinct keys** of 72. The 32 unclassified keys are named in source (Wi-Fi credentials, hostname, display, theme, scoreboard, best difficulty, self-test thresholds, hardware inventory); a catch-all would turn a theme change into a safety violation. `NVS_CONFIG_OVERCLOCK_ENABLED` increments **both** frequency and voltage because it moves both envelopes.

## 4. The canonical tuning snapshot

An earlier revision of W6.1 reduced the configuration to one 32-bit FNV-1a digest and compared digests. **That was wrong as a safety decision** — a 32-bit non-cryptographic hash cannot be shown to give every distinct valid configuration a distinct value, and per-field "changing X changes the hash" tests do not eliminate collisions. A collision would have let a real tuning change compare equal to the baseline.

The authority is now an exact comparison of canonical values. `nx_tuning_snapshot_equal()` contains, with comments stripped, **zero `memcmp`, zero digest reads and 13 explicit field comparisons** plus a per-point loop:

| # | Field | Type · units | Serialized (BE) | Validity |
|---|---|---|---|---|
| 1 | `version` | u32 | `[0..3]` | must match |
| 2 | `size` | u32 | — | must match |
| 3 | `valid` | bool | `[4]` | explicit |
| 4 | `frequency_mhz_x10` | u16 · 0.1 MHz | `[5..6]` | >0, ≤20000 |
| 5 | `voltage_mv` | u16 · mV | `[7..8]` | >0, ≤2000 |
| 6 | `fan_mode` | u16 · 0 manual / 1 auto | `[9..10]` | ≤1 |
| 7 | `fan_percent` | u16 · % configured | `[11..12]` | ≤100 |
| 8 | `fan_min_percent` | u16 · % | `[13..14]` | ≤100 |
| 9 | `fan_hysteresis_c` | u16 · °C | `[15..16]` | ≤150 |
| 10 | `temp_target_c` | u16 · °C | `[17..18]` | >0, ≤150 |
| 11 | `overheat_mode` | u16 · 0/1 | `[19..20]` | ≤1 |
| 12 | `thermal_mode` | u16 · stable enum | `[21..22]` | < `__COUNT` |
| 13 | `curve_point_count` | u16 | `[23..24]` | 0 or 4 |
| 14 | `curve[4]` | 4 × (u8 °C, u8 %) | `[25..32]` | ≤100 %; all-zero when count 0 |

Canonical length **33 bytes**, pinned by `_Static_assert`. The struct holds **no pointer**, so it is copyable by value and no parse buffer can survive into the baseline.

**Fan curve.** Parsed by the committed `thermal_curve_parse()` + `thermal_curve_validate()` into the fixed-width `ThermalCurve` (4 ordered points, whole °C and %), then copied into the canonical array. The text is freed in the adapter and never reaches the baseline. `NX_TUNING_CURVE_POINTS == THERMAL_CURVE_POINTS` is asserted in the one translation unit that knows both models, so drift is a build failure. "No curve" has exactly one representation — count 0 with every slot zeroed; a stale leftover point makes the snapshot invalid.

*Honest limit:* the committed grammar `"v1;T:P;T:P;T:P;T:P"` admits no optional whitespace or alternate forms, so there is no equivalent text formatting for canonicalisation to absorb. What the tests pin instead is that the **parsed points, not the text**, are what is stored and compared.

**Thermal mode.** Mapped to a stable `NxTuningThermalMode` enum **explicitly, never by numeric cast**, so a change to the committed `ThermalControlMode` values cannot silently remap a stored baseline.

**The digest survives only as a bounded diagnostic.** It has **no production caller**, is never stored in the snapshot, is never consulted by the equality decision, and is nowhere described as collision-free. Test `W61-T10` forces both digests to `0xDEADBEEF` and proves the equality decision is unaffected.

## 5. The baseline readiness predicate

A delay is not a readiness test. The 120-second monotonic settle is **one of fourteen** prerequisites, evaluated in fixed order by a separate pure predicate:

| Block code | Prerequisite | Source |
|---|---|---|
| `ALREADY_READY` | not already captured | internal |
| `VIOLATION_LATCHED` | no violation latched | permanent latch |
| `HISTORY_LOST` | no lost history latched | permanent latch |
| `SETTLING` | uptime ≥ 120 s monotonic | caller |
| `CONFIG_NOT_LOADED` | boot configuration loaded | provider registered after `nvs_config_init()` |
| `HOST_TASK_INVALID` | host task is the expected one | latched task handle + stack floor |
| `RUNTIME_UNAVAILABLE` | B5/B6 snapshot available | `pool_session_runtime_snapshot()` |
| `OWNER_PRESENT` | lease owner exactly NONE | `snap.lease_owner` |
| `PROTOCOL_POSTURE` | normal-source protocol posture | `snap.protocol` + `snap.mining_policy` |
| `SESSION_POSTURE` | no session or recovery posture | `session_present`, `restore_required`, `persistence_pending`, `recovery_error` |
| `TERMINAL_PENDING` | no pending terminal result | `snap.state != RUNTIME_TERMINAL_PENDING` |
| `COUNTERS_UNREADABLE` | counter snapshot readable | snapshot validity |
| `COUNTER_SATURATED` | no counter saturated | `saturated_mask` |
| `COUNTER_NONZERO` | every counter still **zero** | all-zero check |
| `TUNING_UNREADABLE` | canonical tuning readable | provider |

**Refusal semantics.** A refusal stores nothing — no counters, no tuning, no timestamp. There is no state in which a half-formed baseline can be compared against. `zero_at_capture` is now an **invariant** of a READY baseline rather than a recorded variable, because a non-zero counter refuses outright.

**Never retried into success.** Counters only rise, so a mutation before the window opened blocks the baseline permanently for that boot. A violation or lost history latches permanently and blocks capture outright. A reboot clears RAM and begins a completely new lifecycle — which is deliberate, since after a restart the counters restart at zero and a carried-over baseline would compare against an unrelated history.

**Recovery-by-restoration is structurally unavailable.** Because the baseline is all-zero by construction, restoring the counters after a violation would otherwise make the delta compare equal again. Once `violation_latched` is set, `nx_mutation_baseline_compare()` never returns true again for that boot.

## 6. Where the pilot runs

The periodic observation creates **no task, timer or queue**. It is called from the already-existing statistics task, which starts only in the normal mining posture (system initialised, ASIC initialised, not self-test). If that task does not exist the device is not in the pilot's scenario either, and the absence of lines — never a healthy-looking line — is what reports it.

Each call attempts the once-per-boot baseline capture on monotonic time, re-reads the authorities, runs the invariant monitor and emits at most one summary per 60 s plus any violation. It performs no NVS write, no network request and no recovery. Its only allocation is the two bounded configuration strings the getters hand out, each freed on every path.

**A defect in the Gate W6 boot line was found and fixed:** it asserted `hardware=unchanged pool=unchanged` **before any baseline existed** — precisely the proposition the pilot exists to prove. It now reports `mutation_claims=pending_baseline`.

**Gate W6 had no periodic caller at all** — one boot line and nothing further, so a baseline could never mature. That is why the observation hook exists.

## 7. Postures A–H (committed tree, all exit 0)

| P | Flags | bin bytes | any sym | increment | counter state | pilot sym | mut RAM |
|---|---|---|---|---|---|---|---|
| A | none (default) | 1 658 416 | **0** | **0** | **0** | 0 | **0 B** |
| B | observability | 1 659 376 | 2 | 1 | 3 | 0 | 48 B |
| C | W4 | 1 668 816 | 0 | 0 | 0 | 0 | 0 B |
| D | + W5 | 1 670 496 | 0 | 0 | 0 | 0 | 0 B |
| E | + W6 pilot | 1 677 504 | 7 | **0** | 2 | 14 | 124 B |
| F | + observability | 1 679 360 | 12 | 1 | 4 | 14 | 168 B |
| G | + timed sessions | 1 714 784 | 12 | 1 | 4 | 14 | 168 B |
| H | + execution + API | 1 760 320 | 12 | 1 | 4 | 14 | 168 B |

**A raw symbol count is too coarse to prove isolation** — the component's pure parts (snapshot comparison, delta arithmetic, canonical tuning, baseline token) compile unconditionally and link wherever a caller uses them. The audit therefore measures the counter storage (`s_counters`, `s_lock`, `s_baseline`, `s_reader`) and the increment separately. **Posture E is the load-bearing case**: the pilot is enabled without the counters, so it has **no increment and no counter storage**, and it honestly reports `mut_obs=0` with the mutation facts stamped `UNAVAILABLE` and the invariant `FACT_UNAVAILABLE` — an unobservable pilot that says so.

Posture A links nothing at all from the new component: the call site in `main.c` is guarded as well as the implementation.

## 8. Test coverage

**57 new tests** across the two gates' W6.1 work, plus the 26 Gate W6 pilot tests:

| Suite | Count | Covers |
|---|---|---|
| `test_nx_mutation_counters.c` | 18 | counter semantics, snapshot contract, saturation, deltas, accepted-then-failed operations |
| `test_nx_mutation_baseline.c` | 22 | the fourteen-part predicate, refusal, no-recapture, latches, comparison verdicts |
| `test_nx_tuning_snapshot.c` | 12 | canonical schema, exact equality, curve points, padding, digest-collision seam, provider seam |
| `test_nx_weather_pilot_diag.c` | 31 | W6 diagnostics and invariant monitor (26) + W6.1 integration (5) |

Properties proven rather than asserted: saturation is exercised through the real increment via a deliberate test-only preset, because reaching `UINT32_MAX` by counting is infeasible and a hand-built struct would prove nothing about the increment itself. Padding is proven not to participate by poisoning two structs with different byte patterns before setting identical values. A digest collision is simulated through an explicit test seam.

## 9. Release identity

| Artefact | Value |
|---|---|
| `git describe --tags --abbrev=8` | `v2.14.2-77-g4fab888f` |
| Firmware app_desc (posture A) | `v2.14.2-77-g4fab888f` |
| Web `dist/axe-os/version.txt` | `v2.14.2-77-g4fab888f` |
| Default factory image | 1 658 416 bytes |

All three match by **exact string equality**, which is the canonical release-pair contract.

## 10. Verdicts

**Software: READY.** Every required pilot invariant has an authoritative live source at the boundary that owns it. No required fact is a hardcoded value, a configuration-derived assumption or a caller default. Absence, unavailability, a missing baseline, lost counter history and an unreadable configuration are each reported as their own distinct unhealthy state.

**Artifact: NOT BUILT.** No pilot image exists. Building one requires private values the owner has not supplied and this work has not chosen.

**Physical pilot: NOT READY.** It stays not ready until the owner supplies the private values, builds the artifact, completes the §6 checklist of the preparation document and confirms its §5 rollback readiness — including the offline rollback verification and the read-only `nx_tps` store inspection. Software readiness is a precondition, never a substitute.

## 11. Limits to carry forward

- **The observation window is explicit, not assumed.** The pilot proves nothing about the interval before `base_us`; the diagnostic line reports that uptime and whether the boot reached it with no counted change.
- **The counters attribute nothing.** They prove a class of configuration changed, never who changed it.
- **A regression against the baseline is unreachable in production.** The baseline is all-zero and counters only rise, so saturation is the reachable lost-history case; the regression guard remains as defence in depth against a snapshot from a different authority.
- **The pilot observes only while the statistics task runs.** That is the normal mining posture, and it is the pilot's scenario — but it is a dependency, and it is stated rather than hidden.
- **The `thermalControlMode` setting is reachable from the generic settings PATCH loop**, which does not enforce the settings table's length range. The adapter therefore parses it with the committed strict parser rather than trusting the stored text.
