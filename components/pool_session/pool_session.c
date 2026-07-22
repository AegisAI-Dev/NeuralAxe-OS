/*
 * NeuralAxe timed pool sessions — pure FSM implementation (Phase 2M.1B, B1).
 * See pool_session.h for the full contract. PURE: no NVS, SNTP, clock, tasks,
 * Stratum, HTTP, logging, restart or heap. No password anywhere.
 */

#include "pool_session.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* True if `s` is NUL-terminated within `maxbuf` bytes (i.e. bounded). */
static bool str_bounded(const char *s, size_t maxbuf)
{
    if (s == NULL) return false;
    for (size_t i = 0; i < maxbuf; i++) {
        if (s[i] == '\0') return true;
    }
    return false;
}

static bool enum_in_range(unsigned v, unsigned count)
{
    return v < count;
}

/* Increment a bounded retry counter; return true if a retry is permitted. */
static bool retry_take(uint8_t *counter, uint8_t max)
{
    if (*counter < max) {
        (*counter)++;
        return true;
    }
    return false; /* saturated: no further retry */
}

static void verify_reset(PoolSessionVerify *v)
{
    v->connection_observed = false;
    v->mining_observed = false;
    v->identity_verified = false;
}

static bool verify_all(const PoolSessionVerify *v)
{
    return v->connection_observed && v->mining_observed && v->identity_verified;
}

/*
 * Set a verification flag. Returns true if the flag transitioned from false to
 * true (a real change); false if it was already set (idempotent duplicate) —
 * satisfying "duplicate verification events are no-ops after already recorded".
 */
static bool verify_set(bool *flag)
{
    if (*flag) return false;
    *flag = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Classification helpers (total, invalid-safe)                        */
/* ------------------------------------------------------------------ */

bool pool_state_is_persistent(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_TARGET_SNAPSHOT_COMMITTED:
        case POOL_STATE_APPLYING_TARGET:
        case POOL_STATE_TARGET_ACTIVE:
        case POOL_STATE_RESTORE_DUE:
        case POOL_STATE_APPLYING_RESTORE:
        case POOL_STATE_COMPLETE:
        case POOL_STATE_TARGET_FAILED:
        case POOL_STATE_RESTORE_FAILED:
        case POOL_STATE_INTERRUPTED:
        case POOL_STATE_RECOVERY_REQUIRED:
        case POOL_STATE_CANCELLED:
            return true;
        default:
            return false;
    }
}

bool pool_state_is_terminal(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_COMPLETE:
        case POOL_STATE_CANCELLED:
        case POOL_STATE_RESTORE_FAILED:
        case POOL_STATE_RECOVERY_REQUIRED:
            return true;
        default:
            return false;
    }
}

bool pool_state_is_active_session(PoolSessionState st)
{
    if (!enum_in_range((unsigned)st, POOL_STATE__COUNT)) return false;
    if (st == POOL_STATE_IDLE) return false;
    return !pool_state_is_terminal(st);
}

bool pool_state_is_target_side(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_PREPARING:
        case POOL_STATE_TARGET_SNAPSHOT_COMMITTED:
        case POOL_STATE_APPLYING_TARGET:
        case POOL_STATE_RESTARTING_FOR_TARGET:
        case POOL_STATE_VERIFYING_TARGET:
        case POOL_STATE_TARGET_ACTIVE:
        case POOL_STATE_TARGET_FAILED:
            return true;
        default:
            return false;
    }
}

bool pool_state_is_restore_side(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_RESTORE_DUE:
        case POOL_STATE_APPLYING_RESTORE:
        case POOL_STATE_RESTARTING_FOR_RESTORE:
        case POOL_STATE_VERIFYING_RESTORE:
        case POOL_STATE_RESTORE_FAILED:
        case POOL_STATE_INTERRUPTED:
            return true;
        default:
            return false;
    }
}

bool pool_session_restore_required(const PoolSession *s)
{
    return s != NULL && s->restore_required;
}

bool pool_session_is_acknowledgeable_terminal(const PoolSession *s)
{
    if (s == NULL) return false;
    /* Only a result state with NO outstanding restore obligation may clear. */
    return pool_state_is_terminal(s->state) && !s->restore_required;
}

bool pool_session_blocks_manual_pool_change(const PoolSession *s)
{
    if (s == NULL) return false;
    if (s->state == POOL_STATE_IDLE) return false;
    /* Unresolved (in progress, or a result state still owing a restore). */
    return !pool_session_is_acknowledgeable_terminal(s);
}

bool pool_session_blocks_ota(const PoolSession *s)
{
    return pool_session_blocks_manual_pool_change(s);
}

bool pool_state_allows_restore_now(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_APPLYING_TARGET:
        case POOL_STATE_RESTARTING_FOR_TARGET:
        case POOL_STATE_VERIFYING_TARGET:
        case POOL_STATE_TARGET_ACTIVE:
        case POOL_STATE_RESTORE_DUE:
        case POOL_STATE_APPLYING_RESTORE:
        case POOL_STATE_RESTARTING_FOR_RESTORE:
        case POOL_STATE_VERIFYING_RESTORE:
        case POOL_STATE_TARGET_FAILED:
        case POOL_STATE_RESTORE_FAILED:
        case POOL_STATE_INTERRUPTED:
        case POOL_STATE_RECOVERY_REQUIRED:
            return true;
        default:
            return false;
    }
}

bool pool_state_allows_cancel(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_PREPARING:
        case POOL_STATE_TARGET_SNAPSHOT_COMMITTED:
        case POOL_STATE_APPLYING_TARGET:
        case POOL_STATE_RESTARTING_FOR_TARGET:
        case POOL_STATE_VERIFYING_TARGET:
        case POOL_STATE_TARGET_ACTIVE:
            return true;
        default:
            return false;
    }
}

bool pool_state_requires_source_snapshot(PoolSessionState st)
{
    if (!enum_in_range((unsigned)st, POOL_STATE__COUNT)) return false;
    return st != POOL_STATE_IDLE && st != POOL_STATE_PREPARING;
}

bool pool_state_requires_target_identity(PoolSessionState st)
{
    if (!enum_in_range((unsigned)st, POOL_STATE__COUNT)) return false;
    return st != POOL_STATE_IDLE;
}

/* ------------------------------------------------------------------ */
/* Diagnostic strings (machine tokens; never a secret/account/host)    */
/* ------------------------------------------------------------------ */

const char *pool_session_error_str(PoolSessionError e)
{
    switch (e) {
        case ERR_NONE:                     return "ERR_NONE";
        case ERR_INVALID_REQUEST:          return "ERR_INVALID_REQUEST";
        case ERR_UNSUPPORTED_BOARD:        return "ERR_UNSUPPORTED_BOARD";
        case ERR_SESSION_ALREADY_ACTIVE:   return "ERR_SESSION_ALREADY_ACTIVE";
        case ERR_NO_ACTIVE_SESSION:        return "ERR_NO_ACTIVE_SESSION";
        case ERR_INVALID_DURATION:         return "ERR_INVALID_DURATION";
        case ERR_PW_MODE_UNSUPPORTED:      return "ERR_PW_MODE_UNSUPPORTED";
        case ERR_SOURCE_IDENTITY_INVALID:  return "ERR_SOURCE_IDENTITY_INVALID";
        case ERR_TARGET_IDENTITY_INVALID:  return "ERR_TARGET_IDENTITY_INVALID";
        case ERR_TARGET_EQUALS_SOURCE:     return "ERR_TARGET_EQUALS_SOURCE";
        case ERR_STATE_CONFLICT:           return "ERR_STATE_CONFLICT";
        case ERR_ILLEGAL_TRANSITION:       return "ERR_ILLEGAL_TRANSITION";
        case ERR_TARGET_APPLY:             return "ERR_TARGET_APPLY";
        case ERR_TARGET_RESTART:           return "ERR_TARGET_RESTART";
        case ERR_TARGET_CONNECT_TIMEOUT:   return "ERR_TARGET_CONNECT_TIMEOUT";
        case ERR_TARGET_VERIFY_TIMEOUT:    return "ERR_TARGET_VERIFY_TIMEOUT";
        case ERR_TARGET_IDENTITY_MISMATCH: return "ERR_TARGET_IDENTITY_MISMATCH";
        case ERR_RESTORE_APPLY:            return "ERR_RESTORE_APPLY";
        case ERR_RESTORE_RESTART:          return "ERR_RESTORE_RESTART";
        case ERR_RESTORE_CONNECT_TIMEOUT:  return "ERR_RESTORE_CONNECT_TIMEOUT";
        case ERR_RESTORE_VERIFY_TIMEOUT:   return "ERR_RESTORE_VERIFY_TIMEOUT";
        case ERR_RESTORE_IDENTITY_MISMATCH:return "ERR_RESTORE_IDENTITY_MISMATCH";
        case ERR_RETRY_EXHAUSTED:          return "ERR_RETRY_EXHAUSTED";
        case ERR_RECORD_CORRUPT:           return "ERR_RECORD_CORRUPT";
        case ERR_SCHEMA_UNSUPPORTED:       return "ERR_SCHEMA_UNSUPPORTED";
        case ERR_RECOVERY_REQUIRED:        return "ERR_RECOVERY_REQUIRED";
        case ERR_CANCELLED:                return "ERR_CANCELLED";
        case ERR_INTERRUPTED:              return "ERR_INTERRUPTED";
        default:                           return "ERR_UNKNOWN";
    }
}

const char *pool_session_state_str(PoolSessionState st)
{
    switch (st) {
        case POOL_STATE_IDLE:                      return "IDLE";
        case POOL_STATE_PREPARING:                 return "PREPARING";
        case POOL_STATE_TARGET_SNAPSHOT_COMMITTED: return "TARGET_SNAPSHOT_COMMITTED";
        case POOL_STATE_APPLYING_TARGET:           return "APPLYING_TARGET";
        case POOL_STATE_RESTARTING_FOR_TARGET:     return "RESTARTING_FOR_TARGET";
        case POOL_STATE_VERIFYING_TARGET:          return "VERIFYING_TARGET";
        case POOL_STATE_TARGET_ACTIVE:             return "TARGET_ACTIVE";
        case POOL_STATE_RESTORE_DUE:               return "RESTORE_DUE";
        case POOL_STATE_APPLYING_RESTORE:          return "APPLYING_RESTORE";
        case POOL_STATE_RESTARTING_FOR_RESTORE:    return "RESTARTING_FOR_RESTORE";
        case POOL_STATE_VERIFYING_RESTORE:         return "VERIFYING_RESTORE";
        case POOL_STATE_COMPLETE:                  return "COMPLETE";
        case POOL_STATE_TARGET_FAILED:             return "TARGET_FAILED";
        case POOL_STATE_RESTORE_FAILED:            return "RESTORE_FAILED";
        case POOL_STATE_INTERRUPTED:               return "INTERRUPTED";
        case POOL_STATE_RECOVERY_REQUIRED:         return "RECOVERY_REQUIRED";
        case POOL_STATE_CANCELLED:                 return "CANCELLED";
        default:                                   return "UNKNOWN_STATE";
    }
}

const char *pool_session_side_effect_str(PoolSessionSideEffect se)
{
    switch (se) {
        case POOL_SIDE_NO_EFFECT:                 return "NO_EFFECT";
        case POOL_SIDE_COMMIT_SOURCE_SNAPSHOT:    return "COMMIT_SOURCE_SNAPSHOT";
        case POOL_SIDE_APPLY_TARGET_CONFIGURATION:return "APPLY_TARGET_CONFIGURATION";
        case POOL_SIDE_RESTART_FOR_TARGET:        return "RESTART_FOR_TARGET";
        case POOL_SIDE_BEGIN_TARGET_VERIFICATION: return "BEGIN_TARGET_VERIFICATION";
        case POOL_SIDE_ARM_MONOTONIC_DEADLINE:    return "ARM_MONOTONIC_DEADLINE";
        case POOL_SIDE_APPLY_SOURCE_CONFIGURATION:return "APPLY_SOURCE_CONFIGURATION";
        case POOL_SIDE_RESTART_FOR_RESTORE:       return "RESTART_FOR_RESTORE";
        case POOL_SIDE_BEGIN_RESTORE_VERIFICATION:return "BEGIN_RESTORE_VERIFICATION";
        case POOL_SIDE_PERSIST_TRANSITION:        return "PERSIST_TRANSITION";
        case POOL_SIDE_CLEAR_SESSION_RECORD:      return "CLEAR_SESSION_RECORD";
        case POOL_SIDE_RETAIN_TERMINAL_RESULT:    return "RETAIN_TERMINAL_RESULT";
        default:                                  return "UNKNOWN_SIDE_EFFECT";
    }
}

/* ------------------------------------------------------------------ */
/* Identity equality + validation                                      */
/* ------------------------------------------------------------------ */

bool pool_endpoint_equal(const PoolEndpoint *a, const PoolEndpoint *b)
{
    if (a == NULL || b == NULL) return false;
    if (!str_bounded(a->host, POOL_SESSION_HOST_MAX) ||
        !str_bounded(b->host, POOL_SESSION_HOST_MAX) ||
        !str_bounded(a->user, POOL_SESSION_USER_MAX) ||
        !str_bounded(b->user, POOL_SESSION_USER_MAX)) {
        return false;
    }
    return a->port == b->port &&
           a->protocol == b->protocol &&
           a->tls == b->tls &&
           strncmp(a->host, b->host, POOL_SESSION_HOST_MAX) == 0 &&
           strncmp(a->user, b->user, POOL_SESSION_USER_MAX) == 0;
}

bool pool_config_identity_equal(const PoolConfigIdentity *a, const PoolConfigIdentity *b)
{
    if (a == NULL || b == NULL) return false;
    if (a->chain != b->chain) return false;
    if (a->fallback_enabled != b->fallback_enabled) return false;
    if (!pool_endpoint_equal(&a->primary, &b->primary)) return false;
    if (a->fallback_enabled && !pool_endpoint_equal(&a->fallback, &b->fallback)) return false;
    return true;
}

static bool endpoint_valid(const PoolEndpoint *e)
{
    if (!str_bounded(e->host, POOL_SESSION_HOST_MAX)) return false;
    if (e->host[0] == '\0') return false;                 /* non-empty host */
    if (e->port == 0) return false;                       /* 1..65535       */
    if (!str_bounded(e->user, POOL_SESSION_USER_MAX)) return false; /* may be "" */
    if (!enum_in_range((unsigned)e->protocol, POOL_PROTO__COUNT)) return false;
    return true;
}

static bool config_valid(const PoolConfigIdentity *c)
{
    if (!enum_in_range((unsigned)c->chain, POOL_CHAIN__COUNT)) return false;
    if (!str_bounded(c->profile_id, POOL_SESSION_PROFILE_ID_MAX)) return false;
    if (!endpoint_valid(&c->primary)) return false;
    if (c->fallback_enabled && !endpoint_valid(&c->fallback)) return false;
    return true;
}

PoolSessionError pool_session_validate_request(const PoolSessionRequest *req)
{
    if (req == NULL) return ERR_INVALID_REQUEST;

    if (req->model_version != POOL_SESSION_MODEL_VERSION) return ERR_SCHEMA_UNSUPPORTED;
    if (req->session_id == 0u) return ERR_INVALID_REQUEST;

    /* Hardware eligibility: board 601 / BM1370 only (explicit inputs). */
    if (!str_bounded(req->board_version, POOL_SESSION_BOARD_MAX) ||
        strncmp(req->board_version, POOL_SESSION_SUPPORTED_BOARD, POOL_SESSION_BOARD_MAX) != 0) {
        return ERR_UNSUPPORTED_BOARD;
    }
    if (!str_bounded(req->asic_model, POOL_SESSION_ASIC_MAX) ||
        strncmp(req->asic_model, POOL_SESSION_SUPPORTED_ASIC, POOL_SESSION_ASIC_MAX) != 0) {
        return ERR_UNSUPPORTED_BOARD;
    }

    /* Keep-current-password only. */
    if (req->password_policy != POOL_SESSION_PW_KEEP_CURRENT) return ERR_PW_MODE_UNSUPPORTED;

    /* Duration bounds (already seconds; range check, no multiplication here). */
    if (req->duration_s < POOL_SESSION_MIN_DURATION_S ||
        req->duration_s > POOL_SESSION_MAX_DURATION_S) {
        return ERR_INVALID_DURATION;
    }

    if (!config_valid(&req->source)) return ERR_SOURCE_IDENTITY_INVALID;
    if (!config_valid(&req->target)) return ERR_TARGET_IDENTITY_INVALID;

    /* Same-chain different pool is valid; exact identity match is a no-op. */
    if (pool_config_identity_equal(&req->source, &req->target)) return ERR_TARGET_EQUALS_SOURCE;

    return ERR_NONE;
}

bool pool_session_minutes_to_seconds(uint32_t minutes, uint32_t *out_seconds)
{
    if (out_seconds == NULL) return false;
    if (minutes > (0xFFFFFFFFu / 60u)) return false;      /* multiply overflow  */
    uint32_t s = minutes * 60u;
    if (s > POOL_SESSION_MAX_DURATION_S) return false;    /* exceeds allowed max */
    *out_seconds = s;
    return true;
}

/* ------------------------------------------------------------------ */
/* Session init                                                        */
/* ------------------------------------------------------------------ */

void pool_session_init(PoolSession *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->model_version = POOL_SESSION_MODEL_VERSION;
    s->session_id = 0u;
    s->generation = 0u;
    s->state = POOL_STATE_IDLE;
    s->password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    s->last_error = ERR_NONE;
    s->terminal_result = POOL_STATE_IDLE;
}

/* ------------------------------------------------------------------ */
/* Transition engine                                                   */
/* ------------------------------------------------------------------ */

/* Convenience setters operating on the outcome `o` and next session `n`. */
#define GO(st, side, err)  do { o.next_state = (st); o.side_effect = (side); \
                                o.error = (err); o.changed = true; } while (0)
#define NOOP(err)          do { o.next_state = current->state; \
                                o.side_effect = POOL_SIDE_NO_EFFECT; \
                                o.error = (err); o.changed = false; } while (0)
#define ILLEGAL()          NOOP(ERR_ILLEGAL_TRANSITION)

static void handle_create(const PoolSession *current, const PoolSessionEvent *ev,
                          PoolSession *n, PoolSessionOutcome *o)
{
    if (current->state != POOL_STATE_IDLE) {
        /* An active/terminal session exists. */
        if (current->session_id == ev->session_id) {
            o->next_state = current->state;
            o->side_effect = POOL_SIDE_NO_EFFECT;
            o->error = ERR_NONE;         /* idempotent duplicate — return existing */
            o->changed = false;
        } else {
            o->next_state = current->state;
            o->side_effect = POOL_SIDE_NO_EFFECT;
            o->error = ERR_SESSION_ALREADY_ACTIVE;
            o->changed = false;
        }
        return;
    }
    if (ev->request == NULL) {
        o->next_state = POOL_STATE_IDLE;
        o->side_effect = POOL_SIDE_NO_EFFECT;
        o->error = ERR_INVALID_REQUEST;
        o->changed = false;
        return;
    }
    PoolSessionError ve = pool_session_validate_request(ev->request);
    if (ve != ERR_NONE) {
        o->next_state = POOL_STATE_IDLE;
        o->side_effect = POOL_SIDE_NO_EFFECT;
        o->error = ve;                   /* rejected; stays IDLE */
        o->changed = false;
        return;
    }
    /* Build a fresh PREPARING session from the validated request, in place
       (no large nested stack copy). */
    pool_session_init(n);
    n->model_version = ev->request->model_version;
    n->session_id = ev->request->session_id;
    n->duration_s = ev->request->duration_s;
    n->password_policy = ev->request->password_policy;
    n->source = ev->request->source;
    n->target = ev->request->target;
    n->state = POOL_STATE_PREPARING;
    o->next_state = POOL_STATE_PREPARING;
    o->side_effect = POOL_SIDE_NO_EFFECT; /* commit happens on SOURCE_SNAPSHOT_COMMITTED */
    o->error = ERR_NONE;
    o->changed = true;
}

/*
 * DEVICE_RESTART_OBSERVED: conservative, SAFE handling. Never emits a pool
 * mutation intent (COMMIT, APPLY, RESTART or CLEAR) and never touches source or
 * target identity. Full boot recovery is Gate B4; B1 only guarantees safety.
 */
static void handle_device_restart(const PoolSession *current, PoolSession *n,
                                  PoolSessionOutcome *o)
{
    o->error = ERR_NONE;
    switch (current->state) {
        case POOL_STATE_RESTARTING_FOR_TARGET:
            verify_reset(&n->target_verify);
            o->next_state = POOL_STATE_VERIFYING_TARGET;
            o->side_effect = POOL_SIDE_BEGIN_TARGET_VERIFICATION;
            o->changed = true;
            break;
        case POOL_STATE_VERIFYING_TARGET:
        case POOL_STATE_TARGET_ACTIVE:
            /* Must re-verify after any reboot — never assume still active. */
            verify_reset(&n->target_verify);
            o->next_state = POOL_STATE_VERIFYING_TARGET;
            o->side_effect = POOL_SIDE_BEGIN_TARGET_VERIFICATION;
            o->changed = true;
            break;
        case POOL_STATE_RESTARTING_FOR_RESTORE:
        case POOL_STATE_APPLYING_RESTORE:
        case POOL_STATE_VERIFYING_RESTORE:
            verify_reset(&n->restore_verify);
            o->next_state = POOL_STATE_VERIFYING_RESTORE;
            o->side_effect = POOL_SIDE_BEGIN_RESTORE_VERIFICATION;
            o->changed = true;
            break;
        case POOL_STATE_PREPARING:
        case POOL_STATE_APPLYING_TARGET:
            /* Target mutation may be partial/in-flight — reconcile via restore. */
            o->next_state = POOL_STATE_INTERRUPTED;
            o->side_effect = POOL_SIDE_NO_EFFECT;
            o->error = ERR_INTERRUPTED;
            o->changed = true;
            break;
        default:
            /* IDLE, TARGET_SNAPSHOT_COMMITTED, RESTORE_DUE, terminals,
               TARGET_FAILED, INTERRUPTED: safe to remain unchanged. */
            o->next_state = current->state;
            o->side_effect = POOL_SIDE_NO_EFFECT;
            o->changed = false;
            break;
    }
}

PoolSessionOutcome pool_session_transition(const PoolSession *current,
                                           const PoolSessionEvent *event,
                                           PoolSession *out_next)
{
    PoolSessionOutcome o;
    memset(&o, 0, sizeof(o)); /* zero padding too, so outcomes are memcmp-stable */
    o.next_state = current ? current->state : POOL_STATE_IDLE;
    o.side_effect = POOL_SIDE_NO_EFFECT;
    o.error = ERR_NONE;
    o.retry = false;
    o.terminal = false;
    o.changed = false;

    if (current == NULL) {
        o.error = ERR_INVALID_REQUEST;
        return o;
    }

    PoolSession n = *current;  /* working copy; *current is never mutated */

    if (event == NULL) {
        o.error = ERR_INVALID_REQUEST;
        goto finalize;
    }

    /* CREATE needs the request + validation + idempotency. */
    if (event->type == POOL_EVT_CREATE_REQUESTED) {
        handle_create(current, event, &n, &o);
        goto finalize;
    }

    /* Cross-cutting events handled uniformly across states. */
    switch (event->type) {
        case POOL_EVT_RECORD_CORRUPT:
        case POOL_EVT_RECORD_SCHEMA_UNSUPPORTED: {
            /* NEVER a pool mutation; source/target preserved for diagnostics. */
            PoolSessionError ec = (event->type == POOL_EVT_RECORD_CORRUPT)
                                      ? ERR_RECORD_CORRUPT : ERR_SCHEMA_UNSUPPORTED;
            if (current->state == POOL_STATE_IDLE ||
                current->state == POOL_STATE_RECOVERY_REQUIRED) {
                NOOP(ERR_NONE);
            } else {
                GO(POOL_STATE_RECOVERY_REQUIRED, POOL_SIDE_NO_EFFECT, ec);
            }
            goto finalize;
        }
        case POOL_EVT_INTERRUPT_OBSERVED:
            if (current->state == POOL_STATE_IDLE) {
                NOOP(ERR_NO_ACTIVE_SESSION);
            } else if (pool_state_is_terminal(current->state) ||
                       current->state == POOL_STATE_INTERRUPTED) {
                NOOP(ERR_NONE);
            } else {
                GO(POOL_STATE_INTERRUPTED, POOL_SIDE_NO_EFFECT, ERR_INTERRUPTED);
            }
            goto finalize;
        case POOL_EVT_DEVICE_RESTART_OBSERVED:
            handle_device_restart(current, &n, &o);
            goto finalize;
        case POOL_EVT_ACKNOWLEDGE_TERMINAL:
            if (pool_session_is_acknowledgeable_terminal(current)) {
                pool_session_init(&n); /* reset in place to a clean IDLE session */
                o.next_state = POOL_STATE_IDLE;
                o.side_effect = POOL_SIDE_CLEAR_SESSION_RECORD;
                o.error = ERR_NONE;
                o.changed = true;
            } else {
                /* A result state that still owes a restore (restore_required),
                   or not a terminal at all: deterministic recovery conflict. */
                NOOP(ERR_STATE_CONFLICT);
            }
            goto finalize;
        default:
            break; /* fall through to per-state handling */
    }

    switch (current->state) {

    case POOL_STATE_IDLE:
        NOOP(ERR_NO_ACTIVE_SESSION);
        break;

    case POOL_STATE_PREPARING:
        switch (event->type) {
            case POOL_EVT_SOURCE_SNAPSHOT_COMMITTED:
                GO(POOL_STATE_TARGET_SNAPSHOT_COMMITTED,
                   POOL_SIDE_COMMIT_SOURCE_SNAPSHOT, ERR_NONE);
                break;
            case POOL_EVT_CANCEL_REQUESTED:
                n.cancel_requested = true;
                GO(POOL_STATE_CANCELLED, POOL_SIDE_RETAIN_TERMINAL_RESULT, ERR_CANCELLED);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED: /* pre-apply: equivalent to cancel */
                GO(POOL_STATE_CANCELLED, POOL_SIDE_RETAIN_TERMINAL_RESULT, ERR_CANCELLED);
                break;
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE); /* deadline meaningless before TARGET_ACTIVE */
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_TARGET_SNAPSHOT_COMMITTED:
        switch (event->type) {
            case POOL_EVT_TARGET_APPLY_REQUESTED:
                GO(POOL_STATE_APPLYING_TARGET, POOL_SIDE_APPLY_TARGET_CONFIGURATION, ERR_NONE);
                break;
            case POOL_EVT_CANCEL_REQUESTED:
                n.cancel_requested = true;
                GO(POOL_STATE_CANCELLED, POOL_SIDE_RETAIN_TERMINAL_RESULT, ERR_CANCELLED);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED: /* pre-apply: source untouched */
                GO(POOL_STATE_CANCELLED, POOL_SIDE_RETAIN_TERMINAL_RESULT, ERR_CANCELLED);
                break;
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_APPLYING_TARGET:
        switch (event->type) {
            case POOL_EVT_TARGET_RESTART_STARTED:
                GO(POOL_STATE_RESTARTING_FOR_TARGET, POOL_SIDE_RESTART_FOR_TARGET, ERR_NONE);
                break;
            case POOL_EVT_TARGET_APPLY_FAILED:
                if (retry_take(&n.retries.target_apply, POOL_SESSION_MAX_TARGET_APPLY_RETRIES)) {
                    o.next_state = POOL_STATE_APPLYING_TARGET;
                    o.side_effect = POOL_SIDE_APPLY_TARGET_CONFIGURATION;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_TARGET_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_TARGET_APPLY);
                }
                break;
            case POOL_EVT_CANCEL_REQUESTED:       /* after mutation -> restore */
                n.cancel_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_RESTARTING_FOR_TARGET:
        switch (event->type) {
            case POOL_EVT_TARGET_RESTART_COMPLETE:
                verify_reset(&n.target_verify);
                GO(POOL_STATE_VERIFYING_TARGET, POOL_SIDE_BEGIN_TARGET_VERIFICATION, ERR_NONE);
                break;
            case POOL_EVT_TARGET_RETRY_REQUESTED:
                if (retry_take(&n.retries.target_restart, POOL_SESSION_MAX_TARGET_RESTART_RETRIES)) {
                    o.next_state = POOL_STATE_RESTARTING_FOR_TARGET;
                    o.side_effect = POOL_SIDE_RESTART_FOR_TARGET;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_TARGET_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_TARGET_RESTART);
                }
                break;
            case POOL_EVT_CANCEL_REQUESTED:
                n.cancel_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_VERIFYING_TARGET:
        switch (event->type) {
            case POOL_EVT_TARGET_CONNECTION_OBSERVED:
            case POOL_EVT_TARGET_MINING_OBSERVED:
            case POOL_EVT_TARGET_HOST_VERIFIED: {
                bool *flag = (event->type == POOL_EVT_TARGET_CONNECTION_OBSERVED)
                                 ? &n.target_verify.connection_observed
                             : (event->type == POOL_EVT_TARGET_MINING_OBSERVED)
                                 ? &n.target_verify.mining_observed
                                 : &n.target_verify.identity_verified;
                if (!verify_set(flag)) {
                    NOOP(ERR_NONE); /* duplicate observe — idempotent no-op */
                } else if (verify_all(&n.target_verify)) {
                    /* connection + mining + identity all observed -> ACTIVE */
                    n.retries.target_apply = 0;
                    n.retries.target_restart = 0;
                    n.retries.target_verify = 0;
                    GO(POOL_STATE_TARGET_ACTIVE, POOL_SIDE_ARM_MONOTONIC_DEADLINE, ERR_NONE);
                } else {
                    o.next_state = POOL_STATE_VERIFYING_TARGET;
                    o.side_effect = POOL_SIDE_PERSIST_TRANSITION;
                    o.error = ERR_NONE; o.changed = true;
                }
                break;
            }
            case POOL_EVT_TARGET_VERIFY_TIMEOUT:
                if (retry_take(&n.retries.target_verify, POOL_SESSION_MAX_TARGET_VERIFY_RETRIES)) {
                    verify_reset(&n.target_verify);
                    o.next_state = POOL_STATE_APPLYING_TARGET;
                    o.side_effect = POOL_SIDE_APPLY_TARGET_CONFIGURATION;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_TARGET_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_TARGET_VERIFY_TIMEOUT);
                }
                break;
            case POOL_EVT_CANCEL_REQUESTED:
                n.cancel_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_TARGET_ACTIVE:
        switch (event->type) {
            case POOL_EVT_DEADLINE_REACHED:
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_CANCEL_REQUESTED:
                n.cancel_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            case POOL_EVT_TARGET_CONNECTION_OBSERVED:
            case POOL_EVT_TARGET_MINING_OBSERVED:
            case POOL_EVT_TARGET_HOST_VERIFIED:
                NOOP(ERR_NONE); /* already active — duplicate observe is a no-op */
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_RESTORE_DUE:
        switch (event->type) {
            case POOL_EVT_RESTORE_APPLY_REQUESTED:
                verify_reset(&n.restore_verify);
                GO(POOL_STATE_APPLYING_RESTORE, POOL_SIDE_APPLY_SOURCE_CONFIGURATION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED: /* already heading to restore */
            case POOL_EVT_CANCEL_REQUESTED:      /* never abandon restore */
            case POOL_EVT_DEADLINE_REACHED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_APPLYING_RESTORE:
        switch (event->type) {
            case POOL_EVT_RESTORE_RESTART_STARTED:
                GO(POOL_STATE_RESTARTING_FOR_RESTORE, POOL_SIDE_RESTART_FOR_RESTORE, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_APPLY_FAILED:
                if (retry_take(&n.retries.restore_apply, POOL_SESSION_MAX_RESTORE_APPLY_RETRIES)) {
                    o.next_state = POOL_STATE_APPLYING_RESTORE;
                    o.side_effect = POOL_SIDE_APPLY_SOURCE_CONFIGURATION;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_RESTORE_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_RESTORE_APPLY);
                }
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
            case POOL_EVT_CANCEL_REQUESTED:
                NOOP(ERR_NONE); /* restore already in progress */
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_RESTARTING_FOR_RESTORE:
        switch (event->type) {
            case POOL_EVT_RESTORE_RESTART_COMPLETE:
                verify_reset(&n.restore_verify);
                GO(POOL_STATE_VERIFYING_RESTORE, POOL_SIDE_BEGIN_RESTORE_VERIFICATION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_RETRY_REQUESTED:
                if (retry_take(&n.retries.restore_restart, POOL_SESSION_MAX_RESTORE_RESTART_RETRIES)) {
                    o.next_state = POOL_STATE_RESTARTING_FOR_RESTORE;
                    o.side_effect = POOL_SIDE_RESTART_FOR_RESTORE;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_RESTORE_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_RESTORE_RESTART);
                }
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
            case POOL_EVT_CANCEL_REQUESTED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_VERIFYING_RESTORE:
        switch (event->type) {
            case POOL_EVT_RESTORE_CONNECTION_OBSERVED:
            case POOL_EVT_RESTORE_MINING_OBSERVED:
            case POOL_EVT_RESTORE_IDENTITY_VERIFIED: {
                bool *flag = (event->type == POOL_EVT_RESTORE_CONNECTION_OBSERVED)
                                 ? &n.restore_verify.connection_observed
                             : (event->type == POOL_EVT_RESTORE_MINING_OBSERVED)
                                 ? &n.restore_verify.mining_observed
                                 : &n.restore_verify.identity_verified;
                if (!verify_set(flag)) {
                    NOOP(ERR_NONE);
                } else if (verify_all(&n.restore_verify)) {
                    /* connected + mining + source identity -> COMPLETE */
                    GO(POOL_STATE_COMPLETE, POOL_SIDE_RETAIN_TERMINAL_RESULT, ERR_NONE);
                } else {
                    o.next_state = POOL_STATE_VERIFYING_RESTORE;
                    o.side_effect = POOL_SIDE_PERSIST_TRANSITION;
                    o.error = ERR_NONE; o.changed = true;
                }
                break;
            }
            case POOL_EVT_RESTORE_VERIFY_TIMEOUT:
                if (retry_take(&n.retries.restore_verify, POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES)) {
                    verify_reset(&n.restore_verify);
                    o.next_state = POOL_STATE_APPLYING_RESTORE;
                    o.side_effect = POOL_SIDE_APPLY_SOURCE_CONFIGURATION;
                    o.error = ERR_NONE; o.retry = true; o.changed = true;
                } else {
                    GO(POOL_STATE_RESTORE_FAILED, POOL_SIDE_PERSIST_TRANSITION, ERR_RESTORE_VERIFY_TIMEOUT);
                }
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
            case POOL_EVT_CANCEL_REQUESTED:
                NOOP(ERR_NONE);
                break;
            default:
                ILLEGAL();
                break;
        }
        break;

    case POOL_STATE_TARGET_FAILED:  /* non-terminal: must restore the source */
        switch (event->type) {
            case POOL_EVT_RESTORE_APPLY_REQUESTED: /* auto-restore path */
                verify_reset(&n.restore_verify);
                GO(POOL_STATE_APPLYING_RESTORE, POOL_SIDE_APPLY_SOURCE_CONFIGURATION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            default:
                NOOP(ERR_STATE_CONFLICT);
                break;
        }
        break;

    case POOL_STATE_INTERRUPTED:    /* non-terminal: must restore the source */
        switch (event->type) {
            case POOL_EVT_RESTORE_APPLY_REQUESTED:
                verify_reset(&n.restore_verify);
                GO(POOL_STATE_APPLYING_RESTORE, POOL_SIDE_APPLY_SOURCE_CONFIGURATION, ERR_NONE);
                break;
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            default:
                NOOP(ERR_STATE_CONFLICT);
                break;
        }
        break;

    case POOL_STATE_RESTORE_FAILED:     /* terminal (retained until ack) */
    case POOL_STATE_RECOVERY_REQUIRED:  /* terminal (retained until ack) */
        switch (event->type) {
            case POOL_EVT_RESTORE_NOW_REQUESTED:
                /* Operator-initiated re-attempt: reset restore budget (manual,
                   not an automatic loop) and drive restore again. */
                n.retries.restore_apply = 0;
                n.retries.restore_restart = 0;
                n.retries.restore_verify = 0;
                n.restore_requested = true;
                GO(POOL_STATE_RESTORE_DUE, POOL_SIDE_PERSIST_TRANSITION, ERR_NONE);
                break;
            default:
                NOOP(ERR_STATE_CONFLICT);
                break;
        }
        break;

    case POOL_STATE_COMPLETE:
        switch (event->type) {
            case POOL_EVT_RESTORE_NOW_REQUESTED: /* already restored — no effect */
                NOOP(ERR_NONE);
                break;
            default:
                NOOP(ERR_STATE_CONFLICT);
                break;
        }
        break;

    case POOL_STATE_CANCELLED:
        NOOP(ERR_STATE_CONFLICT);
        break;

    default:
        ILLEGAL();
        break;
    }

finalize:
    if (o.changed) {
        n.state = o.next_state;
        n.last_error = o.error;
        /*
         * Monotonic restore obligation. It becomes true the moment an
         * APPLY_TARGET_CONFIGURATION intent is produced (the target pool may be
         * mutated from here on) and is cleared ONLY on reaching COMPLETE
         * (verified source restoration). No failure, cancel, interruption or
         * restart path ever clears it — these are the ONLY two writers.
         */
        if (o.side_effect == POOL_SIDE_APPLY_TARGET_CONFIGURATION) {
            n.restore_required = true;
        }
        if (o.next_state == POOL_STATE_COMPLETE) {
            n.restore_required = false;
        }
        if (pool_state_is_terminal(o.next_state)) {
            n.terminal_result = o.next_state;
        } else if (o.next_state == POOL_STATE_IDLE) {
            n.terminal_result = POOL_STATE_IDLE;
        }
    } else {
        n = *current; /* guarantee byte-identical output on no-op / illegal */
    }
    o.terminal = pool_state_is_terminal(o.next_state);
    if (out_next != NULL) {
        *out_next = n;
    }
    return o;
}
