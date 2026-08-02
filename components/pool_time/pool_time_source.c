/*
 * NeuralAxe trusted-time SOURCE policy — pure implementation (Gate B10).
 *
 * No ESP-IDF, no networking, no DNS, no NVS, no FreeRTOS, no heap, no
 * logging, no global mutable state and no clock read. Every function is a
 * total function of its arguments, so identical inputs always produce
 * byte-identical output.
 *
 * The candidate source string is READ but never copied, resolved, contacted
 * or logged, and no model in this file has a string field to carry it.
 */

#include <string.h>

#include "pool_time_source.h"

/* ------------------------------------------------------------------ */
/* Bounded source validation                                           */
/* ------------------------------------------------------------------ */

static bool src_is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool src_is_label_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || src_is_digit(c) || c == '-';
}

/* Reject with a zeroed verdict: no partial shape survives a failure. */
static PoolTimeSourceState src_reject(PoolTimeSourceValidation *out)
{
    memset(out, 0, sizeof(*out));
    out->state  = TIME_SOURCE_INVALID;
    out->reason = TIME_ERR_INVALID_SERVER_CONFIG;
    return out->state;
}

static PoolTimeSourceState src_unconfigured(PoolTimeSourceValidation *out)
{
    memset(out, 0, sizeof(*out));
    out->state  = TIME_SOURCE_UNCONFIGURED;
    out->reason = TIME_OK;
    return out->state;
}

/*
 * One dotted-quad octet: 1..3 digits, no leading zero unless the octet is
 * exactly "0", value 0..255. Rejecting the leading zero is deliberate —
 * lwIP's ipaddr_aton() accepts octal ("010") and hex ("0x0A") forms, so an
 * ambiguous literal could otherwise resolve to a different address than the
 * one an operator read.
 */
static bool src_octet_valid(const char *s, uint32_t len)
{
    uint32_t v = 0u;
    uint32_t i;

    if (len == 0u || len > 3u) {
        return false;
    }
    if (len > 1u && s[0] == '0') {
        return false;
    }
    for (i = 0; i < len; i++) {
        if (!src_is_digit((unsigned char)s[i])) {
            return false;
        }
        v = (v * 10u) + (uint32_t)(s[i] - '0');
    }
    return v <= 255u;
}

PoolTimeSourceState pool_time_source_validate(const char *host,
                                              PoolTimeSourceValidation *out)
{
    uint32_t len = 0u;
    uint32_t i;
    uint32_t label_start = 0u;
    uint32_t label_count = 0u;
    uint32_t numeric_labels = 0u;
    bool     last_label_numeric = false;
    bool     terminated = false;

    if (out == NULL) {
        return TIME_SOURCE_INVALID;
    }
    if (host == NULL) {
        return src_unconfigured(out);
    }

    /* Bounded scan: at most POOL_TIME_SOURCE_HOST_MAX + 1 bytes are read. */
    for (i = 0; i <= POOL_TIME_SOURCE_HOST_MAX; i++) {
        if (host[i] == '\0') {
            len        = i;
            terminated = true;
            break;
        }
    }
    if (!terminated) {
        return src_reject(out); /* overlong or unterminated in the window */
    }
    if (len == 0u) {
        return src_unconfigured(out);
    }

    /*
     * Character set: [A-Za-z0-9.-] only. This single rule rejects every URL
     * scheme, path, query, fragment, userinfo and credential form (':' '/'
     * '?' '#' '@' '%' are all outside it), every whitespace character
     * including leading and trailing space, every control character, every
     * non-ASCII byte and every IPv6 literal.
     */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c != '.' && !src_is_label_char(c)) {
            return src_reject(out);
        }
    }

    /* Labels: 1..63 characters, no empty label, no leading/trailing hyphen. */
    for (i = 0; i <= len; i++) {
        if (i == len || host[i] == '.') {
            uint32_t label_len = i - label_start;
            uint32_t j;
            bool     all_digits;

            if (label_len == 0u || label_len > POOL_TIME_SOURCE_LABEL_MAX) {
                return src_reject(out); /* empty / leading / trailing dot too */
            }
            if (host[label_start] == '-' || host[i - 1u] == '-') {
                return src_reject(out);
            }
            if (label_count >= POOL_TIME_SOURCE_LABELS_MAX) {
                return src_reject(out);
            }
            all_digits = true;
            for (j = label_start; j < i; j++) {
                if (!src_is_digit((unsigned char)host[j])) {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits) {
                numeric_labels++;
            }
            last_label_numeric = all_digits;
            label_count++;
            label_start = i + 1u;
        }
    }

    if (label_count < 2u) {
        /* A bare single label depends on unaudited DNS search-domain
         * behaviour, so it is never accepted as a product source. */
        return src_reject(out);
    }

    if (numeric_labels == label_count) {
        /* An all-numeric name is only meaningful as a dotted-quad literal. */
        uint32_t start = 0u;
        if (label_count != 4u) {
            return src_reject(out);
        }
        for (i = 0; i <= len; i++) {
            if (i == len || host[i] == '.') {
                if (!src_octet_valid(&host[start], i - start)) {
                    return src_reject(out);
                }
                start = i + 1u;
            }
        }
        memset(out, 0, sizeof(*out));
        out->state        = TIME_SOURCE_CONFIGURED;
        out->usable       = true;
        out->length       = len;
        out->label_count  = label_count;
        out->literal_ipv4 = true;
        out->reason       = TIME_OK;
        return out->state;
    }

    if (last_label_numeric) {
        /* A partly-numeric name whose last label is all digits is neither a
         * valid literal nor a resolvable host. */
        return src_reject(out);
    }

    memset(out, 0, sizeof(*out));
    out->state        = TIME_SOURCE_CONFIGURED;
    out->usable       = true;
    out->length       = len;
    out->label_count  = label_count;
    out->literal_ipv4 = false;
    out->reason       = TIME_OK;
    return out->state;
}

/* ------------------------------------------------------------------ */
/* Bounded start decision                                              */
/* ------------------------------------------------------------------ */

static PoolTimeSourceStartDecision src_no_start(PoolTimeSourceState state,
                                                PoolTimeError reason)
{
    PoolTimeSourceStartDecision d;
    memset(&d, 0, sizeof(d));
    d.start_provider   = false;
    d.observation_only = false;
    d.state            = state;
    d.reason           = reason;
    return d;
}

PoolTimeSourceStartDecision pool_time_source_decide_start(
    const PoolTimeSourceStartInput *in)
{
    PoolTimeSourceStartDecision d;

    if (in == NULL) {
        return src_no_start(TIME_SOURCE_UNCONFIGURED, TIME_ERR_INVALID_ARGUMENT);
    }

    /* 1. Without the runtime feature no time subsystem exists at all. */
    if (!in->runtime_enabled) {
        return src_no_start(TIME_SOURCE_UNCONFIGURED, TIME_ERR_NOT_INITIALIZED);
    }

    /* 2/3. Source configuration is decided BEFORE any need or network fact,
     * so an unconfigured or malformed source can never start networking. */
    if (!in->source_present) {
        return src_no_start(TIME_SOURCE_UNCONFIGURED, TIME_OK);
    }
    if (!in->source_usable) {
        return src_no_start(TIME_SOURCE_INVALID, TIME_ERR_INVALID_SERVER_CONFIG);
    }

    /* 4. Nothing needs trusted time: the source stays validated but idle. */
    if (!in->trusted_time_required && !in->observe_enabled) {
        return src_no_start(TIME_SOURCE_CONFIGURED, TIME_OK);
    }

    /*
     * 5. Observation mode is for devices with NO session owner. A record or
     * recovery owner means the B4 plan — not observation — decides whether
     * time is needed, so observation never starts alongside one.
     */
    if (!in->trusted_time_required && in->session_owner_present) {
        return src_no_start(TIME_SOURCE_CONFIGURED, TIME_OK);
    }

    /* 6. Provider startup happens ONLY after network readiness. */
    if (!in->network_ready) {
        return src_no_start(TIME_SOURCE_START_PENDING, TIME_ERR_SYNC_PENDING);
    }

    /* 7. At most one start per provider lifecycle: duplicate events are
     * idempotent and never create a second SNTP client. */
    if (in->provider_started) {
        d = src_no_start(TIME_SOURCE_SYNCING, TIME_OK);
        d.observation_only = !in->trusted_time_required;
        return d;
    }

    /* 8. Start. Observation-only when no B4 plan demanded the trust. */
    memset(&d, 0, sizeof(d));
    d.start_provider   = true;
    d.observation_only = !in->trusted_time_required;
    d.state            = TIME_SOURCE_START_PENDING;
    d.reason           = TIME_OK;
    return d;
}

/* ------------------------------------------------------------------ */
/* Bounded start/retry policy                                          */
/* ------------------------------------------------------------------ */

void pool_time_source_retry_defaults(PoolTimeSourceRetryPolicy *p)
{
    if (p == NULL) {
        return;
    }
    p->max_attempts   = POOL_TIME_SOURCE_ATTEMPTS_MAX;
    p->base_backoff_s = POOL_TIME_SOURCE_BACKOFF_BASE_S;
    p->max_backoff_s  = POOL_TIME_SOURCE_BACKOFF_MAX_S;
}

bool pool_time_source_retry_policy_valid(const PoolTimeSourceRetryPolicy *p)
{
    if (p == NULL) {
        return false;
    }
    if (p->max_attempts == 0u || p->max_attempts > POOL_TIME_SOURCE_ATTEMPTS_MAX) {
        return false;
    }
    if (p->base_backoff_s == 0u || p->base_backoff_s > p->max_backoff_s) {
        return false;
    }
    return p->max_backoff_s <= POOL_TIME_SOURCE_BACKOFF_MAX_S;
}

/* base << min(shift, 3), saturating at the ceiling; overflow-free. */
static uint32_t src_backoff_for(const PoolTimeSourceRetryPolicy *p, uint32_t attempts_made)
{
    uint32_t shift = (attempts_made > 0u) ? (attempts_made - 1u) : 0u;
    uint32_t delay = p->base_backoff_s;
    uint32_t i;

    if (shift > 3u) {
        shift = 3u;
    }
    for (i = 0; i < shift; i++) {
        if (delay > (p->max_backoff_s >> 1)) {
            return p->max_backoff_s;
        }
        delay <<= 1;
    }
    return (delay > p->max_backoff_s) ? p->max_backoff_s : delay;
}

PoolTimeSourceRetryDecision pool_time_source_retry_decide(
    const PoolTimeSourceRetryPolicy *p, uint32_t attempts_made,
    bool any_attempt_made, uint64_t last_attempt_monotonic_us,
    uint64_t monotonic_now_us)
{
    PoolTimeSourceRetryDecision d;
    uint32_t delay_s;
    uint64_t elapsed_us;
    uint64_t needed_us;

    memset(&d, 0, sizeof(d));

    if (!pool_time_source_retry_policy_valid(p)) {
        d.exhausted = true; /* fail closed: an invalid policy never retries */
        return d;
    }
    if (attempts_made >= p->max_attempts) {
        d.exhausted     = true;
        d.attempt_index = attempts_made;
        return d;
    }

    d.attempt_index = attempts_made + 1u;

    if (!any_attempt_made || attempts_made == 0u) {
        d.may_attempt = true; /* the first attempt is always immediate */
        return d;
    }

    delay_s   = src_backoff_for(p, attempts_made);
    needed_us = (uint64_t)delay_s * POOL_TIME_US_PER_S;

    if (monotonic_now_us < last_attempt_monotonic_us) {
        /* Monotonic regression: fail safe by waiting the full backoff again
         * rather than treating the anomaly as elapsed time. */
        d.next_delay_s = delay_s;
        return d;
    }
    elapsed_us = monotonic_now_us - last_attempt_monotonic_us;
    if (elapsed_us >= needed_us) {
        d.may_attempt = true;
        return d;
    }
    d.next_delay_s = (uint32_t)(((needed_us - elapsed_us) + (POOL_TIME_US_PER_S - 1ull)) /
                                POOL_TIME_US_PER_S);
    return d;
}

/* ------------------------------------------------------------------ */
/* Sanitized diagnostics                                               */
/* ------------------------------------------------------------------ */

void pool_time_source_diagnostics_init(PoolTimeSourceDiagnostics *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->model_version    = POOL_TIME_SOURCE_MODEL_VERSION;
    out->state            = TIME_SOURCE_UNCONFIGURED;
    out->last_sync_result = TIME_ERR_NOT_INITIALIZED;
}

/* A candidate that B2 refused for a TRUST reason (as opposed to a service or
 * argument error) — the only codes that justify reporting REJECTED. */
static bool src_result_is_rejection(PoolTimeError e)
{
    switch (e) {
    case TIME_ERR_EPOCH_BELOW_MIN:
    case TIME_ERR_EPOCH_ABOVE_MAX:
    case TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN:
    case TIME_ERR_MONOTONIC_REGRESSION:
    case TIME_ERR_TRUST_REGRESSION:
    case TIME_ERR_OVERFLOW:
        return true;
    default:
        return false;
    }
}

static PoolTimeSourceState src_state_for(const PoolTimeSourceDiagnosticsInput *in)
{
    if (!in->runtime_enabled || !in->source_present) {
        return TIME_SOURCE_UNCONFIGURED;
    }
    if (!in->source_usable) {
        return TIME_SOURCE_INVALID;
    }
    if (in->lifecycle == (uint8_t)POOL_TIME_SNTP_ERROR) {
        return TIME_SOURCE_ERROR;
    }
    if (in->snapshot_trusted) {
        return TIME_SOURCE_TRUSTED;
    }
    if (in->lifecycle == (uint8_t)POOL_TIME_SNTP_STOPPED) {
        return TIME_SOURCE_STOPPED;
    }
    if (src_result_is_rejection(in->last_sync_result)) {
        return TIME_SOURCE_REJECTED;
    }
    if (in->wait_expired || in->attempts_exhausted) {
        return TIME_SOURCE_TIMEOUT;
    }
    if (in->provider_started) {
        return TIME_SOURCE_SYNCING;
    }
    if (in->provider_initialized) {
        return TIME_SOURCE_START_PENDING;
    }
    return TIME_SOURCE_CONFIGURED;
}

void pool_time_source_diagnostics_build(const PoolTimeSourceDiagnosticsInput *in,
                                        PoolTimeSourceDiagnostics *out)
{
    uint64_t age_s;

    if (out == NULL) {
        return;
    }
    pool_time_source_diagnostics_init(out);
    if (in == NULL) {
        return;
    }

    out->source_configured        = in->runtime_enabled && in->source_present &&
                                    in->source_usable;
    out->observation_mode_enabled = in->runtime_enabled && in->observe_enabled;
    out->state                    = src_state_for(in);
    out->trusted_time_available   = in->snapshot_trusted && out->source_configured;
    out->trusted_time_operational = out->trusted_time_available &&
                                    out->state == TIME_SOURCE_TRUSTED;
    out->sync_attempt_count       = in->attempts;
    out->wait_elapsed_s           = in->wait_elapsed_s;
    out->wait_limit_s             = in->wait_limit_s;

    out->last_sync_result = ((unsigned)in->last_sync_result < (unsigned)POOL_TIME_ERR__COUNT)
                                ? in->last_sync_result
                                : TIME_ERR_NOT_INITIALIZED;

    /* A monotonic AGE, never an epoch: saturating, so no wrap can shorten it. */
    if (in->anchor_age_valid && in->snapshot_trusted) {
        age_s              = in->anchor_age_us / POOL_TIME_US_PER_S;
        out->sync_age_valid = true;
        out->sync_age_s     = (age_s > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)age_s;
    }
}

bool pool_time_source_diagnostics_valid(const PoolTimeSourceDiagnostics *d)
{
    if (d == NULL || d->model_version != POOL_TIME_SOURCE_MODEL_VERSION) {
        return false;
    }
    if ((unsigned)d->state >= (unsigned)POOL_TIME_SOURCE_STATE__COUNT) {
        return false;
    }
    if ((unsigned)d->last_sync_result >= (unsigned)POOL_TIME_ERR__COUNT) {
        return false;
    }
    if (d->trusted_time_operational && !d->trusted_time_available) {
        return false;
    }
    if (d->trusted_time_available && !d->source_configured) {
        return false;
    }
    if (d->state == TIME_SOURCE_TRUSTED && !d->trusted_time_available) {
        return false;
    }
    if (!d->source_configured &&
        (d->state == TIME_SOURCE_TRUSTED || d->state == TIME_SOURCE_SYNCING)) {
        return false;
    }
    if (!d->sync_age_valid && d->sync_age_s != 0u) {
        return false;
    }
    return d->sync_attempt_count <= POOL_TIME_SOURCE_ATTEMPTS_MAX;
}

const char *pool_time_source_state_str(PoolTimeSourceState s)
{
    switch (s) {
    case TIME_SOURCE_UNCONFIGURED:  return "TIME_SOURCE_UNCONFIGURED";
    case TIME_SOURCE_CONFIGURED:    return "TIME_SOURCE_CONFIGURED";
    case TIME_SOURCE_INVALID:       return "TIME_SOURCE_INVALID";
    case TIME_SOURCE_START_PENDING: return "TIME_SOURCE_START_PENDING";
    case TIME_SOURCE_SYNCING:       return "TIME_SOURCE_SYNCING";
    case TIME_SOURCE_TRUSTED:       return "TIME_SOURCE_TRUSTED";
    case TIME_SOURCE_REJECTED:      return "TIME_SOURCE_REJECTED";
    case TIME_SOURCE_TIMEOUT:       return "TIME_SOURCE_TIMEOUT";
    case TIME_SOURCE_STOPPED:       return "TIME_SOURCE_STOPPED";
    case TIME_SOURCE_ERROR:         return "TIME_SOURCE_ERROR";
    default:                        return "TIME_SOURCE_UNKNOWN";
    }
}
