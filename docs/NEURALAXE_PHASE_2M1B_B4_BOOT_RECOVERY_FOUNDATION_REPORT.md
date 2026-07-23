# NeuralAxe OS — Phase 2M.1B, Gate B4

## Pure Boot-Recovery Decision Engine Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-recovery` · **Commit:** `4584e4e` — `feat: add pure boot-recovery decision engine` (**v2.14.2-52-g4584e4e**, parent `cfd045b` = the committed Gate B3 report; descends from `7e85a41` / `463417d` / `e72db15`)
**Scope:** deterministic, side-effect-free boot-recovery decision layer only — **no runtime wiring** (no scheduler tasks, queues, boot hooks, `esp_reset_reason()` calls, NVS, store writes, SNTP, Wi-Fi, pool mutation, Stratum control, `esp_restart()`, HTTP, OTA blocking, or frontend). No production runtime behavior changed; nothing instantiates or calls the engine.

Architectural sources of truth: the committed Phase 2M.1A reports (audit §13/§15, state-machine §6/§7, security §9) and the B1/B2/B3 foundation reports.

---

## 1. Verdict

**Gate B4 PASS (Phase A + owner-directed eligibility correction + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 9 approved B4 files, 2275 insertions (see §8) |
| Runtime wiring | **none** — zero references outside `components/pool_session_recovery/`; no task/queue/boot hook; nothing calls the engine; linker-discardable like B1–B3 |
| Forbidden APIs | none — no `esp_reset_reason()` call (comment mentions only), no `nvs_*`, no `esp_restart`, no networking anywhere in the component |
| B1/B2/B3 sources | untouched; **57 + 82 + 47 + 29** all green in the same run |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**, unchanged |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **332 Tests, 0 Failures, 0 Ignored — exit 0** (83 base + 57 + 82 + 47 + 29 + **34 `[pool_recovery]`**) |
| Main app, flag OFF / ON | both exit 0 (temp sdkconfig copies; tracked config untouched) |
| Module warnings | all 4 new translation units **clean under `-Wall -Wextra -Werror`** |
| Secret / local-path scan | clean; synthetic `*.example` fixtures only |
| Tree after verification | clean; `test-ci/CMakeLists.txt` restored byte-exact (22 B); no `report.xml`; no release artifacts |

---

## 2. What Gate B4 delivers

A new self-contained component **`components/pool_session_recovery/`**:

- `include/pool_session_reset.h` + `pool_session_reset.c` — the pure, NeuralAxe-owned **reset classification**: 10 classes mapped from the 16 pinned raw `esp_reset_reason_t` values (ESP-IDF v5.5.3), with total pure helpers (expected/abnormal, counter effects, may-resume, conservative-restore, B3 record-class mapping, clean tokens). `pool_session_reset_espidf.c` **compile-asserts every pin against the real enum** and provides an uncalled classify wrapper — `esp_reset_reason()` is never invoked in B4; the Gate B5+ integrator reads it once and classifies the raw value.
- `include/pool_session_recovery.h` + `pool_session_recovery.c` — the pure engine: `PoolSessionBootContext` (store result, immutable record reference, reset class, bounded sync-wait elapsed/limit, the B2 `PoolTimeSnapshot`, provider-initialized flag, and the explicit **mining-inhibition capability**) → `PoolSessionRecoveryPlan` (decision, allowed configuration, mining policy, persistence intent, runtime intent — descriptions only — machine error + reason, remaining seconds, safety flags, a deterministic FNV-1a plan fingerprint, the counter proposal and the bounded record proposal).

### 2.1 Reset policy

Expected = {POWER_ON, SOFTWARE}. Abnormal (consumes the consecutive-failure budget) = {PANIC (incl. CPU_LOCKUP), TASK/INTERRUPT/OTHER WATCHDOG, BROWNOUT (incl. PWR_GLITCH), UNKNOWN (incl. EFUSE and any future value)}. Conservative-restore, never resume = {DEEP_SLEEP, EXTERNAL (EXT/SDIO/USB/JTAG), UNKNOWN}. Crash-class resets may reach the TARGET_ACTIVE eligibility path only **within** the bounded budget — repeated crashes exhaust it and force operator recovery, never a reboot loop. A reset class alone never authorizes anything.

### 2.2 Total decision tables

**Store results (16 + future-value guard):** `EMPTY`/`CLEARED` → normal source boot, source mining allowed, nothing persisted. `OK` → record re-validated (kind + full B3 semantic validation) then the state table. Every other result → the conservative plan (`RECOVERY_REQUIRED`, `CURRENT_CONFIG_UNVERIFIED`, `VERIFY_BEFORE_MINING`, `SURFACE_OPERATOR_RECOVERY`, no record update) with distinct machine codes; a `default:` case plus a `_Static_assert` pin on `POOL_STORE_RESULT__COUNT` prevent silent fall-through for future results. **Only STORE_OK reaches state planning.**

**Persisted states (total over all 17; 11 reachable):** pre-mutation snapshot → boot source + propose `CANCELLED`; `APPLYING_TARGET`/`TARGET_FAILED`/`INTERRUPTED` → `RESTORE_SOURCE_NOW` + propose `RESTORE_DUE` (interruptions drive to restoration, never target retry); `RESTORE_DUE`/`APPLYING_RESTORE` → restore (idempotent source reapply per the audit reconcile rule); `TARGET_ACTIVE` → the time path (§2.3); `RESTORE_FAILED` → bounded re-attempt below budget, operator recovery at exhaustion; `RECOVERY_REQUIRED` → bounded restore when the obligation is held and budgeted, else retain + operator; `COMPLETE`/`CANCELLED` → `RETAIN_TERMINAL_SOURCE` (B4 never clears; no tombstone path exists). Ephemeral states are unreachable through the B3 validator and land in the conservative plan.

### 2.3 TARGET_ACTIVE: eligibility, never authorization (owner-corrected)

The resume predicate requires **all** of: valid SESSION record · `restore_required=true` · the full persisted target-verification triple · valid identities · valid persisted UTC deadline **with non-zero sync generation** · valid verified-start · budgets not exhausted · a resume-permitting reset class · a provider-initialized, trusted, `TIME_OK` snapshot **not below the persisted floor** · and a B2 `RESUME_TARGET_WITH_REMAINING_TIME` with non-zero remaining. Even then the plan is **eligibility, not authorization**: `RESUME_VERIFIED_TARGET` (documented as *"eligible for live target verification and possible resumption; mining is not yet authorized"*) + `TARGET_ONLY` + **`VERIFY_BEFORE_MINING`** + `VERIFY_TARGET_CONFIGURATION`, with `restore_required` still true. B4 operates only on persisted facts — pre-reboot verification does not prove the live post-boot configuration still matches the persisted target — so **`ALLOW_TARGET_MINING` is unreachable from the pure engine** (reserved for the Gate B6/B7 live-verification layer; only its token-table string exists; all three property sweeps assert it is never emitted).

Time mapping: B2 `WAIT` → bounded `WAIT_FOR_TRUSTED_TIME` (`NO_POOL_ALLOWED`, `INHIBIT_MINING`, `inhibit_target_stratum`, non-terminal, no new duration) — and if the runtime cannot guarantee mining inhibition, **no wait is emitted**: immediate restore with `MINING_INHIBITION_UNAVAILABLE`. B2 `RESTORE_DUE` → restore (`DEADLINE_EXPIRED`). B2 `FAIL_SAFE` → restore with the mapped reason (`TIME_TIMEOUT`, `DEADLINE_MISSING`, `TIME_REGRESSION`, …); a missing UTC deadline restores immediately with zero waiting. B2 `RECOVERY`/unknown → conservative. The B2 policy floor (`pool_session_recovery_build_time_policy`) is the persisted `latest_accepted_trusted_epoch` when valid, the audit-permitted `verified_start_epoch` fallback otherwise; a "trusted" snapshot earlier than the floor is a rejected trust claim (`TIME_REGRESSION` → bounded wait → fail-safe).

### 2.4 Counters and proposals (invariant 21)

Counter values are **proposals** derived from the record's persisted values via the B3 saturating helpers: reboot +1 per boot with a record; consecutive +1 on abnormal classes; attempt +1 only when the plan itself is a restore/verify action. Prospective exhaustion blocks eligibility and converts recovery actions into `RECOVERY_REQUIRED` + `SURFACE_OPERATOR_RECOVERY` — no plan can request a reboot (no such intent value exists; token-scanned). The engine is idempotent: the same context yields a byte-identical plan (fingerprint included) with no double increment — the caller must persist the proposal through the B3 store **before** re-evaluating or executing the associated external action. The record proposal carries only `{proposed_state, proposed_failure_code}` (+ counters) and can only name `RESTORE_DUE`, `RECOVERY_REQUIRED`, or pre-mutation `CANCELLED` — structurally incapable of touching identities, epochs, generation, slots, or the obligation.

---

## 3. Test suite (34 `[pool_recovery]` tests → QEMU 298 ⇒ 332)

Reset model (raw mapping totality incl. invalid values; expected/abnormal/conservative set exactness; counter-effect helpers; record-class mapping; clean tokens) · store-result table (EMPTY/CLEARED normal boot; all 13 non-OK results conservative with specific codes; OK demands a valid SESSION record; contradictory EMPTY+record; null-argument safety) · persisted-state table (snapshot-committed cancellation; interruption restores; restore-side continuation; terminal retention; RESTORE_FAILED bounded-retry/exhaustion split; RECOVERY_REQUIRED obligation split; ephemeral rejection) · TARGET_ACTIVE time (eligibility plan with exact remaining; **persisted-evidence-never-authorizes-mining**; at/past deadline restores; bounded untrusted wait with exact boundary; missing deadline immediate restore; floor construction + floor-gated trust with regression wait-then-failsafe; sync-generation/provider/status structural failures; counter exhaustion; conservative reset classes restore while watchdog-within-budget still reaches the time decision) · wait contract (inhibits everything, grants nothing, non-terminal; inhibition-unavailable → immediate restore) · counters (single increment + idempotent repeat; saturation; action-gated attempts; abnormal progression to bounded exhaustion) · record-proposal bounds (input immutability; safe proposed states only; obligation echoed never discharged) · privacy (all six token tables clean; planted host/user markers never reach plan bytes or tokens; no RESTART/REBOOT intent token) · properties (store×reset totality/determinism/no-target-mining; state×reset obligation preservation + source-identity immutability + no counter wrap; the TARGET_ACTIVE timing permutation sweep proving exactly-one-safe-class and that **no B4 plan ever emits `ALLOW_TARGET_MINING`**; fingerprint determinism/discrimination).

---

## 4. Engineering notes (recorded for future gates)

1. **The Gate B5+ boot integrator contract:** read `esp_reset_reason()` exactly once → `pool_session_reset_classify_raw()`; produce the boot `PoolTimeSnapshot` under `pool_session_recovery_build_time_policy(record, …)` so the snapshot and the engine enforce the SAME floor; persist the counter/record proposal via the B3 store **before** executing any plan action; on the eligibility plan, re-verify the live target identity + mining evidence before granting `ALLOW_TARGET_MINING`.
2. Plan memcmp determinism relies on `plan_defaults_conservative()` memsetting the struct first (padding deterministic) — keep that invariant when extending the plan.
3. `VERIFY_RESTORE` and `ALLOW_TARGET_MINING` are deliberately unproduced in B4 (reserved values, documented).
4. Repo reset facts: `esp_reset_reason()` appears only in `system_api_json.c` (cosmetic GET string); `esp_restart()` callers are HTTP restart/OTA, BAP, self-test and one stratum path — all untouched.

---

## 5. Validation summary (Phase B, against commit `4584e4e`)

- Branch/ancestry verified read-only; exactly the 9 approved files in the commit; tree clean before and after; **no Git write operations**.
- Frontend gate, fresh test-ci build, full QEMU **332/0/0**, main flag-OFF and flag-ON builds: all green (§1). The mission-named properties (eligibility-not-authorization, wait-inhibits-mining, missing/expired-deadline restores, floor-fed `required_min_epoch_s`, non-OK-never-resumes, obligation preservation, exactly-one-safe-class) were each re-confirmed by their specific committed tests in this exact run.
- **No hardware, physical-device NVS, COM/USB, real NTP, live DNS, real BTC/BCH pools, accounts, wallets, credentials, owner LAN, or private TCH recovery data were accessed.** No outbound networking occurred in any test or build; no release artifacts were created.

---

## 6. Committed files (9)

| File | Change |
|---|---|
| `components/pool_session_recovery/CMakeLists.txt` | new — `REQUIRES pool_session pool_session_store pool_time`, `PRIV_REQUIRES esp_system` |
| `components/pool_session_recovery/include/pool_session_reset.h` | new — 10-class model, pinned raws, pure helpers |
| `components/pool_session_recovery/pool_session_reset.c` | new — pure classification |
| `components/pool_session_recovery/pool_session_reset_espidf.c` | new — compile-only pin validation; uncalled wrapper |
| `components/pool_session_recovery/include/pool_session_recovery.h` | new — context/plan models, codes, engine contract |
| `components/pool_session_recovery/pool_session_recovery.c` | new — the total decision engine |
| `components/pool_session_recovery/test/CMakeLists.txt` | new — test registration |
| `components/pool_session_recovery/test/test_pool_session_recovery.c` | new — 34 Unity tests |
| `test/CMakeLists.txt` | `TEST_COMPONENTS` += `pool_session_recovery` |

---

## 7. Gate B5 prerequisites

1. Branch from this committed B4 head (`4584e4e`).
2. **B5 = operation-ownership lease + coordinator integration** (audit §10/§17): the persisted single-owner lease semantics over the B3 store, `protocol_coordinator` hook points, and the manual-pool-PATCH/OTA 409 policy surfaces — consuming B4 plans without yet executing apply/restart. B5 owns the first (still bounded) runtime touch-points: reading `esp_reset_reason()` once, producing the boot snapshot under the B4-built policy, and persisting proposals before actions per §4.1.
3. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; the B1 restore-obligation invariants; B2 trust semantics; B3 crash-consistency; QEMU ≥ 332 green; frontend 1052.
