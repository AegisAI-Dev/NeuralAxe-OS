# NeuralAxe OS — Phase 2M.1B, Gate B5

## Single-Owner Operation Lease and Coordinator Conflict Policy Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-ownership` · **Commit:** `dcba70b` — `feat: add single-owner operation lease and conflict policy` (**v2.14.2-54-gdcba70b**, parent `f210990` = the committed Gate B4 report; descends from `4584e4e` / `7e85a41` / `463417d` / `e72db15`)
**Scope:** the pure single-owner operation-lease policy, its synchronized caller-owned coordinator wrapper, and the sanitized HTTP 409 conflict mapper — **no runtime wiring** (no singleton instance, no boot hook, no HTTP handler registration, no `protocol_coordinator` calls, no NVS, no `esp_reset_reason()`, no `esp_restart()`, no networking, no frontend change). No production runtime behavior changed; nothing instantiates the coordinator.

Architectural sources of truth: the committed Phase 2M.1A reports (audit §10/§17) and the B1–B4 foundation reports.

---

## 1. Verdict

**Gate B5 PASS (Phase A + two owner-directed ownership corrections + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 12 approved B5 files, 3553 insertions / 1 deletion (see §7) |
| Runtime wiring | **none** — `pool_operation` references exist only inside `components/pool_operation_coordinator/` plus the `test/CMakeLists.txt` registration; nothing creates a coordinator; linker-discardable like B1–B4 |
| Forbidden APIs | none — no `nvs_*`, `esp_restart`, `esp_reset_reason`, sockets, Wi-Fi, SNTP, or HTTP-server dependency anywhere in the component |
| B1/B2/B3/B4 sources | untouched; **57 + 82 + 47 + 29 + 34** all green in the same run |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**, unchanged |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **373 Tests, 0 Failures, 0 Ignored — exit 0** (83 base + 57 + 82 + 47 + 29 + 34 + **41 `[pool_op]`/`[pool_op_coord]`**) |
| Main app, flag OFF / ON | both exit 0 (temp sdkconfig copies; tracked config untouched) |
| Module warnings | all 5 new translation units **clean under `-Wall -Wextra -Werror`** |
| Secret / local-path scan | clean; synthetic `*.example` fixtures only |
| Tree after verification | clean; `test-ci/CMakeLists.txt` restored byte-exact (22 B); no `report.xml`; no release artifacts |

---

## 2. What Gate B5 delivers

A new self-contained component **`components/pool_operation_coordinator/`** (REQUIRES `pool_session pool_session_store pool_session_recovery pool_time`):

- `include/pool_operation_types.h` — the bounded ownership model: **11 request kinds** (read-only, timed-session start/internal, restore-now, session-acknowledge, manual pool PATCH, OTA, manual restart, destructive maintenance, operator recovery, protocol reconcile), **9 owners**, **10 phases**, a **6-bit resource-scope mask** (session store, pool configuration, Stratum control, device restart, OTA flash, destructive maintenance), **21 statuses**, the lease token `{valid, owner, lease_generation}` (generation 0 permanently invalid), the coordinator state, **4 persistence-proof kinds** and **3 manual outcomes**. The header states the trust boundary explicitly: tokens are *consistency guards inside one firmware image*, not authentication capabilities.
- `include/pool_operation_policy.h` + `pool_operation_policy.c` — the PURE policy: fail-closed bootstrap from the B3 store result + record + real B4 plan; the total request × owner × phase conflict matrix; check-and-acquire; Restore Now as a control transition of the *same* lease; the legal phase graph; the persistence-before-action barrier; and evidence-demanding releases.
- `include/pool_operation_coordinator.h` + `pool_operation_coordinator.c` — the caller-owned synchronized wrapper: a static portMUX critical section around every whole pure operation, consistent snapshots, **no singleton** (Gate B6 creates the single instance). The header documents the future `protocol_coordinator` integration contract; a compile adapter was deliberately not added because `protocol_coordinator.h` drags in `global_state.h` and the whole application surface.
- `include/pool_operation_http_policy.h` + `pool_operation_http_policy.c` — the pure HTTP 409 mapper: 14 stable conflict codes; the surface carries **only** the sanitized owner class and the `restore_required` / `terminal_ack_required` booleans — no generations, session ids, identities, or record bytes.

### 2.1 Fail-closed bootstrap

Unbootstrapped state denies every mutation. `EMPTY`/`CLEARED` → FREE. A retained **COMPLETE or safe pre-mutation CANCELLED** → FREE with `terminal_pending=true` (no execution lease). Snapshot-committed → BOOT_RECOVERY holding RESERVED_PENDING_PERSISTENCE; B4 WAIT → WAITING_FOR_TRUSTED_TIME; the B4 eligibility plan → VERIFYING_TARGET; restore plans → SOURCE_RESTORE holding RESTORING_SOURCE. Every non-OK store result, every inconsistent input pair, and `STORE_COMMIT_UNCERTAIN` reconstruct **RECOVERY_GUARD**, which admits only bounded operator recovery.

### 2.2 Owner correction 1 — RECOVERY_REQUIRED is never an acknowledgeable terminal

Per the committed B1 contract, only COMPLETE and safe pre-mutation CANCELLED are acknowledgeable. A persisted **`RECOVERY_REQUIRED` record therefore bootstraps to the RECOVERY_GUARD owner and phase even when `restore_required=false`** and even though the B4 plan class is RETAIN: `terminal_pending` stays false, session acknowledgement is denied `OP_ERR_RECOVERY_LOCKED` alongside every other normal mutation (new sessions, PATCH, OTA, restart, reconcile), the record and all recovery evidence are preserved (`durable_claim=true`, bound session id retained), and no clear or tombstone path exists — the `TERMINAL_COMMITTED` persistence proof accepts only {COMPLETE, CANCELLED}, so a RECOVERY_REQUIRED "proof" is rejected as invalid. `restore_required=false` means no known source-restoration obligation remains; it does not make the state acknowledgeable. A future recovery workflow must first durably transform the record into a legitimately acknowledgeable B1 state.

### 2.3 Owner correction 2 — protocol reconcile never bypasses ownership

`OP_REQUEST_PROTOCOL_RECONCILE` is classified as **Stratum-mutating internal owner work** (Category A). The allow path requires all of: a compatible phase, the lease's `OP_SCOPE_STRATUM_CONTROL`, and the **matching current token** via the same `check_token` used everywhere (generation staleness dominates owner mismatch). The denial map: FREE ⇒ `OP_ERR_NOT_OWNER` — there is **no free-state reconcile path in B5**; a later runtime gate must supply an explicit short-lived reconcile-lease acquisition before ownerless protocol mutation becomes available. WAITING_FOR_TRUSTED_TIME ⇒ busy (HOLD_STRATUM outranks reconcile). RESERVED_PENDING_PERSISTENCE ⇒ persistence-required. TERMINAL_ACK_PENDING ⇒ invalid transition. OTA and acknowledgement leases ⇒ denied (their scope sets exclude Stratum control), so **OTA ownership cannot be bypassed**. RECOVERY_GUARD ⇒ recovery-locked. VERIFYING_TARGET and RESTORING_SOURCE permit only the owner's own matching-token internal reconcile; a manual-PATCH lease can be driven only by its own token. Failed decisions are byte-identical no-ops; an allowed decision creates no second owner and mutates nothing. The kind was removed from the retained-safe-terminal allowance.

### 2.4 Exclusive lease, persistence barrier, evidence-demanding release

One owner at a time, ever: acquisition of any mutating kind while a lease exists is denied without partial tokens; a deterministic two-contender FreeRTOS test proves exactly one of two racing tasks acquires through the wrapper. New sessions acquire RESERVED_PENDING_PERSISTENCE and stay inert until the **matching** `SESSION_COMMITTED` proof arrives (generation-checked); `STORE_COMMIT_UNCERTAIN` converts to RECOVERY_GUARD, always. Session release is legal only from TERMINAL_ACK_PENDING with a durable terminal proof for {COMPLETE, CANCELLED}; acknowledgement clears `terminal_pending` only with a `CLEARED` proof; manual releases carry an outcome, and `MUTATION_UNCERTAIN` lands in the guard. Restore Now is idempotent, rotates the generation of the *existing* lease (a superseded token reports `OP_ERR_STALE_LEASE`), and never creates a second owner. Token-rotating APIs are alias-safe (in-place rotation supported). Destructive maintenance is denied in every state. No B5 surface can grant target mining — the coordinator holds no mining-policy field at all, proven by test.

---

## 3. Test suite (41 tests → QEMU 332 ⇒ 373)

**Policy, 31 `[pool_op]`:** model totality (fail-closed init; request properties exact; scopes match the audited call sites; clean machine tokens) · bootstrap (unbootstrapped denies all; EMPTY/CLEARED free; COMPLETE and CANCELLED retain with `terminal_pending=true`; recovery plans reconstruct durable owners through the real B4 engine; all 13 non-OK results guard; inconsistent inputs fail closed; **RECOVERY_REQUIRED is never an acknowledgeable terminal** — guard posture, everything locked, operator-only, obligated variant keeps SOURCE_RESTORE) · conflict matrix (session start only from clean FREE; PATCH/OTA/restart denied under session owners; retained safe terminal permits manual work but denies reconcile; **protocol reconcile requires the owning token** — FREE denied, RESTORING_SOURCE and VERIFYING_TARGET matching-token arms with byte-snapshot no-mutation/no-second-owner proof, stale token, wrong owner class, manual-owner exclusivity, OTA denied with its own matching token, WAITING blocked, guard blocked; guard admits only operator recovery; internal session work demands the current token; destructive/unknown kinds fail closed) · acquisition (reserve + persistence demand; uncertain commit ⇒ guard, never active; PATCH/OTA mutual exclusion; generation exhaustion fails closed) · Restore Now (same-lease control, idempotent, staleness) · the legal phase graph rotating tokens · releases (durable terminal proof required, **RECOVERY_REQUIRED proof rejected**; stale tokens and wrong owners never release) · acknowledgement (CLEARED-proof-only; unresolved sessions never acknowledgeable) · properties (denied and read-only requests never mutate across bootstrap classes; **one exclusive owner and obligation safety** across eight persisted session states × six mutating kinds including tokenless reconcile; pure calls deterministic and input-immutable).

**Coordinator + HTTP, 10 `[pool_op_coord]`:** deterministic fail-closed lifecycle · the full session lifecycle through the synchronized wrapper · failed acquisitions never mutate the snapshot · exactly one of two semaphore-barriered contender tasks acquires · uncertain outcomes surface as a guard · every ownership conflict maps to 409 with a stable code · allowed decisions are not conflicts · the surface leaks no generations, ids, or identities (planted-marker byte scan) · terminal-ack and code tokens are clean · no B5 surface grants target mining.

---

## 4. Engineering notes (recorded for future gates)

1. **The Gate B6 integrator contract:** B6 creates the single coordinator instance, boots it exactly once from `{store result, record, B4 plan, committed generation}`, persists every proposal through the B3 store **before** executing the associated action, and feeds persistence proofs back to unlock RESERVED → ACTIVE. The documented 8-function `protocol_coordinator` surface is the intended Stratum touch-point; the compile adapter is deferred to keep B5 free of `global_state.h`.
2. Token semantics: generation staleness dominates owner mismatch in `check_token` — a superseded token is always `OP_ERR_STALE_LEASE` regardless of its owner field. Keep that ordering; the Restore Now test pins it.
3. Free-state protocol reconcile is intentionally unavailable. When a runtime gate needs ownerless reconcile, it must add an explicit short-lived reconcile lease — not widen the existing allow path.
4. Repo HTTP facts: the firmware serves no 409 today (only 400/404/302); the BAP handlers are an independent config+restart writer; there is no factory-reset endpoint; existing locks (`stratum_mux`, `nvs_cache_mutex`, queues) are not ownership locks. The HTTP busy code for an acknowledgement-lease conflict maps to `OPERATION_BUSY_TIMED_SESSION` (documented in the mapper).

---

## 5. Validation summary (Phase B, against commit `dcba70b`)

- Branch/ancestry verified read-only; exactly the 12 approved files in the commit; tree clean before and after; **no Git write operations**.
- Frontend gate **1052/1052 exit 0**, fresh test-ci build exit 0, full QEMU **373/0/0** (31 policy + 10 coordinator PASS by name in `output.log`, all B1–B4 baselines intact), main flag-OFF and flag-ON builds exit 0, all 5 TUs strict-clean. The owner-corrected properties (RECOVERY_REQUIRED guard posture and unacknowledgeability; no unowned Stratum-mutating reconcile path) were each re-confirmed by their specific committed tests in this exact run.
- **No hardware, physical-device NVS, COM/USB, real NTP, live DNS, real BTC/BCH pools, accounts, wallets, credentials, owner LAN, or private TCH recovery data were accessed.** No outbound networking occurred in any test or build; no release artifacts were created.

---

## 6. Ownership model quick reference

| Phase | Owner class | Admits |
|---|---|---|
| FREE (clean) | none | reads, new session, PATCH/OTA/restart (mutually exclusive) |
| FREE + retained safe terminal | none (`terminal_pending`) | reads, acknowledgement, PATCH/OTA/restart — **not** a new session, **not** reconcile |
| RESERVED_PENDING_PERSISTENCE | TIMED_SESSION / BOOT_RECOVERY | only the matching persistence proof |
| ACTIVE / VERIFYING_TARGET / RESTORING_SOURCE | session-class owner | owner's own token: internal work, reconcile, Restore Now, legal transitions |
| WAITING_FOR_TRUSTED_TIME | BOOT_RECOVERY | reads only — HOLD_STRATUM blocks even the owner's reconcile |
| TERMINAL_ACK_PENDING | session-class owner | terminal-proof release only |
| RECOVERY_GUARD | RECOVERY_GUARD | bounded operator recovery only |

---

## 7. Committed files (12)

| File | Change |
|---|---|
| `components/pool_operation_coordinator/CMakeLists.txt` | new — `REQUIRES pool_session pool_session_store pool_session_recovery pool_time` |
| `components/pool_operation_coordinator/include/pool_operation_types.h` | new — requests/owners/phases/scopes/statuses/token/state/proofs/outcomes |
| `components/pool_operation_coordinator/include/pool_operation_policy.h` | new — pure policy contract |
| `components/pool_operation_coordinator/pool_operation_policy.c` | new — bootstrap, conflict matrix, acquire, transitions, releases |
| `components/pool_operation_coordinator/include/pool_operation_coordinator.h` | new — synchronized wrapper + Stage-17 integration contract |
| `components/pool_operation_coordinator/pool_operation_coordinator.c` | new — portMUX-synchronized wrapper |
| `components/pool_operation_coordinator/include/pool_operation_http_policy.h` | new — sanitized 409 surface |
| `components/pool_operation_coordinator/pool_operation_http_policy.c` | new — pure conflict mapper (14 codes) |
| `components/pool_operation_coordinator/test/CMakeLists.txt` | new — test registration |
| `components/pool_operation_coordinator/test/test_pool_operation_policy.c` | new — 31 Unity tests |
| `components/pool_operation_coordinator/test/test_pool_operation_coordinator.c` | new — 10 Unity tests |
| `test/CMakeLists.txt` | `TEST_COMPONENTS` += `pool_operation_coordinator` |

---

## 8. Gate B6 prerequisites

1. Branch from this committed B5 head (`dcba70b`).
2. **B6 = scheduler / session execution**: the single coordinator instance, the boot integration per B4 §4.1 and §4.1 here (read `esp_reset_reason()` once, build the boot time snapshot under the B4 policy floor, bootstrap the coordinator, persist before acting), and the first bounded execution paths (apply target, live verification before any mining grant, the monotonic session timer, restore-now execution) — all still behind `CONFIG_NX_TIMED_SESSIONS`.
3. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; the B1 restore-obligation invariants; B2 trust semantics; B3 crash-consistency; B4 eligibility-not-authorization; the B5 exclusive lease, persistence barrier, RECOVERY_REQUIRED guard posture, and owned-only reconcile; QEMU ≥ 373 green; frontend 1052.
