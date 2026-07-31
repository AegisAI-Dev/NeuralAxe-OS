# NeuralAxe OS — Phase 2M.1B, Gate B6

## Feature-Flagged Timed-Session Runtime Skeleton & Boot Integration Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-runtime`
**Commits:** `57fe248` — `feat: add feature-flagged timed-session boot integration and runtime skeleton` (**v2.14.2-56-g57fe248**, parent `a31975a` = the committed Gate B5 report) **plus the owner-directed corrective follow-up** `00f077e` — `fix: make B6 persistence proposal tracking generation-aware` (**v2.14.2-57-g00f077e**, parent `57fe248`). This report documents the COMBINED committed B6 implementation.
**Scope:** the first runtime wiring of the timed-session foundations, entirely behind `CONFIG_NX_TIMED_SESSIONS` (default **n**): boot integration, the protocol-start barrier, the pure runtime classifier, the single owner task and generation-aware persistence-proposal tracking. Gate B6 **executes nothing outward** — no pool-configuration change, no Stratum stop/start/reconnect, no target-mining authorization, no restore execution, no restart, no OTA, no record clear or tombstone, no HTTP surface, no frontend change.

Architectural sources of truth: the committed Phase 2M.1A reports (`…_AUDIT.md`, `…_STATE_MACHINE.md`, `…_SECURITY.md`) and the B1–B5 foundation reports.

---

## 1. Verdict

**Gate B6 PASS (Phase A + owner-directed corrective pass + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | `57fe248`: exactly the 14 approved B6 files (4,372 insertions / 9 deletions) · `00f077e`: exactly the 6 approved corrective files (1,381 insertions / 178 deletions), all inside `components/pool_session_runtime/` |
| Ancestry | `00f077e` → `57fe248` → `a31975a` (B5 report) → `dcba70b` (B5 implementation); verified read-only |
| B1–B5 sources | untouched by both commits; `main/`, `test/`, `test-ci/` untouched by the corrective commit |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**; new `CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` **default ""** (no baked-in time source) |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** (complete run) |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **450 Tests, 0 Failures, 0 Ignored — exit 0** (373 B1–B5 baseline + **77** B6 tests, incl. the corrective and forced-collision tests, all PASS by name) |
| Default firmware build (flag OFF) | exit 0 (temp sdkconfig copy; tracked `sdkconfig` untouched) |
| Feature-enabled firmware build (flag ON) | exit 0 |
| Default-off runtime reachability | **0** — `xtensa-esp32s3-elf-nm` finds zero `pool_session_runtime` / `pool_runtime_` / `nx_timed_sessions` symbols in the flag-off ELF |
| Feature-enabled linkage | 29 runtime symbols linked; the boot hook, the barrier, `pool_session_runtime_boot`, `pool_runtime_proposal_build`, `pool_runtime_proposal_equal` and `pool_runtime_tracker_record_commit` all present |
| Module warnings | all 5 translation units compile **clean under `-Wall -Wextra -Werror`** |
| Secret / local-path scan | clean (synthetic `*.example` fixtures only; no hostnames, IPs, secrets, wallets or absolute paths) |
| Tree after verification | clean; `test-ci/CMakeLists.txt` byte-exact (22 B); no `report.xml`; no release artifacts |
| Access | no hardware, physical-device NVS, real NTP, live DNS, pools, OTA, restart, owner LAN or private recovery data; all builds and QEMU ran offline |

---

## 2. What Gate B6 delivers

A new self-contained component **`components/pool_session_runtime/`** in three layers, plus the minimal boot integration:

1. **Pure runtime controller** (`pool_session_runtime_core.h/.c`) — the total, fail-closed classification of {B3 store outcome × B4 plan × B5 ownership × persistence evidence} onto ONE runtime state, ONE protocol permission and ONE sanitized string-free snapshot; the bounded event model; and the generation-aware persistence-proposal domain (§3). No ESP-IDF, no IO, no heap, no globals.
2. **ESP-IDF adapter** (`pool_session_runtime.h/.c`) — the caller-owned runtime instance, the synchronous boot sequence, the persistence commit/readback/proof engine, the trusted-time provider lifecycle and the single owner task. Every platform dependency (store backend, SNTP ops, reset-reason read, monotonic clock, NTP server, wait limit) is injected; tests run the full adapter on deterministic fakes and the real NVS backend on the isolated QEMU partition.
3. **Feature-gated boot glue** (`pool_session_runtime_boot.h/.c`) — three functions for `main()`; under the disabled flag they are no-ops, no instance storage exists, `nx_tps` is never opened and SNTP is never initialized.

**Boot integration (main.c, both call sites under `#ifdef CONFIG_NX_TIMED_SESSIONS`):**

- **The boot hook sits immediately after `nvs_config_init()`** — the earliest point where NVS exists and nothing has started Stratum — and runs the whole Gate B6 order synchronously.
- **The protocol barrier is real:** `protocol_coordinator_task` is the SOLE creator of the stratum v1/v2 tasks and the only caller of the pool probes, so `nx_timed_sessions_protocol_start_allowed()` gating that ONE `xTaskCreate` withholds every pool connection. A hold is never released later in B6 — controlled release after live verification or restoration is Gate B7's job.
- The single bounded `nx_timed_sessions_notify_network_ready()` fires after Wi-Fi association; it performs no networking itself.

**Boot order (steps, exactly once per boot):** read `esp_reset_reason()` **exactly once** (audited by a per-instance counter; the raw value is classified immediately and never stored, published or logged) → open the real B3 `nx_tps` store and load the committed state (a failed open/load is NEVER `STORE_EMPTY`) → build the B4 trust-policy floor from the record and take the initial UNTRUSTED B2 snapshot under that same floor → build the boot context → run the pure **B4 plan** → bootstrap **exactly one B5 coordinator** → normalize, commit, independently read back and prove the mandatory persistence proposal (§3) → classify → publish the sanitized snapshot → answer ALLOW/HOLD → create the **single statically-allocated runtime owner task** (module-wide single-task guard; no heap). Boot itself never re-plans; re-evaluation is event-driven in the owner task (TIME_SYNC_CHANGED, MONOTONIC_BOUNDARY, STORE_RELOAD_REQUIRED, bounded-wait expiry) — the B4 §4.1 order plan → persist → act is preserved.

**Instance model:** exactly ONE production runtime instance (existing only under the flag; `pool_session_runtime_default_instance()` returns NULL when disabled), exactly one runtime task, exactly one B6 reset-reason read. A debug coordinator-depth counter proves no NVS or SNTP operation ever runs inside a B5 coordinator call.

### 2.1 Protocol permission (total, one-way in B6)

| Runtime state | Permission |
|---|---|
| `RUNTIME_FREE` (proven empty/cleared store, no obligation) · `RUNTIME_TERMINAL_PENDING` (retained safe COMPLETE / pre-mutation CANCELLED awaiting ack) | **ALLOW_SOURCE** — the existing, unchanged source/default protocol startup may proceed; never target mining, never a timed-session action |
| `RUNTIME_UNINITIALIZED` · `BOOTSTRAPPING` · `PERSISTENCE_PENDING` · `WAITING_FOR_TRUSTED_TIME` · `VERIFY_TARGET_PENDING` · `RESTORE_SOURCE_PENDING` · `OPERATOR_RECOVERY` · `RECOVERY_GUARD` · `STOPPED` · `ERROR` · any out-of-range value | **HOLD** (value 0, so any zeroed/partial decision fails closed) |

Target eligibility remains **verification-only** (`VERIFY_TARGET_PENDING` grants no mining; B4 never emits `ALLOW_TARGET_MINING` and a plan claiming it is rejected as impossible); restore remains **pending-only** (`RESTORE_SOURCE_PENDING` executes nothing). Nothing in B6 turns a HOLD back into an ALLOW.

---

## 3. Owner-directed corrective pass — generation-aware persistence-proposal tracking (`00f077e`)

The original B6 enforced a blanket rule: at most one B4 proposal commit per physical boot, with re-evaluation never writing. The motivation was real — the committed B4 counter model derives every proposal from the record's PERSISTED values (`reboot_count = increment(record.reboot_count)`; consecutive on the boot's frozen reset class), so a same-boot replan over a freshly committed record re-proposes the per-boot increments a second time. But the blanket rule was **too broad**: it also silently dropped every legitimate later proposal (the wait-expiry restore state + recovery-attempt increment; a raised trusted-epoch floor) while still reconciling the lease and advancing state — persist-before-action was violated for every post-boot proposal. The corrected contract:

- **RAM-only, bounded `PoolRuntimeProposalTracker`** (reset naturally by RAM loss on a real reboot; no wall clock, Stratum ntime, raw reset value or persistent boot identifier) plus a **normalized semantic `PoolRuntimeProposal`** carrying every field a commit may change: kind, source record generation, state-update flag + proposed state + failure code, all three counters, reset class, epoch-raise flag + proposed floor, expected `restore_required`. No identity, secret, raw record or unrestricted string exists in either.
- **The same semantic proposal derived from the same committed source generation commits at most once.** A **different later proposal** — new state, failure code, recovery-attempt increment, raised trusted-epoch floor — still commits, is independently read back and is proven during the same physical boot, **before** any related lease reconciliation or runtime-state advancement. A proposal derived from a **newer committed B3 generation** is never suppressed by an earlier one.
- **Per-physical-boot facts:** the reboot increment and the reset-class-driven consecutive increment are durably accounted **at most once per boot** (recorded at commit time, so the accounting survives a later proof-step failure) and are NORMALIZED out of later raw plans **without discarding the other field changes those plans carry**. `recovery_attempt_count` is per-planned-restore-action and is **never** normalized. The trusted-epoch floor ratchets only strictly upward, through the B3 acceptance helper, inside the sanity band.
- **Ordering per evaluation:** evaluate the B4 plan → build the normalized proposal → dedupe against the tracker → stage from the current committed record → commit through `pool_session_store` → **independent reload** → exact readback verification (SESSION kind, exact identity, obligation preserved, floor never lowered / raised floor lands exactly, strictly newer generation, exact state/failure/counters/reset class) → the matching **B5 persistence proof** whenever an owned session-class lease exists (B5 proofs are token-gated by the committed B5 design, so unowned postures — FREE with a retained safe terminal — have no lease to update; commit + readback is the complete proof there and no B5 call is made) → only then mark the tracker complete and advance. A failed commit stays pending (retried only on explicit events — never a tick-driven write loop); a readback mismatch stays pending in a conservative posture; **`STORE_COMMIT_UNCERTAIN` enters the B5 recovery guard, retains ownership evidence, retains protocol HOLD and never advances toward execution.**

### 3.1 Final owner correction — hash equality is NOT semantic equality

Proposal deduplication is a safety-ordering mechanism, and a 32-bit fingerprint may collide, so **a matching FNV-1a fingerprint alone NEVER establishes semantic proposal equality.** The tracker stores the **exact normalized proposal key** (`last_proposal` — fixed-width bounded fields only), and `pool_runtime_proposal_equal()` — explicit, unconditional **field-by-field equality** over every key field, never a memcmp over the padded struct — is the ONLY deduplication authority. `already_proven` requires: tracker proven (commit + independent readback verified + applicable B5 proof accepted) AND exact field equality including kind and source generation. The FNV fingerprint is retained solely as a diagnostic token, an inexpensive preliminary **inequality** check (sound because the hash is a pure function of the key fields: a differing hash proves the fields differ; a matching hash proves nothing), and test instrumentation. A forced collision — the stored diagnostic hash poked to equal a candidate's hash while any exact field differs — is proven, per field (state, failure code, epoch floor, recovery-attempt counter, `restore_required` expectation, source generation), to be treated as a **distinct** proposal that earns its own commit, independent readback and B5 proof; no collision case authorizes target mining or permits pool/Stratum mutation.

---

## 4. Test suite (77 B6 tests → QEMU 373 ⇒ 450)

**Pure core (46 `[pool_runtime]`):** feature boot-action for both flag values (disabled ⇒ unchanged startup; the default build reports no instance) · the total ALLOW/HOLD rule · store open/load failure guards (never `STORE_EMPTY`) · every boot posture through the REAL B4 engine + REAL B5 bootstrap (FREE, retained terminals, obligated terminals never allow, TARGET_ACTIVE resume verification-only, bounded-wait, expired-deadline restore-pending, RESTORE_DUE/RESTORE_FAILED hold, persisted RECOVERY_REQUIRED guards, operator-recovery dominates) · every non-OK store result holds · uncertain load and uncertain proposal guard · persistence-pending postures (unpersisted / failed / readback-failed) · generation-aware tracking (identical-proposal dedupe with completion only via `record_proven`; normalization removes ONLY accounted per-boot facts while preserving state updates and attempt increments; same-content proposals from another generation are new; **forced-collision never suppresses across five field mutations**; epoch-floor strict progress inside the band; runtime-known `persist_required` arms the barrier; proposal build determinism + exact-equality determinism) · proposal readback proof accepts only an exact landing (incl. exact raised-floor landing) · fail-closed inputs (NULL/malformed, failed B5 bootstrap, impossible mining-grant plan, phase/plan disagreement, every B5 phase maps or fails closed) · sanitized snapshot (string-free, no identity/session id, validation rejects inconsistent views) · bounded events (unknown dropped, duplicates idempotent, provider start only while waiting, shutdown releases nothing, bounded saturating wait, reload never mutates) · stable dot-free machine tokens.

**Adapter (29 `[pool_runtime_rt]`, fake platform + real NVS):** deterministic fail-closed lifecycle · reset reason read exactly once and only the CLASS published · boot postures over the real store (empty FREE+allow, failed-open guard, retained COMPLETE allow, TARGET_ACTIVE hold, corrupt pointer guard) · the mandatory boot proposal committed/read-back/proven exactly once · failed and uncertain boot commits hold/guard · the identical proposal never written twice · **five duplicate evaluations never recommit** · **a distinct trusted-time proposal (raised epoch floor) commits exactly once after boot, with duplicate sync events deduped and exactly one matching B5 proof per distinct proposal** · **the wait-expiry restore proposal is committed, not suppressed (state + attempt preserved, reboot normalized once-per-boot)** · **a new physical boot permits exactly one new reboot increment** · **a failed later commit blocks advancement until an event-driven retry proves it** · **an independent readback failure blocks advancement** · **an uncertain later commit enters the recovery guard with ownership evidence retained** · **a forced fingerprint collision still commits the distinct proposal end-to-end** · an unowned retained terminal commits its boot accounting with no B5 proof (`proof_count` 0) and keeps TERMINAL_PENDING/ALLOW · byte-identical snapshots and tracker state for identical inputs · exactly one runtime task module-wide with a measured stack high-water mark · duplicate/unknown/latched events safe · no configured time source ⇒ nothing starts, hold stays · a configured source starts only after network-ready while waiting · the bounded wait expires into restore-pending and still holds · the published snapshot carries no identity or session id (byte-scan) · real-NVS boot on the isolated QEMU partition (empty ⇒ FREE; committed session reloads and holds).

---

## 5. Security & privacy posture (honest)

- **No mutation surfaces exist in B6:** no pool-configuration write, no Stratum call, no restart, no OTA, no record clear, no tombstone, and no path that sets `target_mining_authorized` or `pool_mutation_permitted` (structurally pinned false and asserted on every produced decision and snapshot).
- The published snapshot has **no string field by construction** — no hostname, account, worker, wallet, password, session identifier, raw record, raw NVS byte or raw reset value can be carried. The proposal tracker is equally identity-free.
- The trust boundaries of B1–B5 are unchanged and not re-claimed: CRC/generation remain crash-consistency (not authenticity; the T13b offline rollback/forgery residual stands), lease tokens remain consistency guards (not capabilities), trusted time remains an operational verdict (SNTP/DNS unauthenticated; residual documented in B2).
- **Unresolved product decision (OPEN):** no NTP hostname is baked into the firmware. `CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` defaults to the empty string; until the product explicitly selects a trusted-time source (privacy, availability and jurisdiction trade-offs), the runtime reports an unconfigured time source and keeps protocol startup HELD whenever recovery requires trusted time — it never invents trust and never contacts a server the product has not chosen.

---

## 6. Engineering notes & gotchas (recorded for future gates)

1. **B4 is not idempotent across a persist** — counters are proposals derived from persisted values. The corrective answer is NOT "never write again": it is exact generation-aware deduplication plus once-per-physical-boot normalization of the reboot/consecutive facts. Keep `recovery_attempt_count` un-normalized (per-planned-action budget; bounded fail-safety depends on it).
2. **Hash equality is never semantic equality for safety decisions.** The exact normalized key + field-by-field equality is the authority; keep the fingerprint demoted to diagnostics/preliminary-inequality. The collision test seam is direct manipulation of the stored diagnostic hash — never brute-force a real FNV collision.
3. **B3 `commit_record` performs TWO pointer reads** (committed-base validation + post-write verify); the independent reload's pointer read is therefore the THIRD after arming a fault counter. A mid-test Unity abort leaks the runtime task and cascades `RUNTIME_ERR_TASK_ALREADY_RUNNING` failures through every later task test — read cascade failure lists root-first.
4. **`test-ci` CMakeCache records `IDF_TARGET=esp32`** (the project sets esp32s3 after `include(project.cmake)`), so `idf.py` refuses incremental re-runs with a target-mismatch error: always delete `test-ci/build` + `test-ci/sdkconfig` and build fresh.
5. **Main-app builds in the bare `espressif/idf` container need `GITHUB_ACTIONS=true`** (CI parity) plus the existing `axe-os/dist` — otherwise the `openapi_generate` step fails on a missing npm. Use temp sdkconfig copies (`idf.py -B <dir> -DSDKCONFIG=<copy> build`) so the tracked `sdkconfig` is never touched.
6. The QEMU action image bundles IDF v5.5.4 — build with v5.5.3 and borrow only `/opt/qemu`; override its entrypoint (`--entrypoint /bin/bash`) or the wrapper consumes the command as its code path.

---

## 7. Validation summary (Phase B, against commit `00f077e`)

- Branch `neuralaxe-v0.1-timed-pool-sessions-runtime`, HEAD `00f077e` (**v2.14.2-57-g00f077e**), parent `57fe248` — ancestry verified read-only; the corrective commit contains exactly the 6 approved files with zero changes outside `components/pool_session_runtime/`; `git diff 57fe248..00f077e` touches no `main/`, `test/`, `test-ci/` or B1–B5 path; working tree clean before and after; **no Git write operations**.
- Frontend `npm run test:gate`: **1052 / 1052, exit 0**.
- Fresh ESP-IDF v5.5.3 `test-ci` build: exit 0; full QEMU: **450 / 0 / 0** — the 434-test committed-B6 baseline intact, all corrective and both forced-collision tests PASS by name in `output.log`.
- Default firmware build (flag OFF): **PASS (exit 0)**; feature-enabled build (flag ON): **PASS (exit 0)**; both from temp sdkconfig copies, tracked `sdkconfig` untouched.
- Strict compile: all 5 TUs clean under `-Wall -Wextra -Werror` (per-TU via `compile_commands.json`).
- Default-off runtime reachability: **0 symbols** in the flag-off ELF; feature-enabled linkage verified: 29 runtime symbols including the boot hook, the barrier and the corrective tracker/equality functions.
- Secret / local-path scan on the committed files: clean (synthetic `*.example` fixtures only); line endings LF; `test-ci/CMakeLists.txt` restored byte-exact (22 B); `report.xml` removed; no release artifacts.
- **No hardware, physical-device NVS, COM/USB, real NTP, live DNS, real BTC/BCH pools, accounts, wallets, credentials, owner LAN, or private TCH recovery data were accessed.** All builds and the QEMU suite ran offline in local containers.

---

## 8. Committed files

**`57fe248` (14):** `components/pool_session_runtime/` — `CMakeLists.txt`, `include/pool_session_runtime.h`, `include/pool_session_runtime_boot.h`, `include/pool_session_runtime_core.h`, `pool_session_runtime.c`, `pool_session_runtime_boot.c`, `pool_session_runtime_core.c`, `test/CMakeLists.txt`, `test/test_pool_session_runtime.c`, `test/test_pool_session_runtime_core.c` (all new) · `main/CMakeLists.txt` (+`pool_session_runtime` dependency) · `main/Kconfig.projbuild` (B6 flag help + `NX_TIMED_SESSIONS_NTP_SERVER` default "") · `main/main.c` (boot hook, network-ready notify, protocol barrier — all under the flag) · `test/CMakeLists.txt` (`TEST_COMPONENTS` += `pool_session_runtime`).

**`00f077e` (6):** `include/pool_session_runtime_core.h`, `include/pool_session_runtime.h`, `pool_session_runtime_core.c`, `pool_session_runtime.c`, `test/test_pool_session_runtime.c`, `test/test_pool_session_runtime_core.c` — the generation-aware tracker, exact field-by-field equality, normalization, the corrected persist-before-action ordering and the corrective/collision tests.

---

## 9. Gate B7 prerequisites

1. Branch from this committed B6 head (`00f077e`).
2. **B7 = controlled apply, restore execution and live verification:** release the protocol hold ONLY after live target-identity + mining-evidence verification (the first legitimate `ALLOW_TARGET_MINING` grant lives here, never in B4/B6); execute pending restores through the B5 `SOURCE_RESTORE` lease with bounded retries; wire the real SNTP sync callback to `TIME_SYNC_CHANGED` and a genuine monotonic-boundary source; establish the session heartbeat cadence (state transitions + the bounded 2M.1A heartbeat — never per telemetry tick; the 24 KB `nvs` partition is shared); surface the HTTP 409 conflict mapping. Every mutation flows through the B5 lease and the generation-aware persist-before-action machinery of §3.
3. Resolve (or explicitly defer again) the **open NTP-source product decision** before any hardware trusted-time pilot.
4. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; the B1 restore-obligation invariants; B2 trust semantics; B3 crash-consistency; B4 eligibility-not-authorization; the B5 exclusive lease, persistence barrier, RECOVERY_REQUIRED guard posture and owned-only reconcile; the B6 one-way barrier, exact-equality proposal deduplication and once-per-boot accounting; QEMU ≥ 450 green; frontend 1052.

**Hardware pilot verdict: NOT READY** — no timed session may run on a physical device before Gate B7 delivers live verification and restore execution, the NTP-source decision is made, and the real-device validation plan (2M.1A §18) is executed.
