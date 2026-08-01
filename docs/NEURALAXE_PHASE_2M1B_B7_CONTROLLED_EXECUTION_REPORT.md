# NeuralAxe OS — Phase 2M.1B, Gate B7

## Controlled Pool Apply, Source Restore and Live Protocol Verification Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-execution`
**Commit:** `93f441c` — `feat: add fenced controlled pool apply, restore and hardware-verified execution` (**v2.14.2-59-g93f441c**, parent `8e5cd9d` = the committed Gate B6 report; 31 files, 11,685 insertions / 18 deletions).
**Scope:** the first layer permitted to EXECUTE a timed-session flow: apply the persisted target pool through an audited transaction, verify it with live protocol and hardware evidence, grant target mining, monitor health, restore the immutable source identity, and persist COMPLETE only after verified source mining resumes. Everything sits behind the new `CONFIG_NX_TIMED_SESSIONS_EXECUTION` (depends on `CONFIG_NX_TIMED_SESSIONS`, **default n**, never auto-enabled). Keep-current-password only: no password is ever read, written, transported or logged by any B7 path.

Architectural sources of truth: the committed Phase 2M.1A reports and the B1–B6 foundation reports. Gate B7 was accepted through seven owner review rounds; every correction those rounds mandated is part of the committed implementation and is documented below.

---

## 1. Verdict

**Gate B7 PASS (Phase A seven-round owner review + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 31 approved B7 files: 18 modified + 13 added (all additions inside `components/pool_session_execution/`, `components/pool_session_runtime/` admission, `main/nx_execution_glue.*`) |
| Ancestry | `93f441c` → `8e5cd9d` (B6 report) → `00f077e` (B6 corrective) → `57fe248` (B6); verified read-only |
| Feature flags | `CONFIG_NX_TIMED_SESSIONS_EXECUTION` **default n**, depends on `CONFIG_NX_TIMED_SESSIONS` |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** (complete run) |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **603 Tests, 0 Failures, 0 Ignored** (450 B1–B6 baseline + **153** B7 tests; 603 PASS lines, 0 FAIL lines) |
| Posture A (both flags OFF) | build exit 0; **0** runtime / execution / fence symbols in the ELF — stock behaviour byte-identical |
| Posture B (`NX_TIMED_SESSIONS` only) | build exit 0; 31 B6 + 3 fence symbols, **0 execution symbols** |
| Posture C (both flags ON) | build exit 0; 32 B6 + **56 execution** + 3 fence symbols |
| Strict compile | **14 / 14 translation units clean under `-Wall -Wextra -Werror`** (incl. `bm1370.c`, `mining.c`, `create_jobs_task.c`, `asic_result_task.c`) |
| Locked-span static scan | **37** gate-lock spans, **0** adapter / IO / logging / allocation calls inside any span |
| Secret / privacy scan | committed diff clean: no IPs, credentials, key material, real pool hosts or personal data; synthetic `*.example` fixtures and the public Stratum-documentation coinbase example only |
| Tree after verification | clean; `test-ci/CMakeLists.txt` byte-exact stub (22 B); no build artifacts, logs or release exports |
| Access | no hardware, COM/USB, flashing, physical-device NVS, real NTP, live DNS, real pools/accounts/wallets/credentials, OTA or device restarts; all builds, QEMU and audits ran offline |

---

## 2. What Gate B7 delivers

A new self-contained component **`components/pool_session_execution/`** in two layers, plus production glue and a production mutation fence:

- **Pure decision core** (`pool_session_execution_core.{h,c}`): the 21-state / 41-reason executor substate contract, gate-posture and mutation-permission maps, config-transaction evaluation (exact field-by-field desired-vs-effective comparison), evidence evaluation, the target-mining grant model, the TLS-mode representability gate, and the **generation-unique work contract** primitives (canonical work facts, exact facts comparison, discriminator capability, non-wrapping tag arithmetic). Deterministic, total, no ESP-IDF includes.
- **Engine** (`pool_session_execution.{c,h}`): the executor driven in bounded `pool_session_executor_step()` calls by the SINGLE committed B6 owner task (it is not a task and holds the SAME B5 lease the runtime reconstructed); the ASIC job-delivery gate; the delivered-work registry; the live-identity slot pool; the per-boot discriminator allocator; lock-depth instrumentation. Every external effect flows through two injected bounded adapter vtables (configuration, protocol).
- **Production adapters** (`main/nx_execution_glue.{c,h}`): flash-level effective-config readback, staging through the audited writer (never a password key), live-copy refresh through the identity slot pool, controlled protocol lifecycle via the coordinator hooks.
- **Production mutation fence** (`components/pool_session_runtime/pool_session_runtime_admission.{h,c}` + call sites in `http_server.c`, `bap_handlers.c`, OTA and restart paths): while a timed session owns the configuration, foreign mutations and restarts answer **409 / refused** under the B5 ownership authority.
- **Controlled protocol hooks** (`protocol_coordinator.{c,h}`, `stratum_api.c`, `stratum_v1_task.c`): single-owner controlled start/stop/poll/counters with drained event queues, restart inhibition on the controlled instance, and a borrowed identity-slot lifetime for the running protocol task.
- **Delivery-path instrumentation** (`bm1370.c`, `create_jobs_task.c`, `asic_result_task.c`): the delivery gate consult, canonical-facts registration before the job frame reaches the chip, the extranonce2 generation-domain embed, and local proof-of-work resolution of every hardware result.

Durable truth is unchanged: B1 transition records commit through the SAME B3 store instance, verified by independent reload and exact comparison, proven to the SAME B5 coordinator under the current token. RESTARTING/VERIFYING states remain RAM-only, so any crash inside a verification span conservatively re-enters through B4 boot recovery (restore-first).

---

## 3. The ASIC verification guarantee — stated in three separate parts

Conflating these was a defect the review rounds corrected; the committed documentation states them apart:

1. **Temporal freshness** is established by the generation-unique work contract (§4) — a protocol-valid generation discriminator embedded in the hashed header plus EXACT canonical header comparison at delivery. It is NOT established by proof of work: a pool may resend the same template after reconnect and the firmware's extranonce2 counter restarts at 0 per work item, so a new generation can rebuild a byte-identical 80-byte header for which a delayed old result is GENUINELY valid. Such work is **excluded deterministically** — never estimated.
2. **Nonce/header binding** is probabilistic local proof of work at the ASIC's own fixed result difficulty — `DEVICE_CONFIG.family.asic.difficulty` = **256** for the BM1370, a compiled `static const` written once at init, clamped into the band [256, 65536]. Pool share difficulty (vardiff) is NEVER an input on this path: it is unvalidated, pool-controlled, and 0.0 before the first difficulty message. A stray nonce for a *different* header clears difficulty 256 with probability 1/(256·2³²) = 2⁻⁴⁰ ≈ 9.1×10⁻¹³.
3. **The two-proof requirement** (`POOL_EXEC_REQUIRED_WORK_PROOFS` = 2 distinct delivered records, one-time consumption each) squares that bound **only under precondition 1**. Expected proof cadence at the Gamma 601 operating point (~1.07 TH/s from the compiled table) is ~1.03 s per proof against 120 s target / 180 s source windows.

No deterministic ASIC reset/quiesce/drain barrier is claimed anywhere: the repository documents no BM1370 in-flight-work behaviour on reset, no required hold time and no residual-result bound, so any such barrier would rest on undocumented silicon behaviour. Window expiry is always fail-closed (target → restore; source → RESTORE_FAILED with the obligation retained), never a false COMPLETE.

---

## 4. The generation-unique work contract

**The hazard, proven with the real construction code:** `create_jobs_task.c` resets `extranonce_2 = 0` for every dequeued work item, and the QEMU suite proves two connections receiving the same provider template with the same extranonce1 produce byte-identical canonical facts — and that a nonce found for the first header passes real double-SHA256 validation against the second.

**The discriminator — extranonce2 generation domain.** The one header field this firmware legitimately owns. The tag occupies bits 24–31 of the extranonce2 counter; the low 24 bits remain rolling space. Stratum V1 serializes the counter little-endian (tag = byte 3, inside the coinbase → merkle root → hashed header) whenever the pool grants width ≥ 4; SV2 **extended** channels encode big-endian (tag at byte [len−4]), same width rule; SV2 **standard** channels give the miner NO coinbase bytes — capability NONE, evidence path fails closed. Rejected alternatives, for the record: version-bit reservation (would restrict the chip's hardware roll mask — undocumented in-flight behaviour) and ntime manipulation (untrusted pool time, share-validity risk). Every embedded value remains pool-valid and is submitted unchanged: the QEMU suite proves the V1 round trip (the pool, rebuilding the coinbase from the SUBMITTED string, reproduces the delivered merkle root byte-for-byte, with the tag visible unmangled) and the SV2-extended round trip (hex encode → decode reproduces the delivered bytes exactly).

**Non-wrapping per-boot allocation.** Tags are NEVER derived by modulo arithmetic. `pool_exec_generation_tag(index)` maps allocation index 1…127 to 0x81…0xFF and returns the fail-closed 0 for everything else — a wrapped or reused domain is unrepresentable. The engine allocator consumes exactly one index per controlled protocol start, strictly monotonically, from the per-boot budget `POOL_EXEC_GENERATION_TAG_LIMIT` = 127; only the boot-time `gate_reset` returns the budget (deinit and rebind never do). On exhaustion the executor **refuses the controlled start entirely** — no connection, no delivered verification work, no reused domain (`start_calls == 0` proven) — reporting `EXEC_REASON_GENERATION_EXHAUSTED` through the ordinary bounded failure paths into the stable held postures: target mining is never granted, source COMPLETE stays impossible, `restore_required` is retained.

**Rolling-domain guard.** A counter that no longer fits 24 bits would silently overflow INTO the tag byte, so the embed query refuses any counter above `POOL_EXEC_GENERATION_COUNTER_MASK`: the item goes out with stock untagged bytes and can never be verification evidence. Reaching the bound would take 2²⁴ jobs under ONE template in ONE generation (~97 days at the 601 job cadence) — but the check does not rely on that arithmetic.

**Exact comparison is the authority.** Every delivered item is registered — inside the same lock that publishes the active-job entry and BEFORE the job frame is serialized to the chip — with its exact canonical facts: base version, prev_block_hash and merkle_root in bm_job order, ntime, nbits (precisely the fields `test_nonce_value()` hashes; the roll mask is deliberately excluded because the chip may choose identical rolled bits in both generations). `header_unique_for_generation` is decided there by field-by-field comparison (never a whole-struct memcmp, never a short hash) against every still-relevant prior-generation record, including the record being overwritten. To make that comparison set exist, `begin_work_generation` RETAINS records instead of wiping them; a record retires only when its chip slot is reused or at boot reset. A retained record can never be credited (stale generation stamp) — it can only EXCLUDE a new identical header.

**Resolution.** The BM1370 allocates job ids as `(id + 24) % 128` — 16 distinct ids, each reused every 16 deliveries — and the result frame carries no sequence number, generation or delivery token. Every hardware result therefore resolves against the registry: verdict `ACCEPTED` (the ONLY evidence increment) requires current protocol + config + work generations, the nonce proving THIS exact work item at the fixed local threshold, a discriminator-carrying record, a proven-unique header, and first consumption. Everything else is counted by verdict: `UNKNOWN_JOB`, `STALE_GENERATION`, `WORK_MISMATCH`, `ALREADY_CONSUMED`, `NO_DISCRIMINATOR`, `HEADER_ALIASED`, `INVALID`. Register reads are chip liveness only and can never satisfy a verification.

---

## 5. Reader-lifetime and blocking-IO contracts

All published-identity storage is static, written once before publication, immutable afterwards, never reused within an epoch, reclaimed only at quiescent points, and bounded to 16 slots derived from the B1 retry budgets; `refcount` counts live reader borrows ONLY (publication is tracked separately), and reclamation refuses pinned slots.

| Reader | Contract |
|---|---|
| V1 share-submit user (`asic_result_task.c`) | copy-under-lock: `identity_copy()` acquires → copies all fields → **releases → returns**; only the caller-owned copy lives across the blocking socket write |
| BAP `pool`/`poolUser` status | same `identity_copy()` shape; JSON built after release |
| HTTP `/api/system` JSON | never touches the engine lock: `nvs_config_get_string()` fresh heap copies |
| Coordinator probes / `s_primary_url` | plain coordinator-task reads; the primary URL is a bounded caller-owned copy, not a retained pointer |
| Controlled stratum instances | slot borrow (refcount) taken before task start, released on proven exit; immutable storage read lock-free for the task lifetime |
| ASIC result path | `test_nonce_value` runs with no lock; registry ops are bounded CPU-only locked sections |

**Machine-checked, two ways.** Dynamic: every take/release of the module's one spinlock goes through depth-instrumented wrappers (`pool_session_execution_lock_depth()`), and all twelve fake blocking boundaries — 4 NVS-store, 4 config-adapter, 4 protocol fakes — assert depth 0 on entry, making the entire 603-test run a continuous proof; dedicated tests additionally prove every registry/identity operation returns at depth 0, a copied identity stays byte-stable through later publications (and never blocks them), and 300 mixed registry/result operations invoke zero adapters. Static: a scan of all 37 locked spans found 0 adapter, IO, logging, allocation or task-delay calls inside any span.

---

## 6. Fail-closed postures

| Condition | Posture |
|---|---|
| Discriminator budget exhausted (127 controlled starts in one boot) | controlled start refused outright; `EXEC_REASON_GENERATION_EXHAUSTED`; held states retain `restore_required` |
| Rolling counter outgrows 24 bits | embed refused; item untagged; never evidence |
| SV2 standard channel / extranonce2 width < 4 | capability NONE; `NO_DISCRIMINATOR`; verification unavailable; windows expire into restore/recovery |
| Identical rebuilt header (template resend) | `NO_DISCRIMINATOR` / `HEADER_ALIASED`; excluded deterministically |
| Verification window expiry | target → bounded rollback/restore; source → `RESTORE_FAILED_HELD`, obligation retained |
| Custom-TLS mode not representable | refused before any mutation |
| Foreign mutation / restart during owned session | 409 / refused by the B5 admission fence |
| Identity slot exhaustion | no publication, no protocol start |
| Config readback failure / partial / foreign | transaction fails closed; rollback path |
| Executor deinit mid-execution | held gate stays held; work domain closes; budget NOT refunded |

---

## 7. Test inventory

**153 Gate B7 tests** (QEMU 603 = 450 B1–B6 baseline + 153), all green ×: flag/substate contract maps; config transaction (exact comparison, timeout, partial, foreign, uncertain, readback failure); evidence evaluation (anomaly, deltas, register-reads-never-verify); grant lifecycle (issue/revoke/stale bindings); interruption/crash re-entry; delivered-work registry with REAL proof-of-work at deterministic search scale (nonce binds to its own job and no other; delayed results across reused job ids and generations rejected); the identical-header reconnect proof and its deterministic exclusion; discriminator capability matrix, tag arithmetic, allocator sweep (final tag works, next request fails, never reissued, boot-only refund), rolling boundary; V1 and SV2-extended share round-trips; SV2-standard/narrow-width fail-closed; two-stale-slots cannot fake the two-proof requirement; executor-level exhaustion postures (source held fail-closed with zero protocol starts; target never grants); reader-lifetime suite (complete-A-or-B never mixed, borrow pins, epoch bounds, exhaustion); lock-depth-per-operation, copy-outlives-lock, registry-never-touches-adapters; B5 admission fence matrix (13); privacy assertions throughout (synthetic fixtures only).

---

## 8. Preserved invariants

Single mutating session owner (B6 task drives the executor; same B5 lease). B3 dual-slot persistence ordering and generation-aware proposal tracking untouched. B4 restore-first crash recovery unchanged. Keep-current-password only — no password field exists in any B7 model, no adapter call carries one, and the configuration contract forbids touching either stored password key. Custom-TLS rejection before mutation. Production mutation and restart fences. Controlled reconnect with drained event queues. Bounded immutable identity storage. `restore_required` set with the first APPLY_TARGET intent and cleared ONLY at COMPLETE. Execution flag default n; posture A is byte-for-byte stock behaviour. No Weather-Aware Tuning integration.

---

## 9. Known limits — hardware-pilot NOT READY

- **No deterministic ASIC generation barrier exists or is claimed.** The temporal-freshness guarantee is the discriminator + exact-comparison contract; the binding guarantee is probabilistic at fixed difficulty 256. Physical validation of BM1370 reset/drain behaviour has not been performed and is prerequisite to any stronger claim.
- **The discriminator uniqueness contract is per controlled-execution lifetime (one runtime boot):** 127 controlled protocol starts per boot, then verification is unavailable until reboot — a deliberate fail-closed ceiling, far above any bounded session's retry budgets (≤ 16 starts per session).
- **SV2 standard channels cannot host a discriminator**; timed-session verification on them is unavailable by design (fail-closed), not degraded.
- B7 execution remains **default-disabled**. Real-hardware pilots require a separate, explicitly scoped gate (B8+) with owner-supervised flashing — not authorized by this report.

---

## 10. Phase B verification appendix

All commands ran offline against the committed tree (`93f441c`, clean status), Docker `espressif/idf:v5.5.3` for builds and the `nx-qemu-action` image for QEMU only:

1. `git show --stat/--name-status HEAD` — 31 files, 18 M + 13 A, ancestry verified. Read-only git throughout; the owner performed the commit.
2. `npm run test:gate` — 1052/1052, exit 0.
3. Fresh `test-ci` build — exit 0; QEMU full suite — **603 Tests, 0 Failures, 0 Ignored**.
4. Postures A/B/C from pristine sdkconfig copies — exit 0 each; `xtensa-esp32s3-elf-nm` symbol counts as tabled in §1 (tracked `sdkconfig` untouched).
5. Strict compile of the 14 B7-touched translation units — 0 failures.
6. Locked-span static scan — 37 spans, 0 blocking calls.
7. Secret/privacy scan of the committed diff — clean.
8. `test-ci/CMakeLists.txt` restored to the byte-exact 22-byte stub; tree clean after verification.
