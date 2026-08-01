#ifndef POOL_SESSION_EXECUTION_H_
#define POOL_SESSION_EXECUTION_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session_execution_core.h"
#include "pool_session_runtime.h"

/*
 * NeuralAxe timed pool sessions — controlled-execution engine
 * (Phase 2M.1B, Gate B7). Board 601 / BM1370 only.
 *
 * The executor is the FIRST layer permitted to execute controlled pool
 * apply, source restoration and live protocol verification. It is NOT a
 * task: the single committed B6 runtime owner task drives it through
 * bounded pool_session_executor_step() calls, so exactly one mutating
 * session owner continues to exist (non-negotiable contract 3). It holds
 * the SAME B5 lease the runtime reconstructed — it never acquires a second
 * competing lease.
 *
 * EVERY external effect flows through two injected, bounded adapter
 * vtables (configuration and protocol). Production adapters live in main/
 * (they need nvs_config/SYSTEM_MODULE/protocol_coordinator); tests inject
 * deterministic fakes and NEVER perform outbound networking. Adapter calls
 * are synchronous and bounded; they may not invoke callbacks, write the
 * session store, transition B5, grant mining or restart.
 *
 * DURABLE-TRUTH MODEL: the bound PoolSessionRuntime's committed record and
 * generation remain the ONE committed truth. The executor commits B1
 * transition records through the SAME B3 store instance, verifies every
 * commit by an independent reload and EXACT field-by-field comparison,
 * proves it to the SAME B5 coordinator under the CURRENT token, and then
 * updates rt->record/rt->committed_generation in place. Only B1 persistent
 * states are committed; the RESTARTING/VERIFYING states remain RAM-only by
 * the committed B3 validation rule, so any crash inside a verification
 * span conservatively re-enters through B4 boot recovery (restore-first).
 *
 * While the executor OWNS the session flow (pool_exec_state_owns_flow),
 * the B6 task defers its B4 plan-persistence and lease-reconcile machinery
 * to the executor — one mutating owner, one truth. The B6 task still
 * refreshes the B2 time snapshot and the B4 plan every re-evaluation; the
 * executor consumes the plan (deadline expiry via trusted time) as INPUT.
 *
 * KEEP-CURRENT-PASSWORD ONLY: no adapter call carries a password, no
 * password field exists in any model here, and the configuration adapter
 * contract forbids touching either stored password key.
 */

/* ------------------------------------------------------------------ */
/* Configuration adapter (bounded, synchronous, password-free)         */
/* ------------------------------------------------------------------ */

typedef struct {
    /*
     * Report the device identity for the fail-closed board/ASIC check.
     * Bounded copies only; false when unavailable (fails closed).
     */
    bool (*device_identity)(void *ctx, char *board, size_t board_cap,
                            char *asic, size_t asic_cap);

    /*
     * Stage the COMPLETE desired identity into the audited configuration
     * writer (primary + fallback + role; NEVER a password key). Returns
     * false ONLY for a definite pre-enqueue rejection with nothing staged.
     * A true return proves nothing — only the independent readback does.
     */
    bool (*stage_apply)(void *ctx, const PoolConfigIdentity *identity);

    /*
     * INDEPENDENT effective-configuration readback (flash-level in
     * production — never the writer's RAM cache). *out is fully written;
     * out->valid=false when the readback itself failed.
     */
    void (*read_effective)(void *ctx, PoolExecEffectiveConfig *out);

    /*
     * Refresh the live protocol-facing configuration copies from the
     * verified effective configuration. MUST only be called while no
     * session protocol task exists (the engine guarantees it). Never
     * touches a password. Returns false on a definite failure.
     */
    bool (*refresh_live)(void *ctx, const PoolConfigIdentity *identity);
} PoolExecConfigOps;

/* ------------------------------------------------------------------ */
/* Protocol adapter (controlled lifecycle; bounded events only)        */
/* ------------------------------------------------------------------ */

typedef struct {
    /*
     * Controlled protocol start for the CURRENT live configuration.
     * The adapter drains every stale queued event before starting, so
     * events polled afterwards belong to this connection generation.
     * Returns false when no task could be started.
     */
    bool (*start)(void *ctx, PoolSessionProtocol protocol);

    /* Bounded controlled stop handshake. False = the task did not
     * provably exit (PROTOCOL_SHUTDOWN_FAILED). */
    bool (*stop)(void *ctx);

    /* True while a controlled session protocol task is believed running. */
    bool (*running)(void *ctx);

    /* Drain pending bounded protocol events (EXEC_PEVT_* bits). */
    uint32_t (*poll_events)(void *ctx);

    /* Sample the audited counters (work_received, shares, queue depth).
     * jobs_forwarded is filled by the engine from the delivery gate. */
    void (*counters)(void *ctx, PoolExecProtocolCounters *out);

    /*
     * Post-COMPLETE handoff: start the production protocol coordinator so
     * normal source operation (failover, pause, heartbeat) resumes. Called
     * at most once, only after COMPLETE is durable and the controlled task
     * is stopped. Returns false on a definite failure.
     */
    bool (*handoff_source)(void *ctx);
} PoolExecProtocolOps;

/* ------------------------------------------------------------------ */
/* Executor instance (caller-owned; driven by the B6 owner task)       */
/* ------------------------------------------------------------------ */

typedef struct {
    bool initialized;
    bool bound;
    bool system_ready;       /* mining runtime (ASIC/jobs/wifi) is up      */

    PoolSessionRuntime        *rt;          /* the ONE bound B6 runtime    */
    const PoolExecConfigOps   *config_ops;
    void                      *config_ctx;
    const PoolExecProtocolOps *proto_ops;
    void                      *proto_ctx;
    PoolExecPolicy             policy;

    PoolExecState             state;
    PoolExecReason            reason;
    PoolExecConfigApplyResult last_apply_result;

    /* Live B1 session (loaded from the committed record at entry). */
    PoolSession session;
    bool        session_loaded;

    /* Bounded work buffers (kept off the task stack). */
    PoolSessionRecord       staged;
    PoolSessionRecord       reloaded;
    PoolExecEffectiveConfig cfg_pre;
    PoolExecEffectiveConfig cfg_now;
    bool                    cfg_pre_valid;
    bool                    stage_rejected;
    bool                    stage_started;

    /* Which identity the current transaction targets (1 target, 2 source). */
    uint8_t apply_role;

    /* Protocol/work generations and generation-bound evidence baselines.
     * `work_generation` is the gate-owned epoch that binds software
     * delivery and hardware results; it is re-issued on every controlled
     * protocol start together with `protocol_generation`. */
    uint32_t                 protocol_generation;
    uint32_t                 work_generation;
    uint32_t                 config_generation;
    PoolExecProtocolCounters baseline;
    uint32_t                 events_seen;   /* sanitized bits this generation */
    bool                     connection_observed_ram;
    bool                     identity_observed_ram;

    /* Bounded monotonic phase deadlines (armed per substate). */
    bool     phase_deadline_valid;
    uint64_t phase_deadline_us;
    bool     job_deadline_valid;
    uint64_t job_deadline_us;

    /* Session deadline (armed at grant from the B4 plan's remaining). */
    bool     session_deadline_valid;
    uint64_t session_deadline_us;
    bool     entry_remaining_valid;
    uint64_t entry_remaining_s;

    /* The target-mining grant (RAM-only, revocable). */
    PoolExecMiningGrant grant;
    uint32_t            issue_sequence;

    /* Bounded attempt counters not covered by the B1 retry budgets. */
    uint8_t stop_attempts;
    bool    handoff_attempted;
    bool    target_health_ok;
    bool    asic_evidence_seen; /* hardware fact seen this work generation */
    bool    generation_exhausted; /* per-boot discriminator budget spent —
                                   * no further controlled start is issued */

    /* Audit counters. */
    uint32_t commit_count;
    uint32_t event_sequence;

    PoolExecutionSnapshot snapshot;
} PoolSessionExecutor;

/* ------------------------------------------------------------------ */
/* Lifecycle (single-threaded: only the B6 owner task may call these)  */
/* ------------------------------------------------------------------ */

/* Zero to the fail-closed DISABLED posture. */
void pool_session_executor_init(PoolSessionExecutor *ex);

/*
 * Bind the executor to the booted B6 runtime and its adapters. Validates
 * every vtable entry; a NULL anywhere refuses the bind (fail closed).
 * `policy` may be NULL for the committed defaults; any policy is clamped.
 * On success the executor is IDLE; it acts only when the runtime exposes
 * an action posture (VERIFY_TARGET_PENDING / RESTORE_SOURCE_PENDING).
 */
PoolExecReason pool_session_executor_bind(PoolSessionExecutor *ex,
                                          PoolSessionRuntime *rt,
                                          const PoolExecConfigOps *config_ops,
                                          void *config_ctx,
                                          const PoolExecProtocolOps *proto_ops,
                                          void *proto_ctx,
                                          const PoolExecPolicy *policy);

/* Unbind and zero. Never releases ownership, never clears the record,
 * never reopens the delivery gate (a held gate stays held). */
void pool_session_executor_deinit(PoolSessionExecutor *ex);

/* The mining runtime (ASIC init + job pipeline + network) is available.
 * The executor performs NO external action before this is true. */
void pool_session_executor_set_system_ready(PoolSessionExecutor *ex, bool ready);

/*
 * ONE bounded execution step. Deterministic: consumes the current runtime
 * posture, adapter observations and monotonic time; performs at most one
 * action boundary (commit chain + state move). Returns the resulting
 * substate. Total and fail-closed for NULL/unbound executors.
 */
PoolExecState pool_session_executor_step(PoolSessionExecutor *ex);

/* True while the executor OWNS the session flow (the B6 task then defers
 * its plan-persistence/lease-reconcile machinery). */
bool pool_session_executor_owns_flow(const PoolSessionExecutor *ex);

/* ------------------------------------------------------------------ */
/* Gate B8 additions — same-boot adoption and externally caused restore */
/* ------------------------------------------------------------------ */

/*
 * ADOPT A SESSION CREATED DURING THIS BOOT.
 *
 * The committed entry path (exec_handle_entry) is BOOT-shaped: it always
 * applies DEVICE_RESTART_OBSERVED first and enters only from the B4 resume
 * or restore postures. A session created by the Gate B8 API in the SAME
 * boot has neither, so it needs this explicit, narrowly scoped entry.
 *
 * Preconditions (all fail closed, none mutate anything):
 *  - the executor is bound, system-ready and IDLE;
 *  - the durable record is a SESSION in TARGET_SNAPSHOT_COMMITTED with
 *    restore_required == false and Keep-current-password policy;
 *  - the current B5 token owns a session-class lease in ACTIVE;
 *  - board 601 / BM1370 and a representable effective TLS mode.
 *
 * On success it drives the committed B1 TARGET_APPLY_REQUESTED boundary —
 * committing APPLYING_TARGET with restore_required == true, reloading it
 * independently, verifying it exactly and proving it to B5 — and only THEN
 * arms the configuration transaction. No pool key is written by this call:
 * the first staging happens on the NEXT executor step, strictly after the
 * obligation is durable.
 *
 * Returns EXEC_REASON_NONE when the executor now owns the flow.
 */
PoolExecReason pool_session_executor_adopt_created_session(PoolSessionExecutor *ex);

/*
 * Externally caused restoration (Gate B8 heartbeat fail-safe).
 *
 * Closes the ASIC delivery gate FIRST, revokes the target-mining grant,
 * then drives the committed restore entry with the given cause. Admissible
 * only while the executor owns a target-side flow; every other posture is
 * refused without side effects. The restoration itself then proceeds
 * through the ordinary committed bounded restore path.
 */
PoolExecReason pool_session_executor_request_restore(PoolSessionExecutor *ex,
                                                     PoolExecReason cause);

/* True while a valid target-mining grant is currently issued. */
bool pool_session_executor_grant_active(const PoolSessionExecutor *ex);

/* Copy the sanitized execution snapshot (string-free by construction). */
PoolExecReason pool_session_executor_snapshot(const PoolSessionExecutor *ex,
                                              PoolExecutionSnapshot *out);

/* ------------------------------------------------------------------ */
/* ASIC job-delivery gate (module-wide; single writer = the executor)  */
/* ------------------------------------------------------------------ */

/*
 * The ONE release point for ASIC work during session execution. The job
 * pipeline consults pool_session_execution_asic_work_allowed() before
 * releasing a job to the ASIC (compiled into the pipeline only under
 * CONFIG_NX_TIMED_SESSIONS_EXECUTION) and reports each released job via
 * pool_session_execution_note_job_forwarded().
 *
 * The gate posture is written ONLY by the executor engine (single writer,
 * the B6 owner task) and read concurrently by the job pipeline; both go
 * through a spinlock so reads are never torn. The zero/reset posture is
 * DEFAULT_OPEN — see the pool_exec_state_gate() contract for why that is
 * safe (target work cannot exist before the executor sets INHIBITED).
 *
 * WORK GENERATION: the gate also owns the work generation that binds
 * software delivery and HARDWARE results together. The executor bumps it on
 * every controlled protocol start; the delivery and ASIC-result counters
 * are per-generation and are ZEROED by that bump, so evidence produced
 * under an earlier protocol/configuration/job generation can never be
 * observed by a later window.
 */
bool pool_session_execution_asic_work_allowed(void);
void pool_session_execution_note_job_forwarded(void);

/* ------------------------------------------------------------------ */
/* Delivered-work registry (per-work-item generation binding)          */
/* ------------------------------------------------------------------ */

/*
 * A counter that merely resets on a generation change is NOT enough: the
 * BM1370 keeps returning nonces for jobs delivered BEFORE a reconfiguration
 * for as long as those job slots live, so a stale result could be credited
 * to the new generation. Every delivered work item is therefore STAMPED
 * with the exact protocol, configuration and work generation that produced
 * it, and every hardware result is RESOLVED against that stamp.
 *
 * The registry is indexed by the ASIC job id (the chip's own slot number),
 * mirroring the firmware's active-job table, and is bounded and static.
 */
#define POOL_EXEC_JOB_SLOTS 128u

/*
 * THE JOB-ID ALIASING PROBLEM AND ITS RESOLUTION.
 *
 * The BM1370 allocates job ids as `id = (id + 24) % 128`, so only 16
 * distinct ids exist and each is REUSED every 16 deliveries. A generation
 * change that merely wipes a job-id-indexed table therefore does NOT stop a
 * delayed result from work A (job id X, old generation) being attributed to
 * work B (same job id X, new generation) once X has been reused.
 *
 * The hardware result carries only: the job id (high nibble of `job.id`),
 * the 32-bit nonce, a 16-bit version field and core/asic ids. It carries NO
 * sequence number, NO generation and NO delivery token — so a RAM-only
 * counter could never be validated against it.
 *
 * What the result DOES carry is a proof of work. `test_nonce_value()`
 * rebuilds the 80-byte header from the EXACT job currently registered in
 * that slot (prev_block_hash, merkle_root, ntime, nbits) plus the returned
 * nonce and rolled version, and double-SHA256s it. A nonce produced for a
 * DIFFERENT header yields an essentially random hash, so it meets the job's
 * difficulty target only with probability 1/difficulty. Requiring that
 * proof binds the result to one exact delivered work item and survives job
 * id reuse — the binding is cryptographic, not bookkeeping.
 *
 * The caller (the ASIC result pipeline) performs that computation, which it
 * already does for share evaluation, and passes the verdict as `work_bound`.
 * NO pool share is required and none is submitted: the proof is entirely
 * local.
 */

/*
 * THE IDENTICAL-HEADER ALIAS — why proof of work alone cannot establish
 * temporal freshness. A pool may resend the same effective template after
 * a reconnect, and the firmware restarts its extranonce2 counter at 0 for
 * every dequeued work item, so a NEW protocol/work generation can rebuild
 * a BYTE-IDENTICAL 80-byte header. A delayed result for the old header is
 * then genuinely valid for the current one — `test_nonce_value()` cannot
 * tell them apart and no probability bound applies. The registry therefore
 * enforces the GENERATION-UNIQUE WORK CONTRACT (pool_session_execution_
 * core.h): every delivered item carries its exact canonical header facts,
 * uniqueness against every still-relevant prior-generation record is
 * established by EXACT field-by-field comparison BEFORE the job reaches
 * the chip, and only items that BOTH carry a generation discriminator AND
 * are proven header-unique can ever be counted as evidence. Everything
 * else is excluded deterministically — never estimated.
 */

/* Resolution verdicts (stable machine tokens; diagnostics and tests). */
typedef enum {
    EXEC_RESULT_ACCEPTED = 0,      /* unique current work, bound, first     */
    EXEC_RESULT_UNKNOWN_JOB,       /* no delivery record for this job id    */
    EXEC_RESULT_STALE_GENERATION,  /* delivered under an older generation   */
    EXEC_RESULT_ALREADY_CONSUMED,  /* this work item already counted once   */
    EXEC_RESULT_WORK_MISMATCH,     /* nonce does not prove THIS work item   */
    EXEC_RESULT_INVALID,           /* out-of-range job id                   */
    EXEC_RESULT_NO_DISCRIMINATOR,  /* work carries no generation domain     */
    EXEC_RESULT_HEADER_ALIASED,    /* header equals a prior-generation one  */
    POOL_EXEC_RESULT_VERDICT__COUNT
} PoolExecResultVerdict;

/* The effective local threshold for an ASIC-reported difficulty setting.
 * Clamped into the compiled band declared in pool_session_execution_core.h;
 * the POOL difficulty is never an input on this path. */
double pool_session_execution_work_proof_threshold(uint32_t asic_difficulty);

const char *pool_session_execution_result_verdict_str(PoolExecResultVerdict v);

/*
 * Record one work item delivered to the ASIC, stamped with the CURRENT
 * generations and its EXACT canonical header facts. Called from the ASIC
 * send path with the real chip job id, inside the same lock that publishes
 * the active-job entry and BEFORE the job frame is written to the chip, so
 * `header_unique_for_generation` is established before delivery.
 *
 * Uniqueness is decided HERE, by exact field-by-field comparison against
 * every still-relevant record from an older generation — including the
 * record this delivery overwrites. Records are RETAINED across generation
 * changes precisely so this comparison set exists; a record stops being
 * relevant only when its chip slot is reused (the old item can then no
 * longer produce an attributable result). `facts == NULL` fails closed:
 * the record registers but can never be unique, so it is never evidence.
 */
void pool_session_execution_note_work_delivered(uint8_t job_id,
                                                const PoolExecWorkFacts *facts);

/*
 * The extranonce2 generation-domain query — the ONE embed point. The job
 * pipeline calls this with the ACTIVE protocol facts and its raw rolling
 * counter immediately before serializing extranonce2. While a controlled
 * work domain is active AND the capability decision (pure core) supports
 * a discriminator at this width, *tagged receives the counter with the
 * generation tag in bits 24..31 and the call returns true; the delivery
 * that follows is stamped as discriminator-carrying. In every other case
 * it returns false and the caller uses its counter unchanged — byte-for-
 * byte stock behaviour outside controlled sessions.
 */
bool pool_session_execution_extranonce2_tag(bool protocol_v2,
                                            bool sv2_extended_channel,
                                            uint32_t extranonce2_len,
                                            uint64_t counter,
                                            uint64_t *tagged);

/*
 * Work-domain activation — executor-only (single writer). Set true when a
 * controlled protocol connection starts (after the work-generation bump),
 * false once the controlled task provably stopped. While inactive the tag
 * query always declines, so normal mining never carries a tag.
 */
void pool_session_execution_set_work_domain_active(bool active);
bool pool_session_execution_work_domain_active(void);

/*
 * PER-BOOT NON-WRAPPING DISCRIMINATOR ALLOCATION — executor-only, called
 * once per controlled protocol start BEFORE any protocol action. Each
 * success consumes one of POOL_EXEC_GENERATION_TAG_LIMIT per-boot tags
 * (0x81..0xFF, strictly monotonic, never reissued). Returns false once
 * the budget is spent — permanently for this boot: the executor must
 * then refuse the controlled start entirely (GENERATION_EXHAUSTED /
 * verification-unavailable posture). Only the boot/test gate_reset
 * returns the budget; deinit and rebind never do.
 */
bool pool_session_execution_allocate_generation_tag(void);

/* Diagnostics: the current generation's tag (0 = none), the untouched
 * per-boot budget, the permanent exhaustion latch, and how many embed
 * requests were refused because the rolling counter outgrew its 24-bit
 * space (fail closed — the overflow can never reach the tag byte). */
uint32_t pool_session_execution_generation_tag_current(void);
uint32_t pool_session_execution_generation_tags_remaining(void);
bool     pool_session_execution_generation_exhausted(void);
uint64_t pool_session_execution_rolling_declined(void);

/*
 * LOCK-DEPTH INSTRUMENTATION. The current nesting depth of the module's
 * one spinlock. Deterministic in the single-task test environment; test
 * fakes standing in for every blocking boundary (socket, protocol
 * lifecycle, NVS) assert it is ZERO on entry, proving no lock is ever
 * held across blocking IO. Copies taken under the lock stay valid after
 * release — the COPY may live across a blocking write; the LOCK may not.
 */
uint32_t pool_session_execution_lock_depth(void);

/*
 * Resolve one hardware result against its delivered-work record.
 *
 * `work_bound` is the caller's proof-of-work verdict: true only when the
 * returned nonce, validated against the EXACT job currently registered in
 * this slot, meets max(that job's difficulty target,
 * POOL_EXEC_WORK_BINDING_MIN_DIFF). That is what defeats job-id reuse — a
 * delayed nonce from an older work item cannot prove work for the item that
 * now occupies the slot.
 *
 * Accepts ONLY when: a record exists for this job id, it was stamped with
 * the CURRENT protocol + configuration + work generation, the nonce is
 * bound to that exact work item, and it has not been consumed before.
 * Acceptance consumes the record (one result per delivered work item), so a
 * single job's nonce stream can never masquerade as sustained processing.
 * Every other case is rejected and counted by verdict.
 */
PoolExecResultVerdict pool_session_execution_resolve_asic_result(uint8_t job_id,
                                                                 bool work_bound);

/*
 * The chip answered a register read: liveness only — NOT proof that
 * delivered work was processed, and never able to satisfy a verification.
 */
void pool_session_execution_note_asic_register_read(void);

/*
 * Start a NEW work generation: zeroes the per-generation delivery and ASIC
 * counters and returns the new generation (never 0). Executor-only.
 *
 * Delivered-work records are deliberately RETAINED, not wiped: they become
 * the prior-generation comparison set for the header-uniqueness decision.
 * Wiping them here would erase exactly the knowledge needed to detect a
 * byte-identical header rebuilt after a reconnect — while the chip still
 * holds the old jobs and can still emit delayed results for them. A
 * retained record can never be credited (its generation stamp is stale);
 * it can only ever EXCLUDE a new identical header from evidence.
 */
uint32_t pool_session_execution_begin_work_generation(void);

/* Sanitized per-record view (diagnostics and tests; no header bytes). */
typedef struct {
    bool     occupied;
    bool     consumed;
    bool     facts_valid;
    bool     header_unique;
    uint8_t  discriminator_kind;  /* PoolExecHeaderDiscriminator          */
    uint32_t discriminator_tag;
    uint32_t prior_match_slot;    /* slot whose header matched; UINT32_MAX = none */
    uint32_t protocol_generation;
    uint32_t config_generation;
    uint32_t work_generation;
} PoolExecDeliveredView;

bool pool_session_execution_delivered_view(uint8_t job_id,
                                           PoolExecDeliveredView *out);

/*
 * Start a NEW configuration generation. Called by the executor immediately
 * after a verified live-configuration publication, so work delivered under
 * an older configuration can never be credited to the new one.
 */
uint32_t pool_session_execution_begin_config_generation(void);

/* The executor publishes its protocol generation here so the delivery
 * stamp and the result resolution share one authority. */
void pool_session_execution_set_protocol_generation(uint32_t generation);

/* Current gate posture, generations and per-generation counters. */
PoolExecGatePosture pool_session_execution_gate_posture(void);
uint32_t            pool_session_execution_work_generation(void);
uint32_t            pool_session_execution_config_generation(void);
uint32_t            pool_session_execution_protocol_generation(void);
uint64_t            pool_session_execution_jobs_forwarded(void);
uint64_t            pool_session_execution_asic_job_results(void);
uint64_t            pool_session_execution_asic_register_reads(void);
uint64_t            pool_session_execution_asic_rejected(PoolExecResultVerdict v);

/* Reset the gate to DEFAULT_OPEN with zeroed counters, generation 0 and an
 * empty delivered-work registry. Boot-time and test initialization ONLY. */
void pool_session_execution_gate_reset(void);

/* ------------------------------------------------------------------ */
/* Live-identity slot pool (proven reader lifetime)                    */
/* ------------------------------------------------------------------ */

/*
 * The protocol-facing identity pointers in the application's shared state
 * are read by surfaces this gate does not own (HTTP/BAP status handlers,
 * the share-submit path) which hold a RAW pointer for the duration of a
 * bounded read and follow no lifetime protocol. Two rotating buffers do
 * NOT make that safe: a long-lived reader can be overwritten in place.
 *
 * OWNERSHIP MODEL (proven, not assumed):
 *  1. All storage is STATIC and is NEVER freed, so no reader can ever
 *     observe freed memory — a use-after-free is structurally impossible.
 *  2. A slot is written EXACTLY ONCE, before it is published. After
 *     publication its bytes are immutable for the rest of the epoch, so a
 *     reader can never observe a torn or mixed identity either.
 *  3. Within one execution epoch a slot is NEVER reused: acquire() only
 *     hands out slots untouched since the last reclaim.
 *  4. Reclamation happens ONLY at a quiescent point (executor bind / gate
 *     reset), where no session protocol instance exists.
 *  5. Exhaustion FAILS CLOSED: acquire() returns NULL and the caller must
 *     refuse to publish or start a protocol instance.
 *  6. Controlled readers may take an explicit refcount (borrow/release);
 *     a borrowed slot is never reclaimed.
 *
 * The pool is sized to the proven worst case: every bounded B1 retry
 * budget on both sides can force one publication each.
 */
#define POOL_EXEC_IDENTITY_SLOTS                                              \
    ((POOL_SESSION_MAX_TARGET_APPLY_RETRIES + 1u) +                           \
     (POOL_SESSION_MAX_TARGET_VERIFY_RETRIES + 1u) +                          \
     (POOL_SESSION_MAX_RESTORE_APPLY_RETRIES + 1u) +                          \
     (POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES + 1u))

typedef struct {
    char primary_host[POOL_SESSION_HOST_MAX];
    char primary_user[POOL_SESSION_USER_MAX];
    char fallback_host[POOL_SESSION_HOST_MAX];
    char fallback_user[POOL_SESSION_USER_MAX];
} PoolExecIdentityStrings;

/* Acquire an unused slot for writing. NULL when the epoch's bound is
 * exhausted (fail closed). *out_slot receives the slot id on success. */
PoolExecIdentityStrings *pool_session_execution_identity_acquire(uint32_t *out_slot);

/* Publish a written slot as the live identity and drop the previous
 * publication reference. After this the slot's bytes are immutable. */
bool pool_session_execution_identity_publish(uint32_t slot);

/* Borrow/release the live slot (controlled readers). A borrowed slot is
 * never reclaimed. NULL when nothing is published. */
const PoolExecIdentityStrings *pool_session_execution_identity_borrow(uint32_t *out_slot);
void pool_session_execution_identity_release(uint32_t slot);

/*
 * COPY-UNDER-LOCK for asynchronous readers that cannot hold a reference
 * across their whole read (BAP status, the share-submit path). The caller
 * receives a complete, self-consistent, CALLER-OWNED copy and never retains
 * a published pointer. Returns false when nothing is published, in which
 * case the caller keeps using the unchanged boot-time configuration.
 */
typedef struct {
    bool valid;
    char primary_host[POOL_SESSION_HOST_MAX];
    char primary_user[POOL_SESSION_USER_MAX];
    char fallback_host[POOL_SESSION_HOST_MAX];
    char fallback_user[POOL_SESSION_USER_MAX];
} PoolExecIdentityCopy;

bool pool_session_execution_identity_copy(PoolExecIdentityCopy *out);

/*
 * Reclaim slots. Quiescent points ONLY (bind / gate reset). REFUSES to
 * reclaim while any slot still carries a reference — a live protocol
 * instance or an in-progress borrow keeps its slot. Returns the number of
 * slots actually reclaimed; a non-zero refcount is reported through
 * `out_pinned` so the caller can fail closed rather than proceed blind.
 */
uint32_t pool_session_execution_identity_reclaim_all(uint32_t *out_pinned);

/* Diagnostics / tests. */
uint32_t pool_session_execution_identity_slots_used(void);
uint32_t pool_session_execution_identity_refcount(uint32_t slot);
uint32_t pool_session_execution_identity_published_slot(void); /* UINT32_MAX = none */

#endif /* POOL_SESSION_EXECUTION_H_ */
