# NeuralAxe OS — Phase 2M.1B, Gate B8

## Timed-Session HTTP Control, Status, Acknowledgement and Conflict API Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-api`
**Commit:** `2021622` — `feat: add feature-flagged timed-session HTTP control API with audited lease release and same-boot execution` (**v2.14.2-61-g2021622**, parent `4926845` = the committed Gate B7 report; descends from `93f441c`). 38 files, 8,840 insertions / 26 deletions.
**Scope:** the first production control plane for the committed timed-session runtime — sanitized status, bounded session creation, Restore Now, safe terminal acknowledgement, owner-task command submission, standardized HTTP 409 conflict responses, and a bounded TARGET_ACTIVE liveness heartbeat. Everything sits behind the new `CONFIG_NX_TIMED_SESSIONS_API` (depends on `CONFIG_NX_TIMED_SESSIONS` **and** `CONFIG_NX_TIMED_SESSIONS_EXECUTION`, **default n**, never auto-enabled). Keep-current-password only: no B8 path accepts, stores, transports or logs a password.

Architectural sources of truth: the committed Phase 2M.1A reports and the B1–B7 foundation reports. Gate B8 was accepted after one owner review round; all three blockers that round raised are part of the committed implementation and are documented below.

---

## 1. Verdict

**Gate B8 PASS (Phase A + one owner review round + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 38 approved files: **20 modified + 18 added**; every path inside the approved boundary |
| Ancestry | `2021622` → `4926845` (B7 report) → `93f441c` (B7 implementation); verified read-only |
| B1 / B2 / B3 / B4 sources | **untouched** — the commit contains no `components/pool_session/`, `pool_session_store/`, `pool_session_recovery/` or `pool_time/` path |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS_API` **default n**, `depends on NX_TIMED_SESSIONS && NX_TIMED_SESSIONS_EXECUTION` |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **693 Tests, 0 Failures, 0 Ignored** (603 B1–B7 baseline + **90** B8) |
| Posture A (all three flags off) | build exit 0; **0** runtime / execution / api / fence / route symbols — stock behaviour |
| Posture B (`NX_TIMED_SESSIONS` only) | build exit 0; 32 runtime, 0 execution, 0 routes |
| Posture C (+ `_EXECUTION`) | build exit 0; 33 runtime, 53 execution, **0 routes, 0 command queue, 0 adopt/abort symbols** |
| Posture D (+ `_API`) | build exit 0; 36 runtime, 57 execution, 34 api, **4 route handlers**, 6 adopt/abort |
| Strict compile | **14 / 14** translation units clean under `-Wall -Wextra -Werror` |
| Locked-span scan | 8 spans, final depth 0, **0** store / coordinator / adapter / IO / logging / allocation calls inside any span |
| Static RAM | **+5,736 B** `.bss` (`s_default_processor`) + 8 B `.data`, API flag only |
| Binary size | **+23,600 B** (posture D − posture C) |
| Secret / privacy / local-path scan | committed diff clean; synthetic `*.example` fixtures only |
| Tree after verification | clean; `test-ci/CMakeLists.txt` byte-exact stub (22 B); no `report.xml`; no release artifacts created |
| Access | no hardware, COM/USB, flashing, physical-device NVS, real NTP, live DNS, real pools/accounts/wallets/credentials, OTA or device restart; all builds, QEMU and audits ran offline |

---

## 2. What Gate B8 delivers

A new self-contained component **`components/pool_session_api/`** in four layers, plus the HTTP adapter and three narrowly scoped integrations:

1. **Pure request parser** (`pool_session_api_parser.{h,c}`) — strict, fail-closed JSON validation with 24 stable machine codes. No ESP-IDF, no HTTP-server dependency, no NVS, no runtime mutation.
2. **Pure sanitized status builder** (`pool_session_api_status.{h,c}`) — an input model of fixed-width scalars only, so an identity cannot even be supplied to it.
3. **Pure heartbeat scheduler and fail-safe policy** (`pool_session_api_heartbeat.{h,c}`) — bounded cadence, bounded failure budget, bounded durable age; the monotonic clock is an input.
4. **Bounded command transport + owner-task processor** (`pool_session_command.{h,c}`) — a statically allocated depth-4 mailbox with many HTTP producers and exactly ONE consumer (the committed B6 owner task), and the create / Restore Now / acknowledge flows.

**HTTP adapter** (`main/http_server/pool_session_api.{c,h}`): origin check, bounded body read, strict parse, command submission, sanitized responses, and THE standardized 409 conflict body shared with the existing fences.

**Integrations:** a bounded API command hook and the epoch-heartbeat entry point in `pool_session_runtime`; the sanitized-conflict variant of the admission fence; the same-boot adoption and ordered-restore entry points in `pool_session_execution`; two audited abort contracts in `pool_operation_coordinator`; route registration and the URI budget in `http_server.c`; the production binding in `nx_execution_glue.c`; OpenAPI.

### 2.1 Routes and the URI budget

| Route | Method | Success | Failures |
|---|---|---|---|
| `/api/system/timed-session` | GET | 200 sanitized status | 401 |
| `/api/system/timed-session` | POST | 202 accepted | 400 / 401 / 409 / 503 |
| `/api/system/timed-session/restore` | POST | 202 | 400 / 401 / 409 / 503 |
| `/api/system/timed-session/acknowledge` | POST | 202 | 400 / 401 / 409 / 503 |

**No route accepts a session id** — only one session can exist. 22 of the previous `max_uri_handlers = 25` were already used, so the value becomes `25 + NX_POOL_SESSION_API_URI_HANDLERS` (0 or 4): `esp_http_server` allocates `max_uri_handlers` POINTERS, so the entire cost is **16 bytes of heap**, and only under the API flag. Registration happens BEFORE the broad `/api` and root wildcards, because the server matches in registration order. Every route calls the repository's existing `is_network_allowed()` before reading or applying anything.

### 2.2 Strict request validation

Rejected before a command can be built, each with its own stable code: oversized bodies, embedded NULs, malformed JSON, non-object roots, unknown fields, duplicate keys, missing/null/wrong-typed fields, over-budget objects, numeric strings for numeric fields, fractional numbers, empty/oversized host, invalid port, empty/oversized account, unsupported protocol, unsupported TLS mode, **custom-certificate TLS** (its own code), unsupported chain, and an invalid diagnostics id. A case-insensitive deny list rejects every password-like (`password`/`passwd`/`pass`/`pwd`/`secret`/`token`/`key`/`credential`/`auth`), source-like, session-id-like and internal-state-like key BEFORE the unknown-field rule, so the client receives the specific reason. **The rejected value is never echoed back**, and the output struct is fully zeroed on every failure path (asserted byte-wise).

Nothing is silently normalized: a host is never trimmed, because trimming would change the identity that must later be compared exactly.

---

## 3. Owner correction 1 — a definite no-mutation failure never orphans a lease

The committed B5 policy had no path that dissolves a `RESERVED_PENDING_PERSISTENCE` session lease (`release_manual` explicitly refuses session owners; `release_session` requires `TERMINAL_ACK_PENDING`). That is right for any lease that may have mutated something, but it left a create whose first record commit definitely failed — and an acknowledgement whose tombstone definitely did not commit — holding a lease nothing could dissolve until a reboot.

Gate B8 adds **two narrow, token-verified, evidence-demanding B5 aborts** (an approved B5 correction; no enum, phase, owner or status value was added, so every committed `_Static_assert` still holds):

- **`pool_operation_abort_reservation`** — admissible ONLY for `OP_OWNER_TIMED_SESSION` in `OP_PHASE_RESERVED_PENDING_PERSISTENCE`, and only while B5's OWN evidence says nothing committed (`!durable_claim && bound_record_generation == 0 && !restore_required`; a committed session would already be `ACTIVE`). A reconstructed `OP_OWNER_BOOT_RECOVERY` reservation is a DURABLE claim and is never abortable.
- **`pool_operation_abort_acknowledge`** — admissible ONLY for `OP_OWNER_SESSION_ACKNOWLEDGE`, and only when an independent reload proves the ORIGINAL terminal record is still the committed state.

`PoolOperationNoMutationEvidence` carries five required proofs — the independent reload's `store_result`, `store_unchanged`, `pool_config_untouched`, `protocol_untouched`, `restore_required_never_set` — all bounded booleans and one enum; no identity, secret, record byte or NVS key. Any missing claim returns `OP_ERR_UNSAFE_RELEASE` and changes **nothing**.

**On success:** the generation rotates (every prior token becomes stale), ownership becomes FREE, the session binding clears, and **no terminal result is invented** — the reservation abort leaves `terminal_pending` exactly as it was, while the acknowledgement abort deliberately keeps it **true** so the retained result survives for a later retry. **No client retry is involved in cleaning ownership.**

**Uncertainty never releases.** `STORE_COMMIT_UNCERTAIN` enters the B5 recovery guard and returns `OP_ERR_PERSISTENCE_UNCERTAIN`. An ambiguous readback, and any state the caller cannot prove unchanged, likewise refuse: the lease is retained (explicit, owned, fenced, published in status, RAM-only) rather than released on a guess.

**The honest asymmetry, pinned by test:** a failed slot *write* stages nothing, so the reload reads exactly the pre-operation state and the abort releases. A failed slot *commit* leaves a staged slot with no committed pointer, so the store is NOT provably unchanged and the abort correctly refuses.

---

## 4. Owner correction 2 — a successful create is executed in the same boot

The committed executor entry (`exec_handle_entry`) is BOOT-shaped: it always applies `DEVICE_RESTART_OBSERVED` first and enters only from the B4 resume or restore postures. The committed B4 table reads a persisted `TARGET_SNAPSHOT_COMMITTED` record as "interrupted before mutation" and proposes CANCELLED, and the B6 classifier fails closed on `OP_PHASE_ACTIVE` because it never reconstructs that phase at boot. None of those surfaces can serve an **in-boot** creation.

Gate B8 therefore adds **`pool_session_executor_adopt_created_session()`**, called by the owner task inside the same create command:

1. bound, system-ready and **IDLE** (a second adoption is refused — there is never a second owner);
2. board 601 / BM1370 through the audited device-identity adapter;
3. the durable record is a SESSION in `TARGET_SNAPSHOT_COMMITTED` with `restore_required == false` and Keep-current-password;
4. the CURRENT B5 token owns a session-class lease whose phase is compatible with target-side work;
5. the live effective TLS mode is representable (a custom certificate is refused before any mutation intent);
6. `record_to_session`, then the lease moves onto `OP_PHASE_VERIFYING_TARGET`;
7. the committed B1 `TARGET_APPLY_REQUESTED` boundary runs through the committed `exec_apply_event`: **commit → independent reload → exact readback verification → B5 proof + token rotation**. This is what makes `restore_required = true` DURABLE;
8. only then is `EXEC_STATE_TARGET_APPLYING` armed. The first `stage_apply` happens on the NEXT executor step.

**Target mutation is therefore impossible before the obligation is durable** — the QEMU suite asserts `stage_calls == 0` and `start_calls == 0` at the instant `APPLYING_TARGET` + `restore_required` are committed, and `stage_calls == 1` only after one further step.

**Adoption failure before mutation** takes the committed pre-mutation B1 CANCELLED path: no pool touched, no restoration owed, a safe terminal result retained for acknowledgement, and the result reported as `ADOPTION_FAILED` — **never as an accepted create**. A completed command therefore never leaves a durable `TARGET_SNAPSHOT_COMMITTED` record behind. A create submitted with **no executor bound** is refused at submission (`API_SUBMIT_NOT_READY` → 503), so the route never accepts a session with no viable adoption path.

**`api_owns_flow` lifetime.** It is a BOUNDED owner-task critical-workflow flag: set at the top of command processing, cleared on every return path, never held waiting for another HTTP request or for future code. `pool_api_processor_step()` returns `api_owns_flow || pool_session_executor_owns_flow(ex)`, and the B6 re-evaluation gate became `if (!runtime_executor_owns_flow() && !runtime_api_owns_flow())`. The union closes the handoff window with no gap: there is never an interval with two mutating owners (they are strictly sequential on one task) and never an interval with no owner while a durable session is active. The B4 plan is still rebuilt on every re-evaluation, because the executor consumes it as INPUT.

**Crash between `SESSION_COMMITTED` and adoption** leaves a `TARGET_SNAPSHOT_COMMITTED` record, which the committed B4 boot table resolves conservatively to CANCELLED — the correct recovery, unchanged from B4.

---

## 5. Owner correction 3 — the heartbeat has a bounded fail-safe

**Schema finding: SUPPORTED, with no B3 change.** The heartbeat re-commits the already committed `latest_trusted_epoch_s` — the monotonically-advancing latest-accepted-trusted-epoch floor — through the committed B3 acceptance helper. That is honest, not a repurposing: the value written IS a trusted epoch the device accepted at that moment.

It is driven by a NEUTRAL plan (every counter equal to the currently committed value, no record proposal) fed to the committed `pool_runtime_proposal_build`, which therefore yields an **EPOCH-FLOOR-ONLY** proposal. Extending a deadline is **structurally impossible**: `deadline_epoch_s`, `deadline_valid`, `deadline_sync_generation` and `duration_s` are never in the proposal. A five-iteration QEMU test asserts all four byte-identical across repeated heartbeats.

Persistence runs through the committed generation-aware B6 engine unchanged: normalized proposal → exact-equality dedupe → commit → independent reload → exact readback verification → B5 proof.

**Cadence gates (both required):** durable `TARGET_ACTIVE`, a valid B7 target-mining grant, no recovery guard, a trusted in-band B2 epoch, `POOL_API_HEARTBEAT_PERIOD_S` = 60 s of monotonic time since the anchor, AND `POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S` = 60 s of trusted-epoch advance over the last durable value. The first evaluation of a grant only ARMS — it never writes. A duplicate tick, an equal epoch and an older epoch all write nothing.

**Bounded failure policy.** RAM-only, monotonic, identity-free: `consecutive_failures` (max **3** definite failures), `age_anchor_us` → durable age (max **180 s**), a separate retry anchor, and a `failsafe_engaged` latch so repeated task ticks can never duplicate an order.

**The ordered fail-safe**, performed by `pool_session_executor_request_restore()` in exactly this sequence — the first two statements of the committed `exec_begin_restore`:

1. **inhibit ASIC target work** (`gate_write(EXEC_GATE_INHIBITED)` — the gate closes BEFORE the revoke so no job can slip through);
2. **revoke the B7 target-mining grant**;
3. **restore the source through the EXISTING session owner** (never a second lease);
4. **persist the restoration intent before any source mutation**;
5. **retain `restore_required`**.

`STORE_COMMIT_UNCERTAIN` engages it immediately. A definite failure publishes `PERSIST_FAILED`, consumes one unit of the budget, never advances the durable epoch and never claims liveness; a success resets the budget, re-anchors both the cadence and the age, and clears the latch. After a reboot every RAM counter is gone and the **persisted B3 floor** is the only reference — asserted by test.

A heartbeat is an accepted-trusted-epoch DURABILITY signal and nothing more. It is not a substitute for ASIC processing evidence, protocol health, thermal health or mining-grant validation, and it never grants, extends or renews target mining.

---

## 6. Non-negotiable safety properties, as committed

- **HTTP handlers only validate and submit.** `main/http_server/pool_session_api.c` contains no store call, no coordinator call, no `esp_restart()`, no pool write, no protocol call and no ASIC-gate write — grep-checkable and asserted by test. The whole `pool_session_api` component contains none either.
- **The source is captured internally.** The create request model has no source field at all; the snapshot is read through the SAME audited flash-level adapter the Gate B7 executor uses (never a second, unaudited reader), first as a pre-flight rejection gate and then again UNDER the lease — where the B7 fence already denies every foreign pool PATCH — so the snapshot that becomes immutable is the race-free current truth.
- **Keep-current-password only.** No password field exists in any B8 model. The only password touch anywhere is a **length-only** probe (`nvs_get_str(..., NULL, &len)`) returning one boolean; no password byte is read, copied, compared, returned or logged.
- **Exact restorability is a precondition, not a hope.** A create is refused when the effective readback fails, when a custom-certificate TLS mode is stored, when the device is currently mining its FALLBACK endpoint (the committed B7 transaction pins `use_fallback` false, so that role could not be restored exactly), or when the stored password would not be retained.
- **Every fallible validation runs BEFORE `try_acquire`**, so a rejected create never holds a lease at all — proven by tests asserting `OP_OWNER_NONE` and zero store writes.
- **The session id is derived once and fixed** (`pending_session_id`) across the pre-flight build, the reservation, the under-lease build and the persistence proof, so they can never disagree.
- **Acknowledgement clears only a safe terminal.** Only COMPLETE or a safe pre-mutation CANCELLED with no restoration obligation; TARGET_FAILED, RESTORE_FAILED, RECOVERY_REQUIRED, every active state and every obligated record are denied. `STORE_CLEARED` from an independent reload is mandatory before the terminal state is considered cleared, and an uncertain clear never becomes FREE.
- **Every ownership conflict returns HTTP 409** with the committed B5 mapper's sanitized body: a stable machine code, `retryable`, the owner CLASS and two safe booleans. Never a lease token, lease generation, record generation, session id, hostname, port, account, worker, wallet, password or raw record byte.
- **Status is read-only and sanitized.** It calls no external operation — it copies the snapshot the owner task published — and the builder's INPUT model has no string or pointer, so an identity cannot reach it. Marker scans over both the pure model and a live post-create status find nothing, and the session id appears nowhere.
- **The existing fences are unchanged in placement.** Pool PATCH, restart, firmware OTA and web OTA still evaluate admission BEFORE the first side effect; only the RESPONSE REPRESENTATION is standardized, and only under the API flag, so postures B and C keep the committed Gate B7 bare-token body byte-for-byte. BAP retains its bounded non-HTTP error.
- **Bounded transport.** Static depth-4 mailbox, commands stored by value, no heap per request, no pointer retained, consumed slots fully zeroed (proven by a raw byte scan), queue-full fails without mutating anything, exactly one consumer.
- **No locked span does blocking work.** 8 spans, final depth 0, zero store / coordinator / adapter / IO / logging / allocation calls inside any of them.

---

## 7. Test suite (90 Gate B8 tests → QEMU 603 ⇒ 693)

**Parser, 16 `[pool_api_parser]`:** valid minimal create · exact duration minimum and maximum · optional diagnostics id · byte-identical output for identical input · malformed / non-object / empty / oversized / embedded-NUL bodies · unknown, duplicate and missing fields · nulls, wrong types, numeric strings and fractional numbers · duration below and above the B1 bounds · bounded host, port and account rules at and past the bound · unsupported protocol / TLS mode / custom certificate / chain · every password-like field with a name-collision check on the legitimate keys · client-supplied source, session id and internal-state fields · action bodies (empty, `{}`, bounded id, and every fail-closed case) · token decoders never normalize · stable dot-free codes.

**Status, 11 `[pool_api_status]`:** the FREE view · the fail-closed NULL view · disabled postures · waiting / verifying / mining / expired / restoring / COMPLETE-pending views · RESTORE_FAILED and recovery-guard operator demand · every invalid enum failing closed · a total deadline verdict · a planted-marker byte scan · byte-identical output · structural validation of impossible views · stable tokens.

**Heartbeat, 10 `[pool_api_hb]`:** not applicable without TARGET_ACTIVE or a grant · guard and untrusted time never write · out-of-band epochs · the first evaluation only arms · the exact cadence boundary and a regressed clock · the exact minimum epoch advance · duplicate ticks after a commit · the durable epoch ratchets only upward · a failure re-arms but never claims liveness · byte-identical pure decisions · stable tokens.

**Command and flow, 41 `[pool_api_cmd]`:** mailbox bounds, FIFO order and zeroed slots · invalid commands refused before queueing · no command can carry a password byte · create from FREE reaching same-boot adoption with the obligation durable and zero pool writes · immutable source snapshot · second create refused · unsupported hardware, unrestorable source and equal-target refusals with **no lease taken and zero store writes** · **audited releases**: definite write failure releases, unprovable state never releases on a guess, source failure under the lease releases, uncertain commit guards, ambiguous readback guards, definite acknowledgement failure releases with the terminal retained, uncertain acknowledgement guards, no failure path mutates pool/protocol/ASIC state · create with no executor refused at submission · `api_owns_flow` false on every completed path · Restore Now (no active session, same-lease control, idempotent, terminal refusal) · acknowledgement (COMPLETE, refused-adoption CANCELLED, every unsafe state, obligated record never reaching the tombstone path, uncertain clear) · heartbeat (floor-only, duplicate suppression, no committed session, deadline/duration byte-identical, reboot uses the persisted floor) · status privacy · unbound processor · exactly one of two racing creates · concurrent producers behind a semaphore barrier · deterministic session-id derivation · the API flag default n.

**B5 aborts, 7 `[pool_op]`:** a proven no-mutation reservation releases to FREE with a rotated generation, a stale old token and a normally-acquiring later create · CLEARED and OK also prove an unchanged state · each of the four missing claims and two unprovable store results refuse individually · uncertainty guards and locks the device · only an uncommitted TIMED_SESSION reservation is abortable (boot-recovery, manual and activated leases all refused) · an acknowledgement abort releases with `terminal_pending` retained and a new session still blocked · an acknowledgement abort demands the unchanged terminal (CLEARED, unchanged=false, uncertain and wrong owner all refused).

**B7 adoption and fail-safe, 5 `[pool_exec_rt]`:** a session created this boot is adopted, with the obligation durable before a single pool key is staged and the transaction armed only on the next step · adoption refused without side effects when not system-ready, on unsupported hardware, on a custom-certificate source and on a wrong durable state · adoption never creates a second owner · an ordered restore inhibits, revokes and restores in that order, keeps the obligation, leaves the deadline untouched and is idempotent · an ordered restore is refused from a non-owning posture.

---

## 8. Security and trust boundary (honest)

- The API is reachable only through the repository's existing **private-network origin admission check**; it is **NOT user-authenticated** and this report does not claim otherwise.
- **HTTP 409 is conflict reporting, not authentication or access control.**
- The command **actor class identifies a request SOURCE class, not a human**. No authenticated user audit logging exists or is claimed.
- Lease tokens remain internal consistency guards, not cryptographic capabilities; arbitrary firmware code could bypass the coordinator entirely.
- Passwords are never accepted, stored, transported, compared or exposed — only their key's readability is probed.
- Pool identities remain private from the status API by construction.
- Secure Boot, flash encryption and NVS encryption remain **disabled** on this build; the B3 plaintext-account-at-rest and offline-flash rollback/forgery residuals stand unchanged, as do the B2 SNTP/DNS trust residual and the unresolved product decision on a trusted-time source (`CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` still defaults to empty).

---

## 9. Validation summary (Phase B, against commit `2021622`)

- Branch, ancestry and the exact 38-file change set verified read-only; every path inside the approved boundary; B1–B4 sources untouched; working tree clean before and after; **no Git write operations** — the owner performed the commit.
- Frontend `npm run test:gate`: **1052 / 1052, exit 0**.
- Fresh ESP-IDF v5.5.3 `test-ci` build: exit 0. Full QEMU: **693 / 0 / 0** (78 `pool_session_api` + 7 `op-abort` + 5 `b8-adopt`/`b8-failsafe` on top of the 603-test committed baseline).
- Build postures A / B / C / D from pristine sdkconfig copies: exit 0 each; symbol counts as tabled in §1; tracked `sdkconfig` untouched.
- Strict compile of the 14 B8-touched translation units: **0 failures**.
- Locked-span scan on the committed source: 8 spans, depth 0, no blocking calls.
- Secret / identity / local-path scan of the committed diff: clean; synthetic `*.example` fixtures only.
- `test-ci/CMakeLists.txt` restored to the byte-exact 22-byte stub; `report.xml` removed; no release artifacts were created.
- **No hardware, COM/USB, flashing, physical-device NVS, real NTP, live DNS, real BTC/BCH pools, accounts, wallets, credentials, OTA, device restart, owner LAN or private TCH recovery data were accessed.** No test performs outbound networking.

---

## 10. Known limits — hardware-pilot NOT READY

- **The API is default-disabled and requires both prior flags.** Posture A is byte-for-byte stock behaviour with zero B8 symbols.
- The **unauthenticated LAN posture** is the dominant residual: anyone who can reach the device's private-network HTTP surface can create, restore and acknowledge a timed session. That is unchanged from the rest of the API and is not solved by Gate B8.
- **No frontend exists.** Gate B8 adds no UI, and no automatic session creation of any kind.
- The Gate B7 limits still stand in full: no deterministic ASIC generation barrier is claimed, the binding guarantee is probabilistic at fixed difficulty 256, the discriminator budget is 127 controlled starts per boot, and SV2 standard channels cannot host a discriminator.
- The **trusted-time source decision remains open**; while it is unconfigured the runtime holds rather than inventing trust.
- Real-hardware pilots require a separate, explicitly scoped gate with owner-supervised flashing — **not authorized by this report**.

---

## 11. Committed files (38)

**Added (18)** — `components/pool_session_api/`: `CMakeLists.txt`, `include/pool_session_api_types.h`, `include/pool_session_api_parser.h`, `include/pool_session_api_status.h`, `include/pool_session_api_heartbeat.h`, `include/pool_session_command.h`, `pool_session_api_types.c`, `pool_session_api_parser.c`, `pool_session_api_status.c`, `pool_session_api_heartbeat.c`, `pool_session_command.c`, `test/CMakeLists.txt`, `test/test_pool_session_api_parser.c`, `test/test_pool_session_api_status.c`, `test/test_pool_session_api_heartbeat.c`, `test/test_pool_session_command.c` · `main/http_server/pool_session_api.c`, `main/http_server/pool_session_api.h`.

**Modified (20)** — `components/pool_operation_coordinator/`: `include/pool_operation_coordinator.h`, `include/pool_operation_policy.h`, `pool_operation_coordinator.c`, `pool_operation_policy.c`, `test/test_pool_operation_policy.c` · `components/pool_session_execution/`: `include/pool_session_execution.h`, `pool_session_execution.c`, `test/test_pool_session_execution.c` · `components/pool_session_runtime/`: `include/pool_session_runtime.h`, `include/pool_session_runtime_admission.h`, `pool_session_runtime.c`, `pool_session_runtime_admission.c` · `main/CMakeLists.txt`, `main/Kconfig.projbuild`, `main/main.c`, `main/nx_execution_glue.c`, `main/nx_execution_glue.h`, `main/http_server/http_server.c`, `main/http_server/openapi.yaml`, `test/CMakeLists.txt`.

---

## 12. Gate B9 prerequisites

1. Branch from this committed B8 head.
2. **B9 is frontend / product integration** — the create route is fully operational in the same boot, so basic executor adoption is no longer a B9 item.
3. Resolve (or explicitly defer again) the **open trusted-time source decision** before any hardware trusted-time pilot.
4. Preserve: `CONFIG_NX_TIMED_SESSIONS_API` default n and its two dependencies; the four build postures; strict parser and unknown-field rejection; no password / source / session-id input; the static bounded mailbox with a single B6 owner-task consumer; internal source capture; B3 commit + independent readback before every B5 proof; the audited no-mutation aborts and the never-release-on-uncertainty rule; Restore Now as a same-owner transition; safe terminal acknowledgement with a mandatory `STORE_CLEARED`; sanitized status; standardized 409 responses; the PATCH / restart / OTA fences before their first side effect; the bounded heartbeat and its ordered fail-safe; and every B1–B7 invariant. QEMU ≥ 693 green; frontend 1052.

**Hardware pilot verdict: NOT READY.**
