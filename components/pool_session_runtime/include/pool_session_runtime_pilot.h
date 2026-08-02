#ifndef POOL_SESSION_RUNTIME_PILOT_H_
#define POOL_SESSION_RUNTIME_PILOT_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session_runtime_core.h"
#include "pool_session_store.h"
#include "pool_operation_types.h"
#include "pool_time_source.h"

/*
 * NeuralAxe timed pool sessions — PURE observation-pilot diagnostics
 * (Phase 2M.1B, Gate B10.1). Board 601 / BM1370 only.
 *
 * This header is the pure diagnostic domain of the supervised trusted-time
 * OBSERVATION pilot. It exists so a real device can be watched during a
 * bounded, owner-executed pilot without adding an HTTP route, without
 * changing any public schema and without exposing a single private value.
 *
 * It executes NOTHING: no ESP-IDF, no IO, no NVS, no SNTP, no FreeRTOS, no
 * networking, no heap, no logging, no global mutable state, no clock read.
 * Every function is total and deterministic; the adapter does the ESP_LOG
 * call, and only outside any lock.
 *
 * DIAGNOSTICS ARE NEVER AUTHORIZATION. Nothing here creates a session,
 * writes the B3 store, takes a B5 lease, changes the pool configuration,
 * touches the protocol permission, the ASIC gate or the mining grant, or
 * restarts the device. A detected invariant violation is REPORTED, never
 * acted upon: during an observation-only pilot the honest response to an
 * unexpected fact is a bounded log line, not an automatic intervention.
 *
 * PRIVACY BY CONSTRUCTION: no model in this header has a string field and no
 * function takes a string input, so a trusted-time hostname, a resolved
 * address, a raw wall-clock epoch, a pool identity, an account, a worker, a
 * password, a session identifier, a lease token or a record generation
 * cannot be carried, formatted or logged. Only fixed-width scalars, stable
 * enums and the committed dot-free token vocabularies appear.
 */

#define POOL_PILOT_MODEL_VERSION 1u

/* Bounded 60-second summary cadence, measured ONLY on the monotonic clock. */
#define POOL_PILOT_SUMMARY_PERIOD_US 60000000ull

/* Hard bound on the events one bounded step may emit. The worst case is the
 * very first step: boot + network-ready + link + start-attempt + source state
 * + invariant violation. */
#define POOL_PILOT_EVENTS_MAX 8u

/* Bounded formatting buffers. Both are sized for the longest token in every
 * vocabulary plus the widest 32-bit decimal fields (proved by test). */
#define POOL_PILOT_SUMMARY_MAX   448u
#define POOL_PILOT_VIOLATION_MAX 128u
#define POOL_PILOT_EVENT_MAX     64u

/* ------------------------------------------------------------------ */
/* Bounded event vocabulary                                            */
/* ------------------------------------------------------------------ */

/*
 * The stable pilot event vocabulary. Values are machine tokens only; any
 * value outside the enum resolves to PILOT_EVENT_UNKNOWN and never to a
 * healthy or authorizing meaning.
 *
 * TIME_SOURCE_CONFIGURED and TIME_SOURCE_STOPPED deliberately map to no
 * event: they are not pilot-significant transitions on their own, and the
 * bounded summary carries the current source state on every line anyway, so
 * nothing is hidden by their absence.
 */
typedef enum {
    POOL_PILOT_EVENT_NONE = 0,
    POOL_PILOT_EVENT_TIME_OBSERVE_BOOT,
    POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED,
    POOL_PILOT_EVENT_TIME_SOURCE_INVALID,
    POOL_PILOT_EVENT_NETWORK_READY,
    POOL_PILOT_EVENT_SNTP_START_ATTEMPT,
    POOL_PILOT_EVENT_SNTP_SYNCING,
    POOL_PILOT_EVENT_SNTP_TRUSTED,
    POOL_PILOT_EVENT_SNTP_REJECTED,
    POOL_PILOT_EVENT_SNTP_TIMEOUT,
    POOL_PILOT_EVENT_SNTP_ERROR,
    POOL_PILOT_EVENT_WIFI_LOST,
    POOL_PILOT_EVENT_WIFI_READY,
    POOL_PILOT_EVENT_OBSERVATION_SUMMARY,
    POOL_PILOT_EVENT_INVARIANT_VIOLATION,
    POOL_PILOT_EVENT__COUNT
} PoolPilotEvent;

/* Stable machine token (dot-free, no prose, never an identity). */
const char *pool_pilot_event_token(PoolPilotEvent e);

/* ------------------------------------------------------------------ */
/* Bounded invariant vocabulary                                        */
/* ------------------------------------------------------------------ */

/*
 * The healthy observation-pilot posture is an EMPTY (or CLEARED) session
 * store on a device that is mining its normal source pool. Every code below
 * names exactly one way that posture can be violated. Codes are checked and
 * reported in this enum order, so `first_violation` is deterministic.
 */
typedef enum {
    POOL_PILOT_INV_OK = 0,
    POOL_PILOT_INV_STORE_NOT_EMPTY,
    POOL_PILOT_INV_SESSION_RECORD_PRESENT,
    POOL_PILOT_INV_OWNER_PRESENT,
    POOL_PILOT_INV_RESTORE_REQUIRED,
    POOL_PILOT_INV_SESSION_WRITE,
    POOL_PILOT_INV_HEARTBEAT_WRITE,
    POOL_PILOT_INV_EXECUTION_REACHABLE,
    POOL_PILOT_INV_API_REACHABLE,
    POOL_PILOT_INV_MINING_GRANT,
    POOL_PILOT_INV_POOL_MUTATION,
    POOL_PILOT_INV_PROTOCOL_HELD,
    POOL_PILOT_INV_RUNTIME_NOT_FREE,
    POOL_PILOT_INV_ENUM_OUT_OF_RANGE,
    POOL_PILOT_INV_SNAPSHOT_INVALID,
    POOL_PILOT_INV__COUNT
} PoolPilotInvariant;

/* Stable machine token (dot-free, no prose, never an identity). */
const char *pool_pilot_invariant_token(PoolPilotInvariant c);

/*
 * Everything the pure invariant checker may know. Scalars, flags and stable
 * enums only — no pointer to mutable state, no record, no lease token, no
 * string. The caller reads these from the ALREADY-PUBLISHED sanitized B6
 * snapshot and B10 diagnostics; the checker itself reads nothing.
 */
typedef struct {
    uint32_t                      snapshot_model_version;
    bool                          snapshot_structurally_valid;

    PoolStoreResult               store_result;
    bool                          session_present;
    PoolOperationOwner            lease_owner;
    bool                          restore_required;

    uint32_t                      session_write_count;   /* B3 proven commits  */
    uint32_t                      heartbeat_write_count; /* B6 epoch heartbeats */

    bool                          execution_compiled;        /* Gate B7 flag  */
    bool                          execution_hook_registered; /* live executor */
    bool                          api_compiled;              /* Gate B8 flag  */
    bool                          api_hook_registered;       /* live mailbox  */

    bool                          target_mining_authorized;
    bool                          pool_mutation_permitted;

    PoolRuntimeProtocolPermission protocol;
    PoolRuntimeState              runtime_state;
    PoolTimeSourceState           time_state;
} PoolPilotInvariantInput;

/*
 * The deterministic verdict. `mask` carries one bit per violated code
 * (bit index == the PoolPilotInvariant value), `first` is the lowest-valued
 * violated code, and `healthy` is true exactly when mask == 0.
 */
typedef struct {
    bool               healthy;
    uint32_t           mask;
    uint32_t           count;
    PoolPilotInvariant first;
} PoolPilotInvariantReport;

/*
 * THE pure invariant check. Total, deterministic, input-immutable; *out is
 * fully written on every path. A NULL input or a NULL output fails CLOSED:
 * the report is unhealthy with POOL_PILOT_INV_SNAPSHOT_INVALID.
 *
 * It performs no mutation of any kind. It cannot restart the device, clear a
 * record, change the pool or alter mining — it only names what it saw.
 */
void pool_pilot_invariants_check(const PoolPilotInvariantInput *in,
                                 PoolPilotInvariantReport *out);

/* ------------------------------------------------------------------ */
/* Bounded observation step                                            */
/* ------------------------------------------------------------------ */

/*
 * One bounded observation, gathered by the adapter from already-published
 * bounded facts. `link_known` is false when no link fact is available at all
 * (the shipped binding, and every build without the pilot flag), in which
 * case no Wi-Fi event is ever emitted rather than a guessed one.
 */
typedef struct {
    bool                network_ready;
    bool                link_known;
    bool                link_up;
    PoolTimeSourceState source_state;
    uint32_t            attempt_count;
    uint64_t            monotonic_us;
    uint32_t            invariant_mask; /* 0 == healthy */
} PoolPilotObservation;

/*
 * The pilot's own bounded bookkeeping. RAM-only, owned exclusively by the
 * single B6 runtime owner task, and naturally reset by RAM loss on a reboot.
 * It holds no pointer, no handle, no identity and no string.
 */
typedef struct {
    uint32_t            model_version;
    bool                boot_emitted;
    bool                network_ready_emitted;
    bool                state_seen;
    PoolTimeSourceState last_state;
    uint32_t            last_attempt_count;
    bool                link_seen;
    bool                last_link_up;
    bool                summary_seen;
    uint64_t            last_summary_us;
    bool                violation_seen;
    uint32_t            last_violation_mask;
    uint32_t            sequence;    /* summaries emitted this boot   */
    uint32_t            emit_count;  /* audit: total events emitted   */
} PoolPilotState;

/* What one bounded step asks the adapter to log. Nothing else. */
typedef struct {
    uint32_t       count;
    PoolPilotEvent events[POOL_PILOT_EVENTS_MAX];
    bool           summary_due;
    uint32_t       sequence; /* the sequence number of THIS summary */
} PoolPilotStepResult;

/* Reset the bookkeeping to the "nothing observed yet" posture. */
void pool_pilot_state_init(PoolPilotState *st);

/*
 * THE pure bounded step. Advances `st` and reports which bounded events the
 * adapter must emit. Total and deterministic; *out is fully written on every
 * path and never carries more than POOL_PILOT_EVENTS_MAX events.
 *
 * RATE LIMITING (no log flood, by construction):
 *  - the observation summary is emitted at most once per
 *    POOL_PILOT_SUMMARY_PERIOD_US, measured ONLY on the monotonic clock;
 *  - a monotonic regression NEVER emits early: it re-anchors the window and
 *    waits the full period again;
 *  - lifecycle events are edge-triggered, so a steady state emits nothing;
 *  - an invariant violation is emitted when the violation set CHANGES (hard
 *    bounded by the number of codes) or alongside a due summary, never once
 *    per tick.
 *
 * NULL arguments are a no-op that emits nothing.
 */
void pool_pilot_step(PoolPilotState *st, const PoolPilotObservation *obs,
                     PoolPilotStepResult *out);

/* ------------------------------------------------------------------ */
/* Bounded formatting                                                  */
/* ------------------------------------------------------------------ */

/*
 * The bounded summary payload. Fixed-width scalars and stable enums only —
 * there is no string field, so no hostname, resolved address, raw epoch,
 * pool identity, session id, lease token or record generation can reach a
 * log line through it. Ages and uptimes are MONOTONIC seconds, never
 * wall-clock time.
 */
typedef struct {
    uint32_t                      sequence;
    uint32_t                      uptime_s;               /* monotonic */
    PoolTimeSourceState           source_state;
    bool                          trusted_available;
    bool                          trusted_operational;
    uint32_t                      attempt_count;
    bool                          sync_age_valid;
    uint32_t                      sync_age_s;             /* monotonic */
    PoolRuntimeProtocolPermission protocol;
    PoolRuntimeState              runtime_state;
    PoolOperationOwner            lease_owner;
    bool                          restore_required;
    uint32_t                      session_write_count;
    uint32_t                      heartbeat_write_count;
    bool                          execution_reachable;
    bool                          api_reachable;
    uint32_t                      free_internal_heap_b;
    uint32_t                      min_free_internal_heap_b;
    /* FreeRTOS stack high-water mark: the SMALLEST free headroom the owner
     * task has ever had, in the port's units (bytes on ESP-IDF). It shrinks
     * toward 0 as the deepest call path grows — 0 means overflow. */
    uint32_t                      owner_task_stack_hwm;
    PoolPilotInvariantReport      invariants;
} PoolPilotSummary;

/*
 * Format the bounded single-line summary into `buf`. Returns the number of
 * characters written (excluding the NUL), or 0 when the arguments are
 * invalid or the buffer is too small — in which case `buf` is left as an
 * empty string rather than a truncated half-line.
 *
 * The output is a stable `TOKEN key=value ...` line. It has no format
 * specifier that could accept a string, so no identity can be injected.
 */
uint32_t pool_pilot_summary_format(const PoolPilotSummary *s, char *buf, uint32_t cap);

/*
 * Format the bounded invariant-violation line. Same contract as above. This
 * line REPORTS: emitting it never restarts the device, never clears a
 * record, never changes the pool and never stops mining.
 */
uint32_t pool_pilot_violation_format(uint32_t sequence, uint32_t uptime_s,
                                     const PoolPilotInvariantReport *r,
                                     char *buf, uint32_t cap);

/* Format a bounded lifecycle-event line (`TOKEN up=<monotonic seconds>`). */
uint32_t pool_pilot_event_format(PoolPilotEvent e, uint32_t uptime_s,
                                 char *buf, uint32_t cap);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_PILOT_EVENT__COUNT == 15,
               "pilot event count changed — review tokens/tests/expected logs");
_Static_assert(POOL_PILOT_INV__COUNT == 15,
               "pilot invariant count changed — review tokens/tests/mask width");
_Static_assert(POOL_PILOT_INV__COUNT <= 32,
               "the violation mask is a uint32_t bitmask");
_Static_assert(POOL_PILOT_EVENTS_MAX >= 6u,
               "one step can legitimately need six events on the first tick");
_Static_assert(POOL_PILOT_INV_OK == 0,
               "OK must be zero so a zeroed report claims nothing");

#endif /* POOL_SESSION_RUNTIME_PILOT_H_ */
