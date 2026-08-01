#ifndef POOL_SESSION_EXECUTION_CORE_H_
#define POOL_SESSION_EXECUTION_CORE_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session.h"
#include "pool_session_record.h"
#include "pool_session_store.h"
#include "pool_operation_types.h"

/*
 * NeuralAxe timed pool sessions — PURE controlled-execution domain
 * (Phase 2M.1B, Gate B7). Board 601 / BM1370 only.
 *
 * This header is the pure decision domain of the first gate permitted to
 * EXECUTE controlled pool apply, source restore and live protocol
 * verification. Everything here is deterministic and total: no ESP-IDF, no
 * NVS, no FreeRTOS, no networking, no heap, no logging, no global mutable
 * state, no clock read. The executor engine (pool_session_execution.h)
 * sequences real adapters over these rules; this layer only decides.
 *
 * NON-NEGOTIABLE CONTRACTS ENFORCED BY THIS DOMAIN:
 *  - A runtime execution substate is NEVER new durable truth: every substate
 *    maps onto exactly one committed B1 state class, one B5 owner class, one
 *    B5 phase set, one permitted action class, one protocol posture and one
 *    mining posture. Invalid combinations fail closed.
 *  - Exact target/source configuration comparison is FIELD-BY-FIELD over the
 *    bounded B1 identity. A hash is never an identity proof anywhere here.
 *  - The target-mining grant is internal, non-cryptographic, RAM-only and
 *    revocable: absent after reboot, stale after any lease/record/protocol
 *    generation change, and never valid for source and target at once.
 *  - The ASIC delivery gate's INHIBITED/OPEN postures are the only mining
 *    release mechanism; nothing else may release target work.
 *  - Evidence (counters) is bound to one protocol generation via baselines;
 *    counter regression is an anomaly and is never evidence.
 *  - No password exists in any model, event, snapshot or token. The bounded
 *    identity types are the B1 types, which carry no password by design.
 */

#define POOL_EXEC_MODEL_VERSION 1u

/* ------------------------------------------------------------------ */
/* Feature posture (pure; testable for all four flag combinations)     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool executor_permitted; /* a B7 executor may exist at all            */
    bool actions_permitted;  /* controlled apply/verify/restore may run   */
} PoolExecBootAction;

/*
 * The exact rule production wiring and tests both obey:
 *  - sessions=n              -> no executor, no actions (B6 also absent);
 *  - sessions=y, execution=n -> no executor, no actions (B6 hold-only);
 *  - sessions=y, execution=y -> executor permitted, actions permitted;
 *  - sessions=n, execution=y -> IMPOSSIBLE by Kconfig dependency; the pure
 *    rule still fails closed (no executor, no actions).
 */
PoolExecBootAction pool_exec_boot_action_for_features(bool sessions_enabled,
                                                      bool execution_enabled);

/* ------------------------------------------------------------------ */
/* Bounded runtime execution substates                                 */
/* ------------------------------------------------------------------ */

/*
 * Zero is DISABLED so a zeroed executor performs nothing. Substates are RAM
 * bookkeeping over the durable B1 state — a crash reconstructs a
 * conservative posture from the committed record, never from these values.
 */
typedef enum {
    EXEC_STATE_DISABLED = 0,          /* no execution permitted / not bound  */
    EXEC_STATE_IDLE,                  /* bound; no action posture exists     */
    EXEC_STATE_ENTRY_PENDING,         /* action posture seen; validating     */
    EXEC_STATE_TARGET_READBACK,       /* resume: verify effective vs target  */
    EXEC_STATE_TARGET_APPLYING,       /* bounded target config transaction   */
    EXEC_STATE_TARGET_CONFIG_VERIFIED,/* exact readback; reconnect pending   */
    EXEC_STATE_TARGET_CONNECTING,     /* verification-only protocol posture  */
    EXEC_STATE_TARGET_PROTOCOL_VERIFIED, /* evidence complete; grant pending */
    EXEC_STATE_TARGET_MINING,         /* grant active; bounded health watch  */
    EXEC_STATE_RESTORE_PENDING,       /* revoke + quiesce toward restore     */
    EXEC_STATE_SOURCE_APPLYING,       /* bounded source config transaction   */
    EXEC_STATE_SOURCE_CONFIG_VERIFIED,/* exact readback; reconnect pending   */
    EXEC_STATE_SOURCE_CONNECTING,     /* verification-only protocol posture  */
    EXEC_STATE_SOURCE_PROTOCOL_VERIFIED, /* identity+connection proven       */
    EXEC_STATE_SOURCE_MINING_VERIFYING,  /* gate open; bounded resume proof  */
    EXEC_STATE_COMPLETE_HANDOFF,      /* COMPLETE persisted; handoff pending */
    EXEC_STATE_DONE,                  /* execution finished; record retained */
    EXEC_STATE_HANDOFF_FAILED,        /* COMPLETE durable, handoff failed    */
    EXEC_STATE_RESTORE_FAILED_HELD,   /* terminal; obligation retained; held */
    EXEC_STATE_RECOVERY_GUARD,        /* fail-closed guard; operator needed  */
    EXEC_STATE_ERROR,                 /* fail-closed internal failure        */
    POOL_EXEC_STATE__COUNT
} PoolExecState;

/* ------------------------------------------------------------------ */
/* Stable machine reasons (dot-free tokens; never identities or prose) */
/* ------------------------------------------------------------------ */

typedef enum {
    EXEC_REASON_NONE = 0,
    EXEC_REASON_FEATURE_DISABLED,
    EXEC_REASON_NOT_BOUND,
    EXEC_REASON_SYSTEM_NOT_READY,
    EXEC_REASON_BOARD_UNSUPPORTED,
    EXEC_REASON_ASIC_UNSUPPORTED,
    EXEC_REASON_RECORD_INCOMPATIBLE,
    EXEC_REASON_OWNERSHIP_MISMATCH,
    EXEC_REASON_PHASE_MISMATCH,
    EXEC_REASON_STALE_TOKEN,
    EXEC_REASON_PERSIST_FAILED,
    EXEC_REASON_PERSIST_UNCERTAIN,
    EXEC_REASON_PERSIST_READBACK,
    EXEC_REASON_TRANSITION_REJECTED,
    EXEC_REASON_CONFIG_STAGE_REJECTED,
    EXEC_REASON_CONFIG_TIMEOUT,
    EXEC_REASON_CONFIG_PARTIAL,
    EXEC_REASON_CONFIG_UNCERTAIN,
    EXEC_REASON_CONFIG_MISMATCH,
    EXEC_REASON_CONFIG_VERIFIED,
    EXEC_REASON_PROTOCOL_START_FAILED,
    EXEC_REASON_PROTOCOL_STOP_FAILED,
    EXEC_REASON_PROTOCOL_FAILED_EVENT,
    EXEC_REASON_CONNECT_TIMEOUT,
    EXEC_REASON_JOB_TIMEOUT,
    EXEC_REASON_EVIDENCE_STALE,
    EXEC_REASON_IDENTITY_MISMATCH,
    EXEC_REASON_GRANT_ISSUED,
    EXEC_REASON_GRANT_REVOKED,
    EXEC_REASON_HEALTH_FAILED,
    EXEC_REASON_DEADLINE_REACHED,
    EXEC_REASON_RESTORE_STARTED,
    EXEC_REASON_RESTORE_FAILED,
    EXEC_REASON_COMPLETE_VERIFIED,
    EXEC_REASON_HANDOFF_STOP_FAILED,      /* engine would not exit; owned  */
    EXEC_REASON_HANDOFF_START_FAILED,     /* coordinator start refused     */
    EXEC_REASON_ASIC_EVIDENCE_MISSING,    /* no ASIC-side processing fact  */
    EXEC_REASON_TLS_MODE_UNSUPPORTED,     /* mode cannot round-trip        */
    EXEC_REASON_GENERATION_EXHAUSTED,     /* per-boot discriminator budget
                                           * spent: verification unavailable,
                                           * no controlled start is issued  */
    EXEC_REASON_RECOVERY_GUARD,
    EXEC_REASON_INTERNAL,
    POOL_EXEC_REASON__COUNT
} PoolExecReason;

/* ------------------------------------------------------------------ */
/* Substate contract map (pure; invalid combinations fail closed)      */
/* ------------------------------------------------------------------ */

/* Permitted external-action class of a substate (documentation + tests). */
typedef enum {
    EXEC_ACTION_NONE = 0,       /* observe/validate only                    */
    EXEC_ACTION_CONFIG_READ,    /* independent configuration readback       */
    EXEC_ACTION_CONFIG_WRITE,   /* bounded staged configuration transaction */
    EXEC_ACTION_PROTOCOL_START, /* controlled protocol start/reconnect      */
    EXEC_ACTION_PROTOCOL_STOP,  /* controlled protocol stop/quiesce         */
    EXEC_ACTION_MONITOR,        /* bounded evidence/health observation      */
    EXEC_ACTION_HANDOFF,        /* post-COMPLETE production handoff         */
    POOL_EXEC_ACTION__COUNT
} PoolExecActionClass;

/* Required protocol posture of a substate. */
typedef enum {
    EXEC_PROTO_POSTURE_STOPPED = 0,  /* no session protocol task may exist */
    EXEC_PROTO_POSTURE_VERIFICATION, /* controlled task; ASIC gate closed  */
    EXEC_PROTO_POSTURE_CONNECTED,    /* controlled task; gate per grant    */
    EXEC_PROTO_POSTURE_ANY,          /* posture not constrained (idle/done) */
    POOL_EXEC_PROTO_POSTURE__COUNT
} PoolExecProtoPosture;

/* ASIC job-delivery gate postures (see pool_session_execution.h for the
 * production gate). DEFAULT_OPEN is zero DELIBERATELY: target work cannot
 * exist before the executor starts a protocol connection, and the executor
 * sets INHIBITED before its first protocol action — while every posture in
 * which the executor may act has the B6 barrier already withholding the
 * production protocol. A zeroed gate therefore cannot leak target work but
 * never blocks unchanged source mining. */
typedef enum {
    EXEC_GATE_DEFAULT_OPEN = 0,     /* no execution epoch; source unaffected */
    EXEC_GATE_INHIBITED,            /* execution epoch: no ASIC delivery     */
    EXEC_GATE_OPEN_TARGET,          /* valid target grant only               */
    EXEC_GATE_OPEN_SOURCE_RESTORED, /* verified source restore posture       */
    POOL_EXEC_GATE__COUNT
} PoolExecGatePosture;

/* Owner-class requirement of a substate. */
typedef enum {
    EXEC_OWNER_REQ_NONE = 0,      /* no lease requirement (idle/disabled)   */
    EXEC_OWNER_REQ_SESSION,       /* TIMED_SESSION or BOOT_RECOVERY         */
    EXEC_OWNER_REQ_RESTORE_SIDE,  /* session class or SOURCE_RESTORE        */
    EXEC_OWNER_REQ_ANY_OWNED,     /* any owned class (guard escalation)     */
    POOL_EXEC_OWNER_REQ__COUNT
} PoolExecOwnerReq;

/* Total per-substate contract lookups. Out-of-range substates return the
 * fail-closed row (NONE action, STOPPED protocol, INHIBITED gate). */
PoolExecActionClass  pool_exec_state_action(PoolExecState s);
PoolExecProtoPosture pool_exec_state_proto_posture(PoolExecState s);
PoolExecGatePosture  pool_exec_state_gate(PoolExecState s);
PoolExecOwnerReq     pool_exec_state_owner_req(PoolExecState s);

/* True when `b1` is a committed B1 state the substate may legally sit on.
 * Total; unknown substates accept nothing. */
bool pool_exec_state_b1_compatible(PoolExecState s, PoolSessionState b1);

/* True when the owner/phase pair satisfies the substate's ownership row.
 * Total and fail-closed: unknown values are never compatible. */
bool pool_exec_state_ownership_compatible(PoolExecState s,
                                          PoolOperationOwner owner,
                                          PoolOperationLeasePhase phase);

/* True for substates in which the executor OWNS the session flow — the B6
 * task must then defer its plan-persistence/lease-reconcile machinery to
 * the executor (exactly one mutating owner). Total; DISABLED/IDLE/DONE are
 * not owning. */
bool pool_exec_state_owns_flow(PoolExecState s);

/* True for substates that permit a MUTATING external action (config write,
 * protocol start/stop, handoff). Used to assert denied actions mutate
 * nothing. */
bool pool_exec_state_permits_mutation(PoolExecState s);

/* ------------------------------------------------------------------ */
/* Board / ASIC compatibility (fail closed)                            */
/* ------------------------------------------------------------------ */

/* Exact bounded comparison against the committed B1 support constants
 * ("601" / "BM1370"). NULL, empty, oversized or different values are
 * unsupported. Never logs or copies the rejected value. */
bool pool_exec_board_supported(const char *board_version);
bool pool_exec_asic_supported(const char *asic_model);

/* ------------------------------------------------------------------ */
/* Effective-configuration model and exact comparison                  */
/* ------------------------------------------------------------------ */

/*
 * Production TLS modes (mirrors the firmware `tls_mode` enum). The committed
 * B1 identity carries only a BOOLEAN tls flag, so only the two modes that
 * round-trip exactly through that boolean are supported by a timed session:
 *
 *   DISABLED (0) <-> tls=false     BUNDLED (1) <-> tls=true
 *
 * CUSTOM (2) — and any unknown value — CANNOT be represented in the B1
 * identity: restoring it through the boolean would silently downgrade a
 * custom-certificate source to the bundled CA bundle while every boolean
 * comparison still "matched". That is a false exact-restoration claim, so
 * such configurations are REJECTED before any mutation instead.
 */
#define POOL_EXEC_TLS_MODE_DISABLED 0u
#define POOL_EXEC_TLS_MODE_BUNDLED  1u
#define POOL_EXEC_TLS_MODE_CUSTOM   2u

/* True only for a mode that round-trips exactly through the B1 boolean. */
bool pool_exec_tls_mode_representable(uint8_t mode);
/* The exact mode a B1 boolean denotes (false->DISABLED, true->BUNDLED). */
uint8_t pool_exec_tls_mode_for_flag(bool tls);

/*
 * The independently-read-back effective pool configuration. Endpoints reuse
 * the bounded B1 type (NO password field exists in it). `use_fallback` is
 * the persisted role preference; controlled sessions pin it false so the
 * PRIMARY endpoint is the verified identity.
 *
 * The RAW production TLS modes are carried alongside the B1 booleans so the
 * representability gate and the readback comparison can both reason about
 * the exact stored mode — never about the lossy boolean alone.
 */
typedef struct {
    bool         valid;        /* the readback itself succeeded          */
    PoolEndpoint primary;
    PoolEndpoint fallback;
    bool         use_fallback;
    uint8_t      primary_tls_mode;  /* raw stored mode                   */
    uint8_t      fallback_tls_mode; /* raw stored mode                   */
} PoolExecEffectiveConfig;

/*
 * TLS representability verdict for one effective configuration. Total:
 * an invalid readback is never representable (fails closed).
 */
typedef enum {
    EXEC_TLS_OK = 0,              /* every endpoint mode round-trips     */
    EXEC_TLS_UNREADABLE,          /* the readback itself failed          */
    EXEC_TLS_PRIMARY_UNSUPPORTED, /* primary uses custom/unknown mode    */
    EXEC_TLS_FALLBACK_UNSUPPORTED,/* fallback uses custom/unknown mode   */
    POOL_EXEC_TLS_VERDICT__COUNT
} PoolExecTlsVerdict;

/*
 * Reject a configuration whose stored TLS mode cannot be represented in (and
 * therefore restored from) the committed B1 identity. Evaluated BEFORE any
 * mutation intent and before any pool write, so a rejected session never
 * touches NVS, the RAM configuration, the lease or the record.
 */
PoolExecTlsVerdict pool_exec_tls_representable(const PoolExecEffectiveConfig *cfg);

/*
 * The exact TLS mode a bounded B1 identity would WRITE for each endpoint.
 * Always inside {DISABLED, BUNDLED} by construction — a timed session can
 * never write a custom-certificate mode.
 */
void pool_exec_identity_tls_modes(const PoolConfigIdentity *identity,
                                  uint8_t *out_primary, uint8_t *out_fallback);

/* Field-level mismatch mask (bounded diagnostics; never strings). */
#define EXEC_MISMATCH_PRIMARY_HOST     (1u << 0)
#define EXEC_MISMATCH_PRIMARY_PORT     (1u << 1)
#define EXEC_MISMATCH_PRIMARY_USER     (1u << 2)
#define EXEC_MISMATCH_PRIMARY_PROTOCOL (1u << 3)
#define EXEC_MISMATCH_PRIMARY_TLS      (1u << 4)
#define EXEC_MISMATCH_FALLBACK_HOST    (1u << 5)
#define EXEC_MISMATCH_FALLBACK_PORT    (1u << 6)
#define EXEC_MISMATCH_FALLBACK_USER    (1u << 7)
#define EXEC_MISMATCH_FALLBACK_PROTOCOL (1u << 8)
#define EXEC_MISMATCH_FALLBACK_TLS     (1u << 9)
#define EXEC_MISMATCH_ROLE             (1u << 10)
#define EXEC_MISMATCH_READBACK         (1u << 11)
#define EXEC_MISMATCH__ALL             0xFFFu

/*
 * Canonical desired effective configuration of a bounded B1 identity:
 * primary copied exactly, fallback copied exactly when enabled and zeroed
 * otherwise (the B3 canonical-empty rule), use_fallback pinned false.
 * Deterministic; *out fully written.
 */
void pool_exec_desired_from_identity(const PoolConfigIdentity *identity,
                                     PoolExecEffectiveConfig *out);

/*
 * EXACT field-by-field comparison of an effective configuration against the
 * canonical desired form of `identity`. Returns true only on a complete
 * match; *out_mask (optional) receives every mismatching field bit.
 * An invalid effective config matches nothing (EXEC_MISMATCH_READBACK).
 * Never a memcmp over padded structs; never a hash.
 */
bool pool_exec_effective_matches_identity(const PoolExecEffectiveConfig *effective,
                                          const PoolConfigIdentity *identity,
                                          uint32_t *out_mask);

/* Exact effective-vs-effective comparison (same field rules). */
bool pool_exec_effective_equal(const PoolExecEffectiveConfig *a,
                               const PoolExecEffectiveConfig *b,
                               uint32_t *out_mask);

/* Number of fields that must change to move `from` onto the canonical form
 * of `identity` (0 = nothing to write). Total; NULL yields UINT32_MAX. */
uint32_t pool_exec_writes_needed(const PoolExecEffectiveConfig *from,
                                 const PoolConfigIdentity *identity);

/* ------------------------------------------------------------------ */
/* Configuration transaction classification                            */
/* ------------------------------------------------------------------ */

typedef enum {
    EXEC_CONFIG_PENDING = 0,             /* not final; keep polling         */
    EXEC_CONFIG_APPLY_EXACT,             /* effective == desired, writes ran */
    EXEC_CONFIG_APPLY_NO_MUTATION,       /* effective == desired == pre, or
                                          * a definite pre-enqueue rejection
                                          * with the pre state fully intact  */
    EXEC_CONFIG_APPLY_PARTIAL,           /* mix of pre and desired fields    */
    EXEC_CONFIG_APPLY_UNCERTAIN,         /* readback failed / timeout with
                                          * staged writes possibly in flight */
    EXEC_CONFIG_APPLY_READBACK_MISMATCH, /* a field is neither pre nor
                                          * desired — foreign mutation       */
    POOL_EXEC_CONFIG_RESULT__COUNT
} PoolExecConfigApplyResult;

/*
 * Pure classification of one bounded apply transaction poll.
 *  pre      : effective configuration captured BEFORE staging;
 *  identity : the desired bounded identity;
 *  now      : the current independent readback;
 *  stage_rejected : the staging call reported a definite pre-enqueue
 *                   rejection (nothing was enqueued);
 *  timed_out      : the bounded transaction window has expired.
 * Deterministic and total. Success is NEVER inferred from setter returns —
 * only from the independent readback. A timeout with writes possibly in
 * flight is UNCERTAIN even when nothing has landed yet (an asynchronous
 * writer may still mutate later).
 */
PoolExecConfigApplyResult pool_exec_classify_apply(
    const PoolExecEffectiveConfig *pre, const PoolConfigIdentity *identity,
    const PoolExecEffectiveConfig *now, bool stage_rejected, bool timed_out);

/* True exactly for the final (non-PENDING) classifications. */
bool pool_exec_apply_result_final(PoolExecConfigApplyResult r);
/* True for final classifications that must be treated as UNCERTAIN MUTATION
 * (PARTIAL / UNCERTAIN / READBACK_MISMATCH). */
bool pool_exec_apply_result_uncertain_mutation(PoolExecConfigApplyResult r);

/* ------------------------------------------------------------------ */
/* Protocol evidence model (generation-bound; stale-proof)             */
/* ------------------------------------------------------------------ */

/* Bounded protocol-event bits delivered by the protocol adapter poll.
 * Payload-free by design: no strings, no pool error text. */
#define EXEC_PEVT_CONNECTION_FAILED (1u << 0) /* retry budget exhausted     */
#define EXEC_PEVT_SETUP_SUCCESS     (1u << 1) /* protocol setup accepted    */
#define EXEC_PEVT_TASK_EXITED       (1u << 2) /* controlled task exited     */
#define EXEC_PEVT_SHUTDOWN_FAILED   (1u << 3) /* stop handshake timed out   */
#define EXEC_PEVT__ALL_VALID        0xFu

/* Drop unknown bits (they never reach any handler). */
uint32_t pool_exec_protocol_events_sanitize(uint32_t raw);

/* Bounded monotonic counters sampled from the existing audited signals. */
typedef struct {
    uint64_t work_received;   /* parse-accepted jobs from the live pool   */
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint32_t queue_depth;     /* pending jobs awaiting delivery           */
    uint64_t jobs_forwarded;  /* jobs released through the delivery gate  */
    /*
     * ASIC-SIDE processing facts, counted ONLY inside the current work
     * generation (the gate resets them whenever the generation changes, so
     * evidence from an earlier protocol/configuration/job generation can
     * never be observed here).
     *
     * asic_job_results — the BM1370 returned a nonce for a job that was
     * still registered in the active-job table: deterministic proof the
     * chip ACCEPTED AND PROCESSED work this firmware delivered. This is the
     * only fact that may satisfy a mining-verified decision.
     *
     * asic_register_reads — the chip answered a register read: liveness
     * only, NOT proof that delivered work was processed. Diagnostic.
     */
    uint64_t asic_job_results;
    uint64_t asic_register_reads;
} PoolExecProtocolCounters;

/*
 * Bounded evidence verdict for one generation-bound observation window.
 * Evidence is computed ONLY from deltas against the baseline captured when
 * the CURRENT protocol generation started; a counter moving backwards is an
 * anomaly (stale/foreign evidence) and is never evidence of anything.
 */
typedef struct {
    bool     anomaly;            /* counter regression — reject everything */
    bool     connection_evidence;/* setup success or any job delta         */
    bool     job_evidence;       /* >= 1 parse-accepted job this window    */
    bool     forward_evidence;   /* >= 1 job released through the gate     */
    /*
     * The ASIC-side processing fact: the chip returned a result for work
     * this generation delivered. Software-side delivery (a dequeue, an
     * ASIC_send_work call or a gate counter tick) NEVER sets this.
     */
    bool     asic_processing_evidence;
    uint64_t job_delta;
    uint64_t forward_delta;
    uint64_t asic_result_delta;
} PoolExecEvidence;

/*
 * LOCAL WORK-PROOF DIFFICULTY — deliberately INDEPENDENT of pool vardiff.
 *
 * The BM1370 is configured with a FIXED result difficulty taken from the
 * compiled per-ASIC table (`DEVICE_CONFIG.family.asic.difficulty` = 256 for
 * the BM1370, written once during BM1370_init). That is the difficulty the
 * chip itself filters on before returning a nonce; it has nothing to do
 * with the pool's share difficulty.
 *
 * Using the POOL difficulty as a liveness threshold would be wrong twice
 * over: local hardware verification would end up waiting for a share-level
 * nonce, and a high vardiff could stall a perfectly healthy source
 * restoration. The binding therefore uses the ASIC's own configured
 * difficulty, clamped into this compiled band so neither a corrupted device
 * config nor any runtime value can weaken it. It is NOT settable at
 * runtime — the source is a `static const` table entry.
 *
 * WHAT THIS PROOF ESTABLISHES — AND WHAT IT CANNOT. The guarantee is
 * split into three separately-stated parts; conflating them was a real
 * defect this design corrects:
 *
 *  1. TEMPORAL FRESHNESS is established by the generation-unique work
 *     contract below (a protocol-valid generation discriminator embedded
 *     in the hashed header, proven distinct by EXACT canonical header
 *     comparison at delivery). It is NOT established by proof of work:
 *     a pool may resend the same template after a reconnect, and the
 *     firmware's extranonce2 counter restarts at 0 for every work item
 *     (create_jobs_task), so a NEW generation can rebuild a byte-identical
 *     80-byte header. A delayed result for the old, identical header is
 *     then GENUINELY VALID for the current header — no probability bound
 *     applies. Such work is therefore EXCLUDED from evidence outright
 *     (deterministic rejection), not "bounded".
 *
 *  2. NONCE/HEADER BINDING is probabilistic local proof of work at the
 *     fixed difficulty above: a nonce produced for a DIFFERENT header
 *     clears difficulty D against the current header with probability
 *     1/(D * 2^32). At D = 256 that is 2^-40 ~= 9.1e-13 per stray result.
 *
 *  3. The TWO-PROOF false-positive bound (see below) applies ONLY after
 *     part 1 has established that the counted headers are unique across
 *     generations. No global bound is claimed without that precondition.
 */
#define POOL_EXEC_WORK_PROOF_MIN_DIFF 256.0
#define POOL_EXEC_WORK_PROOF_MAX_DIFF 65536.0

/*
 * How many DISTINCT delivered work items must be proven inside ONE work
 * generation before local ASIC liveness counts as established.
 *
 * At the supported Gamma 601 / BM1370 operating point (2040 small cores *
 * 525 MHz ~= 1.07 TH/s from the compiled device table) a difficulty-256
 * result is expected about every 256 * 2^32 / 1.07e12 ~= 1.03 s. Requiring
 * two distinct proofs is therefore expected to take ~2 s — negligible
 * inside the 120 s target and 180 s source windows — and it never depends
 * on a pool share or on pool difficulty.
 *
 * Two independent proofs square the false-positive bound ONLY UNDER THE
 * HEADER-UNIQUENESS PRECONDITION (part 1 above): once every counted record
 * is proven generation-unique, a spurious "verified" needs two separate
 * stray nonces to each clear difficulty 256 against a genuinely DIFFERENT
 * header (~9.1e-13 each), each additionally attributed to a DISTINCT
 * unconsumed record of the current generation. Without that precondition
 * the results are not counted at all, so no bound is needed or claimed —
 * exclusion, not estimation.
 */
#define POOL_EXEC_REQUIRED_WORK_PROOFS 2u

/* ------------------------------------------------------------------ */
/* Generation-unique work contract (temporal freshness authority)      */
/* ------------------------------------------------------------------ */

/*
 * CANONICAL WORK FACTS — the exact fields of the delivered work item that
 * the BM1370 hashes into the 80-byte block header (everything except the
 * chip-chosen rolled version bits and the nonce), byte-for-byte as stored
 * in the firmware's bm_job: base version, prev_block_hash and merkle_root
 * in bm_job word order, ntime and nbits. `test_nonce_value()` rebuilds the
 * header it validates from EXACTLY these fields, so two work items with
 * equal facts span the same header space and a result for one is
 * indistinguishable from a result for the other. Version rolling cannot
 * separate them: the chip is free to choose the same rolled bits in both
 * generations, so the roll mask is deliberately NOT part of the facts.
 */
typedef struct {
    uint32_t version;             /* base header version (pre-roll)       */
    uint32_t ntime;
    uint32_t nbits;               /* bm_job "target" field                */
    uint8_t  prev_block_hash[32]; /* bm_job word order                    */
    uint8_t  merkle_root[32];     /* bm_job word order                    */
} PoolExecWorkFacts;

/*
 * EXACT canonical comparison — the ONLY authority on header uniqueness.
 * Field-by-field (never a whole-struct memcmp, so padding can never lie),
 * total, NULL-safe. A short hash or fingerprint is NEVER used as proof of
 * inequality anywhere in this contract; bounded storage permits keeping
 * the full facts for every ASIC job slot, so the comparison is exact.
 */
bool pool_exec_work_facts_equal(const PoolExecWorkFacts *a,
                                const PoolExecWorkFacts *b);

/*
 * THE GENERATION DISCRIMINATOR. ASIC results may satisfy B7 verification
 * ONLY for work whose exact block header contains a generation-specific
 * discriminator. The one protocol-valid mechanism this firmware can offer
 * on the audited paths is the EXTRANONCE2 GENERATION DOMAIN:
 *
 *  - Stratum V1: extranonce2 is miner-owned rolling space by protocol
 *    definition. The firmware serializes its uint64 counter little-endian
 *    into the first min(len, 8) bytes (extranonce_2_generate), so counter
 *    bits 24..31 land in extranonce2 byte 3 — inside the coinbase, hence
 *    the merkle root, hence the hashed header — whenever the pool grants
 *    a width of at least POOL_EXEC_DISCRIMINATOR_MIN_EN2_BYTES.
 *  - Stratum V2 EXTENDED channels: the miner-rollable extranonce portion
 *    is encoded big-endian into the trailing bytes (create_jobs_task), so
 *    counter bits 24..31 land at byte [len-4] — same width rule.
 *  - Stratum V2 STANDARD channels: the pool computes the merkle root;
 *    the miner owns NO coinbase bytes. No discriminator is possible and
 *    the capability is NONE — the evidence path FAILS CLOSED.
 *
 * Rejected alternatives, for the record: version-bit reservation would
 *    require restricting the chip's hardware rolling mask, whose in-flight
 *    behaviour is undocumented in this repository (the audited barrier
 *    finding), and ntime manipulation would trade on untrusted pool time
 *    and risk share validity. Neither is used.
 *
 * The tag occupies counter bits 24..31 and leaves the low 24 bits as
 * per-generation rolling space. It is NEVER derived by modulo arithmetic:
 * tags are allocated MONOTONICALLY, ONCE PER CONTROLLED PROTOCOL START,
 * from a per-boot budget of POOL_EXEC_GENERATION_TAG_LIMIT values
 * (0x81..0xFF; the top bit marks the active domain, 0x80 itself is never
 * issued, and byte value 0 remains the untagged stock encoding). A tag
 * value is NEVER REUSED within one controlled execution lifetime (one
 * runtime boot): when the budget is spent the allocator FAILS CLOSED —
 * the executor refuses to issue any further controlled verification
 * start (EXEC_REASON_GENERATION_EXHAUSTED), target mining cannot be
 * granted, source COMPLETE stays impossible and restore_required is
 * retained by the ordinary bounded failure paths. Wrap is not "handled";
 * it is made unreachable. The exact header comparison above REMAINS the
 * uniqueness authority for every delivered item regardless.
 *
 * The ROLLING DOMAIN is equally guarded: a counter that no longer fits
 * the low 24 bits would silently overflow INTO the tag byte, so the embed
 * query REJECTS any counter above POOL_EXEC_GENERATION_COUNTER_MASK
 * (the work item goes out untagged and can never be verification
 * evidence — fail closed, never silent reuse). Reaching that bound would
 * take 2^24 jobs under ONE provider template in ONE generation: at the
 * supported Gamma 601 job cadence (~500 ms/job, ASIC_get_asic_job_
 * frequency_ms) that is ~97 days against bounded verification windows of
 * 120/180 s and bounded session durations — but the check does not rely
 * on that arithmetic; it fails closed regardless.
 */
typedef enum {
    POOL_EXEC_DISCRIMINATOR_NONE = 0,   /* no generation domain: fail closed */
    POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,/* coinbase extranonce2 tag domain   */
    POOL_EXEC_DISCRIMINATOR__COUNT
} PoolExecHeaderDiscriminator;

#define POOL_EXEC_DISCRIMINATOR_MIN_EN2_BYTES 4u
#define POOL_EXEC_GENERATION_TAG_SHIFT        24u
#define POOL_EXEC_GENERATION_TAG_MASK         0xFFu
#define POOL_EXEC_GENERATION_COUNTER_MASK     0x00FFFFFFull
/* Per-boot budget of controlled-generation discriminators (0x81..0xFF). */
#define POOL_EXEC_GENERATION_TAG_LIMIT        127u

/*
 * Capability decision for the ACTIVE pool protocol. Total and fail-closed:
 * anything but a recognised (protocol, channel, width) combination is NONE.
 */
PoolExecHeaderDiscriminator pool_exec_discriminator_capability(
    bool protocol_v2, bool sv2_extended_channel, uint32_t extranonce2_len);

/*
 * The tag for allocation index 1..POOL_EXEC_GENERATION_TAG_LIMIT
 * (0x80 + index, i.e. 0x81..0xFF). Returns 0 — the fail-closed "no tag"
 * value — for index 0 and for ANY index beyond the limit: this function
 * cannot express a wrapped or reused domain. Total.
 */
uint32_t pool_exec_generation_tag(uint32_t allocation_index);

/* Embed the tag into an extranonce2 counter: bits 24..31 carry the tag,
 * bits 0..23 keep rolling, higher bits are cleared (they are zero-padding
 * in both audited encodings). Deterministic and total. */
uint64_t pool_exec_apply_generation_tag(uint64_t counter, uint32_t tag);

/*
 * The complete mining-verified predicate, in one auditable place: the pool
 * is serving work for this generation, work reached the ASIC through the
 * delivery gate, AND the ASIC returned POOL_EXEC_REQUIRED_WORK_PROOFS
 * distinct exact-work proofs for it. Used for the initial target-health
 * window and as the precondition of the source COMPLETE transition. Total;
 * a NULL or anomalous verdict is never mining-verified.
 */
bool pool_exec_evidence_mining_verified(const PoolExecEvidence *ev);

/*
 * Pure evidence evaluation. `events` must be pre-sanitized bits observed in
 * the CURRENT generation; the caller guarantees baseline/now are sampled
 * from the same generation (the engine re-baselines on every controlled
 * start and discards events polled before it). Deterministic and total.
 */
void pool_exec_evaluate_evidence(const PoolExecProtocolCounters *baseline,
                                 const PoolExecProtocolCounters *now,
                                 uint32_t events, PoolExecEvidence *out);

/* ------------------------------------------------------------------ */
/* Target-mining grant (internal, revocable, RAM-only)                 */
/* ------------------------------------------------------------------ */

/*
 * The first legitimate target-mining authorization. Internal and
 * non-cryptographic: a consistency token, not a capability. RAM-only by
 * construction (never persisted), so it is absent after any reboot; it is
 * stale after any lease rotation, any committed-record generation change
 * and any protocol generation change; it is revocable at any time and a
 * revocation is permanent for that issuance.
 */
typedef struct {
    bool     valid;
    bool     revoked;
    uint32_t session_id;
    uint32_t lease_generation;    /* B5 token generation at issue          */
    uint32_t record_generation;   /* B3 committed generation at issue      */
    uint32_t protocol_generation; /* controlled connection epoch at issue  */
    uint32_t issue_sequence;      /* runtime-monotonic issuance number     */
} PoolExecMiningGrant;

/* Zero (invalid) grant. */
void pool_exec_grant_init(PoolExecMiningGrant *g);

/*
 * Issue a grant binding the CURRENT session, lease, record and protocol
 * generations. Returns false (and an invalid grant) for zero/invalid
 * bindings — a grant can never be issued blind.
 */
bool pool_exec_grant_issue(PoolExecMiningGrant *g, uint32_t session_id,
                           uint32_t lease_generation, uint32_t record_generation,
                           uint32_t protocol_generation, uint32_t issue_sequence);

/* Permanently revoke (idempotent). */
void pool_exec_grant_revoke(PoolExecMiningGrant *g);

/*
 * TOTAL validity check against the CURRENT bindings. False when the grant
 * is absent, revoked, or ANY binding differs (session, lease generation,
 * record generation, protocol generation). This is the only rule that may
 * open the target side of the delivery gate.
 */
bool pool_exec_grant_valid(const PoolExecMiningGrant *g, uint32_t session_id,
                           uint32_t lease_generation, uint32_t record_generation,
                           uint32_t protocol_generation);

/* The pure delivery-gate rule: does `gate` release ASIC work right now?
 * Total; out-of-range postures never release. OPEN_TARGET additionally
 * requires `target_grant_valid` (the engine passes the evaluated grant). */
bool pool_exec_gate_allows(PoolExecGatePosture gate, bool target_grant_valid);

/* ------------------------------------------------------------------ */
/* Bounded action policy (monotonic seconds; no wall clock, no ntime)  */
/* ------------------------------------------------------------------ */

/* Committed defaults (seconds). Bounded and clamped by validate(). */
#define EXEC_POLICY_CONFIG_TIMEOUT_DEFAULT_S    15u
#define EXEC_POLICY_STOP_TIMEOUT_DEFAULT_S      15u
#define EXEC_POLICY_CONNECT_TIMEOUT_DEFAULT_S   60u
#define EXEC_POLICY_JOB_TIMEOUT_DEFAULT_S       90u
/*
 * The health windows must comfortably contain the ASIC's nonce-return
 * interval: they wait for a HARDWARE result for delivered work, not for a
 * hashrate figure. They are TIME bounds, never performance thresholds, and
 * expiring is always fail-safe (target -> restore; source -> RESTORE_FAILED
 * with the obligation retained), never a false "verified".
 */
#define EXEC_POLICY_TARGET_HEALTH_DEFAULT_S    120u
#define EXEC_POLICY_SOURCE_HEALTH_DEFAULT_S    180u
#define EXEC_POLICY_TIMEOUT_MIN_S               1u
#define EXEC_POLICY_TIMEOUT_MAX_S               3600u
#define EXEC_POLICY_STOP_RETRY_MAX              2u

typedef struct {
    uint32_t config_timeout_s;   /* write+independent-readback window     */
    uint32_t stop_timeout_s;     /* controlled protocol stop handshake    */
    uint32_t connect_timeout_s;  /* connection-evidence window            */
    uint32_t job_timeout_s;      /* valid-job-evidence window (>= connect) */
    uint32_t target_health_s;    /* initial target health window          */
    uint32_t source_health_s;    /* source mining-resumed window          */
    uint8_t  stop_retry_max;     /* bounded stop retries before guard     */
} PoolExecPolicy;

/* Fill the committed defaults. */
void pool_exec_policy_defaults(PoolExecPolicy *p);

/* Clamp every field into its committed band (never rejects — a malformed
 * policy becomes the nearest bounded one; determinism over failure). */
void pool_exec_policy_clamp(PoolExecPolicy *p);

/* Monotonic deadline arithmetic (saturating; never wraps). */
uint64_t pool_exec_deadline_us(uint64_t now_us, uint32_t seconds);
bool     pool_exec_deadline_reached(uint64_t now_us, uint64_t deadline_us);

/* ------------------------------------------------------------------ */
/* Record staging and exact transition readback                        */
/* ------------------------------------------------------------------ */

/*
 * Stage a persistable record for one B1 transition result: converts the
 * session field-by-field (never a memcpy format), then restores the
 * committed record's time facts (verified start, deadline, trusted floor)
 * and bounded recovery counters, which are NOT part of the B1 session
 * model. Validates the staged record before returning RECORD_OK.
 */
PoolRecordCodecError pool_exec_stage_record(const PoolSession *session,
                                            const PoolSessionRecord *committed,
                                            PoolSessionRecord *out);

/*
 * EXACT field-by-field record equality over every persisted field
 * (identities via the B1 operational comparison, chain, profile, verify
 * structs, retries, counters, flags, epochs, failure code). `generation`
 * participates only when ignore_generation is false. Never a memcmp over
 * the padded struct; never a hash. Total: NULLs are never equal.
 */
bool pool_exec_record_equal_exact(const PoolSessionRecord *a,
                                  const PoolSessionRecord *b,
                                  bool ignore_generation);

/*
 * Independent read-back proof of one committed B1-transition record:
 *  - the reload succeeded (STORE_OK);
 *  - the reloaded record equals the staged record exactly (generation
 *    ignored in the field comparison);
 *  - the reloaded generation is STRICTLY newer than the pre-commit one;
 *  - the restore obligation is monotone: it may read false only when the
 *    staged state is COMPLETE (or was already discharged before staging).
 * Returns EXEC_REASON_NONE only when every check passes.
 */
PoolExecReason pool_exec_verify_transition_readback(const PoolSessionRecord *staged,
                                                    const PoolSessionRecord *reloaded,
                                                    uint32_t pre_commit_generation,
                                                    bool pre_commit_restore_required,
                                                    PoolStoreResult reload_result);

/* ------------------------------------------------------------------ */
/* Sanitized execution snapshot (string-free by construction)          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t                  model_version;
    PoolExecState             state;
    PoolExecReason            reason;
    PoolExecConfigApplyResult last_apply_result;
    PoolOperationOwner        owner;
    PoolOperationLeasePhase   phase;
    PoolExecGatePosture       gate;

    bool executor_bound;
    bool system_ready;
    bool restore_required;
    bool config_verified;
    bool protocol_verified;
    bool job_verified;
    bool mining_grant_active;
    bool protocol_generation_present;
    /* An ASIC-side processing result was observed in the CURRENT work
     * generation. Never set by software-side delivery. */
    bool asic_evidence_seen;

    uint8_t  target_role;        /* 0 none, 1 target, 2 source            */
    uint8_t  stop_attempts;
    uint32_t protocol_generation;
    uint32_t work_generation;    /* gate-owned delivery/hardware epoch    */
    uint32_t commit_count;       /* B7 transition commits this boot       */
    uint32_t event_sequence;     /* bounded observation sequence          */
} PoolExecutionSnapshot;

/* Zero to the fail-closed DISABLED view. */
void pool_exec_snapshot_init(PoolExecutionSnapshot *snap);

/* Structural consistency: version, enums in range, gate consistent with
 * the substate contract, grant flag consistent with the gate. */
bool pool_exec_snapshot_valid(const PoolExecutionSnapshot *snap);

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_exec_state_str(PoolExecState s);
const char *pool_exec_reason_str(PoolExecReason r);
const char *pool_exec_gate_str(PoolExecGatePosture g);
const char *pool_exec_apply_result_str(PoolExecConfigApplyResult r);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(POOL_EXEC_STATE__COUNT == 21,
               "execution substate count changed — review map/tokens/tests");
_Static_assert(POOL_EXEC_REASON__COUNT == 41,
               "execution reason count changed — review tokens/tests");
_Static_assert(POOL_EXEC_TLS_VERDICT__COUNT == 4,
               "TLS verdict count changed — review the representability gate");
_Static_assert(EXEC_STATE_DISABLED == 0,
               "DISABLED must be zero so a zeroed executor performs nothing");
_Static_assert(EXEC_GATE_DEFAULT_OPEN == 0,
               "gate zero-value contract documented above must hold");
_Static_assert(POOL_EXEC_GATE__COUNT == 4 && POOL_EXEC_CONFIG_RESULT__COUNT == 6,
               "gate/classification enums changed — review rules/tests");
_Static_assert(EXEC_PEVT__ALL_VALID == 0xFu,
               "protocol event mask changed — review sanitization/tests");
/* The executor consumes these committed domains; growth must be reviewed. */
_Static_assert(POOL_STATE__COUNT == 17 && POOL_EVT__COUNT == 28,
               "B1 model changed — review the executor mapping");
_Static_assert(OP_OWNER__COUNT == 9 && OP_PHASE__COUNT == 10,
               "B5 ownership model changed — review the executor mapping");
_Static_assert(POOL_STORE_RESULT__COUNT == 16,
               "B3 store results changed — review the executor mapping");

#endif /* POOL_SESSION_EXECUTION_CORE_H_ */
