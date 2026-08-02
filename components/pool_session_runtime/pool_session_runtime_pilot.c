/*
 * NeuralAxe timed pool sessions — PURE observation-pilot diagnostics
 * (Phase 2M.1B, Gate B10.1).
 *
 * See pool_session_runtime_pilot.h for the contract. Nothing in this file
 * performs IO, allocates, blocks, locks, logs, reads a clock or touches any
 * global mutable state; every function is total and deterministic, so the
 * whole diagnostic domain is exhaustively testable off-device.
 */

#include <string.h>
#include <stdio.h>

#include "pool_session_runtime_pilot.h"

/* ------------------------------------------------------------------ */
/* Token vocabularies                                                  */
/* ------------------------------------------------------------------ */

const char *pool_pilot_event_token(PoolPilotEvent e)
{
    switch (e) {
    case POOL_PILOT_EVENT_NONE:                      return "PILOT_EVENT_NONE";
    case POOL_PILOT_EVENT_TIME_OBSERVE_BOOT:         return "PILOT_TIME_OBSERVE_BOOT";
    case POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED:  return "PILOT_TIME_SOURCE_UNCONFIGURED";
    case POOL_PILOT_EVENT_TIME_SOURCE_INVALID:       return "PILOT_TIME_SOURCE_INVALID";
    case POOL_PILOT_EVENT_NETWORK_READY:             return "PILOT_NETWORK_READY";
    case POOL_PILOT_EVENT_SNTP_START_ATTEMPT:        return "PILOT_SNTP_START_ATTEMPT";
    case POOL_PILOT_EVENT_SNTP_SYNCING:              return "PILOT_SNTP_SYNCING";
    case POOL_PILOT_EVENT_SNTP_TRUSTED:              return "PILOT_SNTP_TRUSTED";
    case POOL_PILOT_EVENT_SNTP_REJECTED:             return "PILOT_SNTP_REJECTED";
    case POOL_PILOT_EVENT_SNTP_TIMEOUT:              return "PILOT_SNTP_TIMEOUT";
    case POOL_PILOT_EVENT_SNTP_ERROR:                return "PILOT_SNTP_ERROR";
    case POOL_PILOT_EVENT_WIFI_LOST:                 return "PILOT_WIFI_LOST";
    case POOL_PILOT_EVENT_WIFI_READY:                return "PILOT_WIFI_READY";
    case POOL_PILOT_EVENT_OBSERVATION_SUMMARY:       return "PILOT_OBSERVATION_SUMMARY";
    case POOL_PILOT_EVENT_INVARIANT_VIOLATION:       return "PILOT_INVARIANT_VIOLATION";
    case POOL_PILOT_EVENT__COUNT:
    default:                                         return "PILOT_EVENT_UNKNOWN";
    }
}

const char *pool_pilot_invariant_token(PoolPilotInvariant c)
{
    switch (c) {
    case POOL_PILOT_INV_OK:                     return "PILOT_INV_OK";
    case POOL_PILOT_INV_STORE_NOT_EMPTY:        return "PILOT_INV_STORE_NOT_EMPTY";
    case POOL_PILOT_INV_SESSION_RECORD_PRESENT: return "PILOT_INV_SESSION_RECORD_PRESENT";
    case POOL_PILOT_INV_OWNER_PRESENT:          return "PILOT_INV_OWNER_PRESENT";
    case POOL_PILOT_INV_RESTORE_REQUIRED:       return "PILOT_INV_RESTORE_REQUIRED";
    case POOL_PILOT_INV_SESSION_WRITE:          return "PILOT_INV_SESSION_WRITE";
    case POOL_PILOT_INV_HEARTBEAT_WRITE:        return "PILOT_INV_HEARTBEAT_WRITE";
    case POOL_PILOT_INV_EXECUTION_REACHABLE:    return "PILOT_INV_EXECUTION_REACHABLE";
    case POOL_PILOT_INV_API_REACHABLE:          return "PILOT_INV_API_REACHABLE";
    case POOL_PILOT_INV_MINING_GRANT:           return "PILOT_INV_MINING_GRANT";
    case POOL_PILOT_INV_POOL_MUTATION:          return "PILOT_INV_POOL_MUTATION";
    case POOL_PILOT_INV_PROTOCOL_HELD:          return "PILOT_INV_PROTOCOL_HELD";
    case POOL_PILOT_INV_RUNTIME_NOT_FREE:       return "PILOT_INV_RUNTIME_NOT_FREE";
    case POOL_PILOT_INV_ENUM_OUT_OF_RANGE:      return "PILOT_INV_ENUM_OUT_OF_RANGE";
    case POOL_PILOT_INV_SNAPSHOT_INVALID:       return "PILOT_INV_SNAPSHOT_INVALID";
    case POOL_PILOT_INV__COUNT:
    default:                                    return "PILOT_INV_UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* Invariant checking                                                  */
/* ------------------------------------------------------------------ */

static void pilot_report_init(PoolPilotInvariantReport *out)
{
    memset(out, 0, sizeof(*out));
    out->healthy = true;
    out->first   = POOL_PILOT_INV_OK;
}

static void pilot_flag(PoolPilotInvariantReport *out, PoolPilotInvariant c)
{
    const uint32_t bit = 1u << (uint32_t)c;

    if ((out->mask & bit) != 0u) {
        return; /* idempotent: a code is counted at most once */
    }
    if (out->mask == 0u) {
        out->first = c; /* codes are raised in enum order, so this is the lowest */
    }
    out->mask |= bit;
    out->count++;
    out->healthy = false;
}

/* Range checks are separate from semantic checks so an out-of-range enum is
 * reported as exactly that, and never silently read as a healthy value. */
static bool pilot_enums_in_range(const PoolPilotInvariantInput *in)
{
    if ((uint32_t)in->runtime_state >= (uint32_t)POOL_RUNTIME_STATE__COUNT) {
        return false;
    }
    if ((uint32_t)in->protocol >= (uint32_t)POOL_RUNTIME_PROTOCOL__COUNT) {
        return false;
    }
    if ((uint32_t)in->time_state >= (uint32_t)POOL_TIME_SOURCE_STATE__COUNT) {
        return false;
    }
    if ((uint32_t)in->lease_owner >= (uint32_t)OP_OWNER__COUNT) {
        return false;
    }
    switch (in->store_result) {
    case STORE_OK:
    case STORE_EMPTY:
    case STORE_CLEARED:
    case STORE_NOT_INITIALIZED:
    case STORE_INVALID_ARGUMENT:
    case STORE_IO_ERROR:
    case STORE_CORRUPT:
    case STORE_UNSUPPORTED_SCHEMA:
    case STORE_INVALID_RECORD:
    case STORE_ACTIVE_POINTER_INVALID:
    case STORE_ACTIVE_SLOT_INVALID:
    case STORE_RECOVERY_REQUIRED:
    case STORE_GENERATION_EXHAUSTED:
    case STORE_READBACK_MISMATCH:
    case STORE_COMMIT_UNCERTAIN:
    case STORE_STATE_CONFLICT:
        break;
    default:
        return false;
    }
    return true;
}

void pool_pilot_invariants_check(const PoolPilotInvariantInput *in,
                                 PoolPilotInvariantReport *out)
{
    if (out == NULL) {
        return;
    }
    pilot_report_init(out);
    if (in == NULL) {
        pilot_flag(out, POOL_PILOT_INV_SNAPSHOT_INVALID); /* fail closed */
        return;
    }

    /* Checked in enum order so `first` is deterministic. */
    if (in->store_result != STORE_EMPTY && in->store_result != STORE_CLEARED) {
        pilot_flag(out, POOL_PILOT_INV_STORE_NOT_EMPTY);
    }
    if (in->session_present) {
        pilot_flag(out, POOL_PILOT_INV_SESSION_RECORD_PRESENT);
    }
    if (in->lease_owner != OP_OWNER_NONE) {
        pilot_flag(out, POOL_PILOT_INV_OWNER_PRESENT);
    }
    if (in->restore_required) {
        pilot_flag(out, POOL_PILOT_INV_RESTORE_REQUIRED);
    }
    if (in->session_write_count != 0u) {
        pilot_flag(out, POOL_PILOT_INV_SESSION_WRITE);
    }
    if (in->heartbeat_write_count != 0u) {
        pilot_flag(out, POOL_PILOT_INV_HEARTBEAT_WRITE);
    }
    if (in->execution_compiled || in->execution_hook_registered) {
        pilot_flag(out, POOL_PILOT_INV_EXECUTION_REACHABLE);
    }
    if (in->api_compiled || in->api_hook_registered) {
        pilot_flag(out, POOL_PILOT_INV_API_REACHABLE);
    }
    if (in->target_mining_authorized) {
        pilot_flag(out, POOL_PILOT_INV_MINING_GRANT);
    }
    if (in->pool_mutation_permitted) {
        pilot_flag(out, POOL_PILOT_INV_POOL_MUTATION);
    }
    if (in->protocol != POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE) {
        pilot_flag(out, POOL_PILOT_INV_PROTOCOL_HELD);
    }
    if (in->runtime_state != RUNTIME_FREE) {
        pilot_flag(out, POOL_PILOT_INV_RUNTIME_NOT_FREE);
    }
    if (!pilot_enums_in_range(in)) {
        pilot_flag(out, POOL_PILOT_INV_ENUM_OUT_OF_RANGE);
    }
    if (!in->snapshot_structurally_valid ||
        in->snapshot_model_version != POOL_RUNTIME_MODEL_VERSION) {
        pilot_flag(out, POOL_PILOT_INV_SNAPSHOT_INVALID);
    }
}

/* ------------------------------------------------------------------ */
/* Bounded observation step                                            */
/* ------------------------------------------------------------------ */

void pool_pilot_state_init(PoolPilotState *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->model_version = POOL_PILOT_MODEL_VERSION;
    st->last_state    = TIME_SOURCE_UNCONFIGURED;
}

/*
 * The stable source-state -> event mapping. CONFIGURED and STOPPED map to no
 * event on purpose (see the header); every bounded summary still carries the
 * current state, so no posture is ever unreported.
 */
static PoolPilotEvent pilot_event_for_state(PoolTimeSourceState s)
{
    switch (s) {
    case TIME_SOURCE_UNCONFIGURED:  return POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED;
    case TIME_SOURCE_INVALID:       return POOL_PILOT_EVENT_TIME_SOURCE_INVALID;
    case TIME_SOURCE_START_PENDING: return POOL_PILOT_EVENT_SNTP_START_ATTEMPT;
    case TIME_SOURCE_SYNCING:       return POOL_PILOT_EVENT_SNTP_SYNCING;
    case TIME_SOURCE_TRUSTED:       return POOL_PILOT_EVENT_SNTP_TRUSTED;
    case TIME_SOURCE_REJECTED:      return POOL_PILOT_EVENT_SNTP_REJECTED;
    case TIME_SOURCE_TIMEOUT:       return POOL_PILOT_EVENT_SNTP_TIMEOUT;
    case TIME_SOURCE_ERROR:         return POOL_PILOT_EVENT_SNTP_ERROR;
    case TIME_SOURCE_CONFIGURED:
    case TIME_SOURCE_STOPPED:
    default:                        return POOL_PILOT_EVENT_NONE;
    }
}

static void pilot_emit(PoolPilotStepResult *out, PoolPilotState *st, PoolPilotEvent e)
{
    if (e == POOL_PILOT_EVENT_NONE || out->count >= POOL_PILOT_EVENTS_MAX) {
        return;
    }
    out->events[out->count++] = e;
    if (st->emit_count < UINT32_MAX) {
        st->emit_count++;
    }
}

void pool_pilot_step(PoolPilotState *st, const PoolPilotObservation *obs,
                     PoolPilotStepResult *out)
{
    bool summary_due;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (st == NULL || obs == NULL) {
        return;
    }

    /*
     * Monotonic-only cadence. A regression (a fake or restarted clock) never
     * emits early: it re-anchors the window and waits the full period again.
     */
    if (!st->summary_seen) {
        summary_due = true;
    } else if (obs->monotonic_us < st->last_summary_us) {
        st->last_summary_us = obs->monotonic_us;
        summary_due         = false;
    } else {
        summary_due = (obs->monotonic_us - st->last_summary_us) >= POOL_PILOT_SUMMARY_PERIOD_US;
    }

    /* 1 — the one-shot boot marker. */
    if (!st->boot_emitted) {
        st->boot_emitted = true;
        pilot_emit(out, st, POOL_PILOT_EVENT_TIME_OBSERVE_BOOT);
    }

    /* 2 — the one-shot network-ready marker. */
    if (obs->network_ready && !st->network_ready_emitted) {
        st->network_ready_emitted = true;
        pilot_emit(out, st, POOL_PILOT_EVENT_NETWORK_READY);
    }

    /* 3 — edge-triggered link state. Nothing is emitted while the link fact
     *     is unavailable, so a build without a link binding stays silent
     *     rather than reporting a guess. */
    if (obs->link_known) {
        if (!st->link_seen || st->last_link_up != obs->link_up) {
            pilot_emit(out, st, obs->link_up ? POOL_PILOT_EVENT_WIFI_READY
                                             : POOL_PILOT_EVENT_WIFI_LOST);
        }
        st->link_seen    = true;
        st->last_link_up = obs->link_up;
    }

    /* 4 — a consumed provider start attempt. Hard bounded by the committed
     *     B10 attempt budget, so this can fire only a handful of times. */
    if (obs->attempt_count > st->last_attempt_count) {
        pilot_emit(out, st, POOL_PILOT_EVENT_SNTP_START_ATTEMPT);
    }
    st->last_attempt_count = obs->attempt_count;

    /* 5 — edge-triggered source lifecycle. */
    if (!st->state_seen || st->last_state != obs->source_state) {
        pilot_emit(out, st, pilot_event_for_state(obs->source_state));
    }
    st->state_seen = true;
    st->last_state = obs->source_state;

    /* 6 — bounded invariant reporting: on a CHANGED violation set (bounded by
     *     the number of codes) or alongside a due summary. Never per tick. */
    if (obs->invariant_mask != 0u) {
        if (!st->violation_seen || st->last_violation_mask != obs->invariant_mask ||
            summary_due) {
            pilot_emit(out, st, POOL_PILOT_EVENT_INVARIANT_VIOLATION);
        }
        st->violation_seen = true;
    } else {
        st->violation_seen = false;
    }
    st->last_violation_mask = obs->invariant_mask;

    if (summary_due) {
        st->summary_seen    = true;
        st->last_summary_us = obs->monotonic_us;
        if (st->sequence < UINT32_MAX) {
            st->sequence++;
        }
        out->summary_due = true;
        out->sequence    = st->sequence;
        if (st->emit_count < UINT32_MAX) {
            st->emit_count++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bounded formatting                                                  */
/* ------------------------------------------------------------------ */

/*
 * A shared bounded finish: snprintf() truncates silently, which would make a
 * log line ambiguous, so a line that does not fit is discarded entirely and
 * reported as 0 rather than half-written.
 */
static uint32_t pilot_finish(char *buf, uint32_t cap, int n)
{
    if (n < 0 || (uint32_t)n >= cap) {
        buf[0] = '\0';
        return 0u;
    }
    return (uint32_t)n;
}

uint32_t pool_pilot_summary_format(const PoolPilotSummary *s, char *buf, uint32_t cap)
{
    int n;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (s == NULL) {
        return 0u;
    }

    n = snprintf(buf, (size_t)cap,
                 "%s seq=%u up=%u src=%s avail=%u oper=%u att=%u agev=%u age=%u "
                 "proto=%s rt=%s owner=%s restore=%u b3w=%u hbw=%u exec=%u api=%u "
                 "heap=%u heapmin=%u hwm=%u inv=%s invn=%u",
                 pool_pilot_event_token(POOL_PILOT_EVENT_OBSERVATION_SUMMARY),
                 (unsigned)s->sequence, (unsigned)s->uptime_s,
                 pool_time_source_state_str(s->source_state),
                 (unsigned)(s->trusted_available ? 1u : 0u),
                 (unsigned)(s->trusted_operational ? 1u : 0u),
                 (unsigned)s->attempt_count,
                 (unsigned)(s->sync_age_valid ? 1u : 0u),
                 (unsigned)s->sync_age_s,
                 pool_runtime_protocol_str(s->protocol),
                 pool_runtime_state_str(s->runtime_state),
                 pool_operation_owner_str(s->lease_owner),
                 (unsigned)(s->restore_required ? 1u : 0u),
                 (unsigned)s->session_write_count,
                 (unsigned)s->heartbeat_write_count,
                 (unsigned)(s->execution_reachable ? 1u : 0u),
                 (unsigned)(s->api_reachable ? 1u : 0u),
                 (unsigned)s->free_internal_heap_b,
                 (unsigned)s->min_free_internal_heap_b,
                 (unsigned)s->owner_task_stack_hwm,
                 pool_pilot_invariant_token(s->invariants.first),
                 (unsigned)s->invariants.count);
    return pilot_finish(buf, cap, n);
}

uint32_t pool_pilot_violation_format(uint32_t sequence, uint32_t uptime_s,
                                     const PoolPilotInvariantReport *r,
                                     char *buf, uint32_t cap)
{
    int n;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (r == NULL) {
        return 0u;
    }

    n = snprintf(buf, (size_t)cap, "%s seq=%u up=%u first=%s n=%u mask=0x%08x",
                 pool_pilot_event_token(POOL_PILOT_EVENT_INVARIANT_VIOLATION),
                 (unsigned)sequence, (unsigned)uptime_s,
                 pool_pilot_invariant_token(r->first),
                 (unsigned)r->count, (unsigned)r->mask);
    return pilot_finish(buf, cap, n);
}

uint32_t pool_pilot_event_format(PoolPilotEvent e, uint32_t uptime_s,
                                 char *buf, uint32_t cap)
{
    int n;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    n = snprintf(buf, (size_t)cap, "%s up=%u", pool_pilot_event_token(e),
                 (unsigned)uptime_s);
    return pilot_finish(buf, cap, n);
}
