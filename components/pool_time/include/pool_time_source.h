#ifndef POOL_TIME_SOURCE_H_
#define POOL_TIME_SOURCE_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_time.h"
#include "pool_time_sntp.h"

/*
 * NeuralAxe trusted-time SOURCE policy (Phase 2M.1B, Gate B10).
 * Board 601 / BM1370 only.
 *
 * This header is the PURE trusted-time-source domain: the bounded source
 * validator, the stable source-state vocabulary, the bounded start decision,
 * the bounded start/retry policy and the sanitized diagnostics model.
 * Nothing here touches ESP-IDF, networking, DNS, NVS, FreeRTOS, heap,
 * logging or global mutable state. It never resolves, contacts or even
 * copies a server name.
 *
 * PRODUCT DECISION — TRUSTED-TIME SOURCE (Gate B10, closed):
 *  1. NeuralAxe OS ships with NO implicit public NTP hostname.
 *  2. CONFIG_NX_TIMED_SESSIONS_NTP_SERVER is an explicit ADMINISTRATOR-BUILD
 *     configuration whose safe default is the empty string.
 *  3. Empty or invalid configuration means trusted time is UNAVAILABLE. No
 *     fake trust is invented; any session state that requires trusted time
 *     stays protocol-held; observation mode reports TIME_SOURCE_UNCONFIGURED.
 *  4. The hostname is NOT configurable through the unauthenticated LAN API.
 *  5. DHCP-provided NTP is NOT automatically trusted or enabled
 *     (CONFIG_LWIP_DHCP_GET_NTP_SRV is off and the B2 adapter rejects the
 *     request when it is not compiled in).
 *  6. For a supervised hardware pilot the owner explicitly compiles ONE
 *     chosen source into the pilot build.
 *  7. The selected hostname never appears in a snapshot, a dashboard
 *     response, a diagnostic token or a routine log line. No model in this
 *     header has a string field, so it cannot.
 *  8. Standard SNTP and DNS are OPERATIONAL trust mechanisms, not
 *     cryptographic authentication (no NTS, no DNSSEC). See the
 *     NETWORK-TRUST RESIDUAL section of pool_time.h.
 *  9. Weather-Aware Tuning will later consume THIS foundation; it must not
 *     create a second time provider.
 *
 * SECURITY INVARIANT: no password, pool account, wallet, hostname, address,
 * session identifier, lease token or record generation exists anywhere in
 * this model, and no machine-readable token ever carries one.
 */

#define POOL_TIME_SOURCE_MODEL_VERSION 1u

/*
 * Bounded hostname limits. The character bound is one below the committed B2
 * config buffer so any accepted source always fits it with room for the NUL.
 */
#define POOL_TIME_SOURCE_HOST_MAX   63u /* characters, excluding the NUL */
#define POOL_TIME_SOURCE_LABEL_MAX  63u /* characters in one DNS label   */
#define POOL_TIME_SOURCE_LABELS_MAX 16u /* bounded label count           */

/*
 * Bounded start/retry policy. The first attempt is immediate; every retry
 * waits a monotonic backoff, and the attempt count is hard-bounded so no
 * unlimited retry or busy loop can exist.
 */
#define POOL_TIME_SOURCE_ATTEMPTS_MAX      5u
#define POOL_TIME_SOURCE_BACKOFF_BASE_S   15u
#define POOL_TIME_SOURCE_BACKOFF_MAX_S   120u

/* ------------------------------------------------------------------ */
/* Stable source states                                                */
/* ------------------------------------------------------------------ */

/*
 * The public trusted-time source vocabulary. Stable machine values; any
 * value outside the enum is treated as unknown by every total helper below
 * and never as a permission.
 */
typedef enum {
    TIME_SOURCE_UNCONFIGURED = 0, /* no source compiled in (the shipped default) */
    TIME_SOURCE_CONFIGURED,       /* a valid source exists; service not running  */
    TIME_SOURCE_INVALID,          /* a source exists but fails validation        */
    TIME_SOURCE_START_PENDING,    /* start is owed (network not ready / queued)  */
    TIME_SOURCE_SYNCING,          /* service started; no accepted anchor yet     */
    TIME_SOURCE_TRUSTED,          /* an anchor was accepted this boot            */
    TIME_SOURCE_REJECTED,         /* the last candidate was refused by B2        */
    TIME_SOURCE_TIMEOUT,          /* bounded wait or attempt budget exhausted    */
    TIME_SOURCE_STOPPED,          /* the service ran and was stopped             */
    TIME_SOURCE_ERROR,            /* a platform op failed deterministically      */
    POOL_TIME_SOURCE_STATE__COUNT
} PoolTimeSourceState;

/* ------------------------------------------------------------------ */
/* Bounded source validation                                           */
/* ------------------------------------------------------------------ */

/*
 * The validation verdict. It deliberately carries NO copy of the candidate:
 * only its shape. `usable` is true exactly for TIME_SOURCE_CONFIGURED.
 * Every field is zeroed on any non-CONFIGURED verdict.
 */
typedef struct {
    PoolTimeSourceState state;
    bool                usable;
    uint32_t            length;      /* characters; 0 unless CONFIGURED  */
    uint32_t            label_count; /* DNS labels; 0 unless CONFIGURED  */
    bool                literal_ipv4;
    PoolTimeError       reason;      /* TIME_OK for UNCONFIGURED/CONFIGURED */
} PoolTimeSourceValidation;

/*
 * Validate a candidate trusted-time source. PURE and bounded: it reads at
 * most POOL_TIME_SOURCE_HOST_MAX + 1 bytes and never copies, resolves,
 * contacts or logs the candidate.
 *
 * PRECONDITION: `host`, when non-NULL, is NUL-terminated within
 * POOL_TIME_SOURCE_HOST_MAX + 1 bytes. A candidate without a terminator in
 * that window is rejected as INVALID rather than read further.
 *
 * ACCEPTED (TIME_SOURCE_CONFIGURED):
 *  - a bounded DNS hostname of 2..POOL_TIME_SOURCE_LABELS_MAX labels drawn
 *    only from [A-Za-z0-9.-], each label 1..63 characters, no leading or
 *    trailing hyphen in a label, no empty label, and a non-numeric last
 *    label; or
 *  - a bounded dotted-quad IPv4 literal: exactly four all-digit labels, each
 *    1..3 digits with NO leading zero (so lwIP's octal/hex ipaddr_aton()
 *    forms can never be reached) and a value of 0..255.
 *
 * UNCONFIGURED (not an error — the shipped default):
 *  - NULL or the empty string.
 *
 * REJECTED (TIME_SOURCE_INVALID, reason TIME_ERR_INVALID_SERVER_CONFIG):
 *  - anything longer than the bound or unterminated in the window;
 *  - leading, trailing or embedded whitespace;
 *  - control characters or any non-ASCII byte;
 *  - URL schemes, paths, queries, fragments, userinfo or credentials (every
 *    one of ':' '/' '?' '#' '@' '%' is outside the accepted character set);
 *  - empty, overlong, leading-hyphen or trailing-hyphen labels;
 *  - a leading dot, a trailing dot or consecutive dots;
 *  - a single label with no dot at all (search-domain resolution behaviour
 *    is not audited, so it is not accepted);
 *  - a malformed or ambiguous numeric literal;
 *  - IPv6 literals in every form. lwIP's SNTP IPv6 path and the bracketed
 *    URI form are NOT audited by this gate, and ':' is outside the accepted
 *    character set, so they are rejected deliberately rather than silently
 *    half-supported.
 *
 * Returns out->state for convenience; TIME_SOURCE_INVALID when `out` is NULL.
 */
PoolTimeSourceState pool_time_source_validate(const char *host,
                                              PoolTimeSourceValidation *out);

/* ------------------------------------------------------------------ */
/* Bounded start decision                                              */
/* ------------------------------------------------------------------ */

/*
 * Everything the pure start rule may know. No hostname, no record, no lease,
 * no pointer to mutable state.
 */
typedef struct {
    bool runtime_enabled;      /* CONFIG_NX_TIMED_SESSIONS                  */
    bool observe_enabled;      /* CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE     */
    bool network_ready;        /* the single bounded network-ready fact     */
    bool trusted_time_required;/* the B4 plan demands fresh trust           */
    bool source_present;       /* a non-empty source string was configured  */
    bool source_usable;        /* the validator returned CONFIGURED         */
    bool provider_started;     /* the B2 provider is already running        */
    bool session_owner_present;/* a session record or recovery owner exists */
} PoolTimeSourceStartInput;

/*
 * The start verdict. `state` is the diagnostic posture implied by the
 * decision ALONE; when a provider exists the authoritative state comes from
 * pool_time_source_diagnostics_build() below, which also sees the provider
 * lifecycle and the accepted anchor.
 */
typedef struct {
    bool                start_provider;
    bool                observation_only; /* started purely to observe time */
    PoolTimeSourceState state;
    PoolTimeError       reason;
} PoolTimeSourceStartDecision;

/*
 * THE pure start rule. Total, deterministic and fail-closed: it returns
 * start_provider=true ONLY when a validated source exists, the network is
 * ready, no provider is running yet and either the B4 plan requires trusted
 * time or observation mode is enabled on a device with no session owner.
 *
 * OBSERVATION IS NOT AUTHORIZATION. observation_only=true means the start is
 * for measurement alone: it grants no target mining, no pool mutation, no
 * lease, no persistence and no protocol change anywhere in the system.
 */
PoolTimeSourceStartDecision pool_time_source_decide_start(
    const PoolTimeSourceStartInput *in);

/* ------------------------------------------------------------------ */
/* Bounded start/retry policy                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t max_attempts;    /* hard bound on provider start attempts */
    uint32_t base_backoff_s;  /* delay before the first retry          */
    uint32_t max_backoff_s;   /* ceiling on the doubling backoff       */
} PoolTimeSourceRetryPolicy;

/* Fill the committed bounded defaults. */
void pool_time_source_retry_defaults(PoolTimeSourceRetryPolicy *p);

/* Structural validity: non-zero, ordered, and within the compiled bounds. */
bool pool_time_source_retry_policy_valid(const PoolTimeSourceRetryPolicy *p);

typedef struct {
    bool     may_attempt;  /* start (or retry) right now             */
    bool     exhausted;    /* the attempt budget is spent; never again */
    uint32_t next_delay_s; /* remaining backoff; 0 when may_attempt   */
    uint32_t attempt_index;/* 1-based index this call would consume   */
} PoolTimeSourceRetryDecision;

/*
 * The pure bounded retry rule. The FIRST attempt is always immediate; every
 * later attempt waits base<<(n-1) seconds (capped at max_backoff_s) measured
 * ONLY on the monotonic clock, and no more than max_attempts ever happen.
 * A monotonic regression fails safe by waiting the full backoff again rather
 * than retrying early. There is no tight-poll path and no unbounded retry.
 */
PoolTimeSourceRetryDecision pool_time_source_retry_decide(
    const PoolTimeSourceRetryPolicy *p, uint32_t attempts_made,
    bool any_attempt_made, uint64_t last_attempt_monotonic_us,
    uint64_t monotonic_now_us);

/* ------------------------------------------------------------------ */
/* Sanitized diagnostics                                               */
/* ------------------------------------------------------------------ */

/*
 * The bounded public trusted-time diagnostic view.
 *
 * PRIVACY BY CONSTRUCTION: this struct has NO string field, so it cannot
 * carry a hostname, a resolved IP, DNS error text, a server index, a raw
 * SNTP status, a pool identity, a session id, a lease token or a record
 * generation. It also carries NO wall-clock epoch — only a monotonic AGE in
 * seconds — so no raw system time can leak through it either.
 */
typedef struct {
    uint32_t            model_version;
    bool                source_configured;        /* a VALID source exists   */
    bool                observation_mode_enabled;
    PoolTimeSourceState state;
    bool                trusted_time_available;   /* B2 says trusted now     */
    bool                trusted_time_operational; /* available AND healthy   */
    uint32_t            sync_attempt_count;       /* bounded start attempts  */
    bool                sync_age_valid;
    uint32_t            sync_age_s;               /* monotonic anchor age    */
    PoolTimeError       last_sync_result;
    uint32_t            wait_elapsed_s;
    uint32_t            wait_limit_s;
} PoolTimeSourceDiagnostics;

/* Everything the diagnostics builder may know. Scalars and flags only. */
typedef struct {
    bool          runtime_enabled;
    bool          observe_enabled;
    bool          source_present;
    bool          source_usable;
    bool          provider_initialized;
    bool          provider_started;
    uint8_t       lifecycle;         /* PoolTimeSntpLifecycle value */
    bool          snapshot_trusted;
    uint64_t      anchor_age_us;
    bool          anchor_age_valid;
    uint32_t      attempts;
    bool          attempts_exhausted;
    bool          wait_expired;
    uint32_t      wait_elapsed_s;
    uint32_t      wait_limit_s;
    PoolTimeError last_sync_result;
} PoolTimeSourceDiagnosticsInput;

/* Zero a diagnostics view to the fail-closed "nothing is known" posture. */
void pool_time_source_diagnostics_init(PoolTimeSourceDiagnostics *out);

/*
 * THE pure diagnostics build. Total, deterministic, input-immutable; *out is
 * fully written on every path. A NULL input yields the fail-closed view.
 *
 * STATE PRIORITY (highest first): no runtime -> UNCONFIGURED; no source ->
 * UNCONFIGURED; unusable source -> INVALID; platform error -> ERROR; an
 * accepted anchor -> TRUSTED; stopped service -> STOPPED; a refused
 * candidate -> REJECTED; expired wait or spent attempt budget -> TIMEOUT;
 * running service -> SYNCING; initialized service -> START_PENDING;
 * otherwise -> CONFIGURED.
 *
 * An accepted anchor keeps trusted_time_available true even in ERROR, which
 * is why `trusted_time_operational` (available AND state == TRUSTED) exists
 * separately: it never overstates the health of the service.
 */
void pool_time_source_diagnostics_build(const PoolTimeSourceDiagnosticsInput *in,
                                        PoolTimeSourceDiagnostics *out);

/*
 * Structural consistency of a published diagnostics view: model version,
 * state in range, and the invariants that an unconfigured source is never
 * TRUSTED, that operational implies available, and that an unavailable
 * clock is never reported operational.
 */
bool pool_time_source_diagnostics_valid(const PoolTimeSourceDiagnostics *d);

/* Stable machine token (dot-free; never carries a hostname or prose). */
const char *pool_time_source_state_str(PoolTimeSourceState s);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_TIME_SOURCE_HOST_MAX + 1u == (uint32_t)POOL_TIME_SNTP_SERVER_HOST_MAX,
               "an accepted source must always fit the B2 config buffer");
_Static_assert(POOL_TIME_SOURCE_LABEL_MAX <= POOL_TIME_SOURCE_HOST_MAX,
               "a label cannot exceed the whole hostname bound");
_Static_assert(POOL_TIME_SOURCE_STATE__COUNT == 10,
               "source state count changed — review tables/tokens/tests");
_Static_assert(TIME_SOURCE_UNCONFIGURED == 0,
               "UNCONFIGURED must be zero so a zeroed verdict trusts nothing");
_Static_assert(POOL_TIME_SOURCE_ATTEMPTS_MAX >= 1u && POOL_TIME_SOURCE_ATTEMPTS_MAX <= 16u,
               "start attempt budget out of range");
_Static_assert(POOL_TIME_SOURCE_BACKOFF_BASE_S <= POOL_TIME_SOURCE_BACKOFF_MAX_S,
               "backoff base exceeds its ceiling");
_Static_assert(POOL_TIME_SOURCE_BACKOFF_MAX_S <= POOL_TIME_SYNC_WAIT_MAX_S,
               "a single backoff must never exceed the bounded sync window");

#endif /* POOL_TIME_SOURCE_H_ */
