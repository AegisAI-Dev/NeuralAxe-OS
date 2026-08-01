#ifndef POOL_SESSION_COMMAND_H_
#define POOL_SESSION_COMMAND_H_

#include "pool_session_api_types.h"
#include "pool_session_api_status.h"
#include "pool_session_api_heartbeat.h"
#include "pool_session_runtime.h"
#include "pool_session_execution.h"

/*
 * NeuralAxe timed pool sessions — bounded command transport and the
 * owner-task command processor (Phase 2M.1B, Gate B8).
 *
 * TRANSPORT. A statically allocated, fixed-depth mailbox. Multiple HTTP
 * producers may submit concurrently; there is EXACTLY ONE consumer — the
 * single committed Gate B6 runtime owner task. No heap is allocated per
 * request, no command carries a pointer, and a consumed slot is zeroed.
 * A full queue fails the submission WITHOUT mutating anything.
 *
 * PROCESSOR. The processor is NOT a task and never creates one: the B6
 * owner task drives it through bounded pool_api_processor_step() calls, so
 * exactly one mutating session owner continues to exist. It uses the SAME
 * B3 store instance, the SAME B5 coordinator and the SAME lease token the
 * B6 runtime reconstructed — it never opens a second store and never
 * acquires a second competing lease.
 *
 * WHAT THE PROCESSOR NEVER DOES:
 *  - it never applies pool configuration, starts/stops/reconnects Stratum,
 *    grants ASIC mining or restarts the device (Gate B7 is the only layer
 *    permitted to execute those, and only through the runtime ownership it
 *    already holds);
 *  - it never accepts, stores, transports or logs a password — no password
 *    field exists in any model reachable from here;
 *  - it never accepts a client-supplied source identity, session id or
 *    lease token.
 *
 * OWNERSHIP AND ORPHANED LEASES. Every fallible validation runs BEFORE the
 * B5 lease is acquired, so a rejected create or acknowledgement never holds
 * a lease at all. After acquisition only the durable sequence remains
 * (commit -> independent reload -> exact verification -> B5 proof), and a
 * DEFINITE no-mutation failure RELEASES the lease through the audited B5
 * abort path (pool_operation_abort_reservation /
 * pool_operation_abort_acknowledge) after independently proving the store
 * is exactly as it was. Ownership returns to FREE with the generation
 * rotated and every prior token stale — with NO client retry involved.
 * UNCERTAINTY never releases: it enters the B5 recovery guard.
 *
 * SAME-BOOT EXECUTION. A successful create does not stop at the durable
 * snapshot: the owner task hands the session directly to the committed
 * Gate B7 executor in the same call, which commits the pre-mutation
 * APPLYING_TARGET boundary (establishing the restore obligation durably)
 * before any pool key can be written. If adoption is refused the session is
 * safely CANCELLED through the pre-mutation B1 path — no pool touched, no
 * restoration owed — so a completed command NEVER leaves a durable
 * TARGET_SNAPSHOT_COMMITTED record behind.
 */

/* ------------------------------------------------------------------ */
/* Bounded static command mailbox                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     initialized;
    uint8_t  head;  /* next slot to consume */
    uint8_t  tail;  /* next slot to fill    */
    uint8_t  count;
    uint32_t next_sequence;    /* monotonic per boot; 0 is never issued */
    uint32_t submitted;        /* saturating audit counters            */
    uint32_t consumed;
    uint32_t rejected_full;
    uint32_t rejected_invalid;
    PoolApiCommand slots[POOL_API_COMMAND_QUEUE_DEPTH];
} PoolApiCommandQueue;

/* Zero to the fail-closed empty posture. */
void pool_api_queue_init(PoolApiCommandQueue *q);

/*
 * Submit one bounded command. Thread-safe for multiple producers (a static
 * bounded critical section). Returns API_SUBMIT_ACCEPTED with the assigned
 * monotonic sequence in *out_sequence, API_SUBMIT_QUEUE_FULL when the
 * bounded depth is exhausted (nothing is mutated) or API_SUBMIT_INVALID for
 * a malformed command. Never allocates.
 */
PoolApiSubmitStatus pool_api_queue_submit(PoolApiCommandQueue *q,
                                          const PoolApiCommand *cmd,
                                          uint32_t *out_sequence);

/*
 * Consume one command (SINGLE consumer only). Returns false when empty. The
 * source slot is fully ZEROED before returning, so no command bytes remain
 * in the mailbox after consumption.
 */
bool pool_api_queue_take(PoolApiCommandQueue *q, PoolApiCommand *out);

uint8_t pool_api_queue_depth(const PoolApiCommandQueue *q);

/* True when the raw slot storage at `index` is entirely zero (test seam
 * proving a consumed command left no residue). */
bool pool_api_queue_slot_is_zero(const PoolApiCommandQueue *q, uint8_t index);

/* Total bounded command validation (kind, actor and, for CREATE, every
 * bounded field). Fail-closed for NULL and unknown values. */
bool pool_api_command_valid(const PoolApiCommand *cmd);

/* ------------------------------------------------------------------ */
/* Injected source-capture adapter (bounded, password-free)            */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Bounded device identity for the fail-closed board/ASIC check.
     * false when unavailable (fails closed). */
    bool (*device_identity)(void *ctx, char *board, size_t board_cap,
                            char *asic, size_t asic_cap);

    /*
     * INDEPENDENT effective-configuration readback — in production the SAME
     * audited flash-level adapter the Gate B7 executor uses, never the
     * writer's RAM cache and never a second unaudited reader. *out is fully
     * written; out->valid=false when the readback itself failed.
     */
    void (*read_effective)(void *ctx, PoolExecEffectiveConfig *out);

    /*
     * True when the CURRENTLY stored pool password is readable and will be
     * retained unchanged by the Keep-current policy. It returns a BOOLEAN
     * ONLY: no password byte is ever read into, copied through or returned
     * by this call, and no caller may ask for one.
     */
    bool (*source_password_retained)(void *ctx);
} PoolApiSourceOps;

/* ------------------------------------------------------------------ */
/* Owner-task command processor (caller-owned; single-threaded)        */
/* ------------------------------------------------------------------ */

typedef struct {
    bool initialized;
    bool bound;

    PoolSessionRuntime     *rt;      /* the ONE bound B6 runtime      */
    const PoolApiSourceOps *src_ops;
    void                   *src_ctx;
    PoolSessionExecutor    *ex;      /* optional; NULL degrades safely */

    PoolApiCommandQueue queue;

    /*
     * BOUNDED owner-task critical-workflow flag. It is set for exactly the
     * duration of ONE command transaction and cleared on every return path,
     * so it can never remain set while waiting for another HTTP request or
     * for future code. While it is set the B6 owner task withholds its
     * BOOT-time B4 re-planning from the in-flight transaction; the moment
     * the transaction ends the executor's own ownership is authoritative
     * (pool_api_processor_step returns the union of the two).
     */
    bool api_owns_flow;

    /* Published sanitized status (copied out under the module lock). */
    PoolApiStatus status;
    uint32_t      status_sequence;

    /* Last command bookkeeping (sanitized; no identity). */
    PoolApiCommandKind   last_kind;
    PoolApiCommandResult last_result;
    uint32_t             last_client_request_id;
    uint32_t             processed_count;
    uint8_t              last_conflict_code; /* PoolOperationHttpCode value */

    PoolApiHeartbeatState hb;

    /* Bounded work buffers — kept OFF the owner-task stack. */
    PoolSessionRequest      req;
    PoolSession             session;
    PoolSessionRecord       staged;
    PoolSessionRecord       reloaded;
    PoolExecEffectiveConfig effective;
    uint32_t                create_sequence;
    /* The session id bound to an OUTSTANDING B5 reservation. It is derived
     * once and then FIXED, so the pre-flight build, the reservation, the
     * under-lease build and the persistence proof can never disagree. */
    uint32_t                pending_session_id;
} PoolSessionApiProcessor;

/* Zero to the fail-closed unbound posture. */
void pool_api_processor_init(PoolSessionApiProcessor *p);

/*
 * Bind to the booted B6 runtime and the source adapter. Validates every
 * vtable entry; a NULL anywhere refuses the bind (fail closed). `ex` may be
 * NULL (status then reports no execution posture and the heartbeat is never
 * applicable). Returns API_SUBMIT_ACCEPTED on success.
 */
PoolApiSubmitStatus pool_api_processor_bind(PoolSessionApiProcessor *p,
                                            PoolSessionRuntime *rt,
                                            const PoolApiSourceOps *ops, void *ctx,
                                            PoolSessionExecutor *ex);

/* Unbind and zero. Never releases ownership and never clears a record. */
void pool_api_processor_deinit(PoolSessionApiProcessor *p);

/* Submit a command (HTTP producers). Publishes the pending-command view. */
PoolApiSubmitStatus pool_api_processor_submit(PoolSessionApiProcessor *p,
                                              const PoolApiCommand *cmd,
                                              uint32_t *out_sequence);

/*
 * ONE bounded owner-task step: drain at most one command, evaluate the
 * bounded heartbeat and publish the sanitized status. Returns the current
 * `api_owns_flow` value so the B6 task can defer its BOOT-time machinery.
 * Total and fail-closed for a NULL/unbound processor.
 */
bool pool_api_processor_step(PoolSessionApiProcessor *p);

/* Copy the published sanitized status (never a torn read). */
void pool_api_processor_status(const PoolSessionApiProcessor *p, PoolApiStatus *out);

/* Saturating count of commands the owner task has processed this boot. */
uint32_t pool_api_processor_processed(const PoolSessionApiProcessor *p);

/*
 * Deterministic non-zero session-id derivation. It uses only the committed
 * record generation the create was planned FROM and a bounded per-boot
 * create sequence — never a wall clock, a raw reset value or an identity.
 */
uint32_t pool_api_derive_session_id(uint32_t pre_generation, uint32_t create_sequence);

/* ------------------------------------------------------------------ */
/* Production singleton (feature-gated)                                */
/* ------------------------------------------------------------------ */

/* The compile-time value of CONFIG_NX_TIMED_SESSIONS_API. */
bool pool_api_feature_enabled(void);

/*
 * The single production processor, or NULL when the API feature is
 * disabled. With the feature disabled NO processor storage exists, no
 * command queue exists and no route can reach one.
 */
PoolSessionApiProcessor *pool_api_default_processor(void);

/* ------------------------------------------------------------------ */
/* Compile-time footprint guards                                       */
/* ------------------------------------------------------------------ */

/*
 * The processor holds every large working struct so nothing lands on the
 * owner-task stack. It is a BOUNDED static (measured 5,720 B on the
 * xtensa-esp32s3 toolchain); these guards make an accidental growth a
 * build failure rather than a stack overflow or a silent RAM regression.
 */
_Static_assert(sizeof(PoolSessionApiProcessor) <= 8192,
               "the API processor must stay a bounded static, never a stack object");
_Static_assert(sizeof(PoolApiCommand) <= 512,
               "a command must stay small: the mailbox stores them by value");
_Static_assert(sizeof(PoolApiCommandQueue) <=
                   (size_t)POOL_API_COMMAND_QUEUE_DEPTH * 512u + 64u,
               "the mailbox must stay a bounded fixed-depth static");

#endif /* POOL_SESSION_COMMAND_H_ */
