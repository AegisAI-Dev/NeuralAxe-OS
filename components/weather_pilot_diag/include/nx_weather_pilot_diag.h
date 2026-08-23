#ifndef NX_WEATHER_PILOT_DIAG_H_
#define NX_WEATHER_PILOT_DIAG_H_

#include <stdint.h>
#include <stdbool.h>

#include "nx_weather_source.h"   /* NxWeatherSourceStatus, config model */
#include "weather_runtime.h"     /* WeatherRuntimeState, recommendation */
#include "pool_time_source.h"    /* Gate B10 sanitized trusted-time model */
#include "nx_weather_io.h"       /* Gate W6.3 bounded async I/O model    */
#include "nx_weather_window_runtime.h" /* Gate W6.4 bounded window model  */

/*
 * NeuralAxe Weather-Aware Tuning — RECOMMENDATION-ONLY PILOT DIAGNOSTICS
 * (Phase 2W, Gate W6). Board 601 / BM1370 only.
 *
 * W6 prepares a SUPERVISED PHYSICAL PILOT. Gate W6.2 wired the first half of
 * it: the device obtains trusted time from the committed B2/B10 provider and
 * evaluates the committed Brussels schedule. The rest is still ahead —
 * retrieve ONE forecast from the explicitly configured W3 provider, evaluate
 * the committed W1 policy, emit a RECOMMENDATION — and throughout, change
 * nothing.
 *
 * WHAT W6 ITSELF DOES, EXACTLY. The pilot steps the W4 runtime with a live
 * BORROWED clock, and with NO transport and NO store. That is what keeps it
 * inert: it can read trusted time and evaluate the schedule, and it holds no
 * seam through which a fetch, a persisted write or a hardware apply could
 * occur.
 *
 * THE CLOCK IS BORROWED, NEVER OWNED. When CONFIG_NX_TIMED_SESSIONS is linked
 * and the bind succeeds, Gate W6.2 lends the pilot the SAME live PoolTimeClock
 * and PoolTimeTrustPolicy objects the committed B2/B10 runtime already owns
 * (pool_session_runtime_clock() / _trust_policy()). Lending a pointer is what
 * makes the next four STRUCTURAL rather than promised — there is no API here
 * through which any of them could be constructed, copied or owned:
 *   - no second SNTP provider;
 *   - no second trusted-time anchor;
 *   - no second accepted-epoch floor;
 *   - no independent wall-clock trust source.
 * So trusted(weather) implies trusted(B2/B10): the only anchor reachable is
 * the one B2 accepted, judged by B2's own policy.
 *
 * IN THE BOUND POSTURE THE READINGS ARE LIVE. `trusted_time_available` comes
 * from the runtime recommendation the pilot just stepped against the borrowed
 * anchor, and the time_* fields below are a read-only PROJECTION of that same
 * B2/B10 authority. Both are measurements, and both can change during a boot.
 * They are still two distinct facts — one is the runtime's position, the other
 * the provider's lifecycle — so do not read them as one.
 *
 * WHEN THE AUTHORITY IS STRUCTURALLY ABSENT, SAY SO. Without
 * CONFIG_NX_TIMED_SESSIONS no runtime instance exists, the clock acquisition
 * is compiled out and the bind can never succeed; the pilot then holds the
 * bounded WAITING_FOR_TRUSTED_TIME / NO_TRUSTED_TIME position for the whole
 * boot and the projection stamps time_fact = STRUCTURAL. A runtime that is
 * linked but publishes nothing quotable is stamped UNAVAILABLE. Neither
 * posture is a live trusted-time measurement, and neither is reported as a
 * reassuring zero — so read time_fact before trusting any time_* value.
 *
 * STILL NOT INJECTED, DELIBERATELY: no production W3 transport (so no fetch is
 * possible), no weather persistence or store seam, and no hardware execution
 * path. Each is later, separately gated work.
 *
 * This module is the pilot's EYES, never its hands. It:
 *   - reads snapshots and counters, and mutates nothing;
 *   - creates no task, timer, queue or HTTP endpoint;
 *   - performs no NVS write and no persistence of any kind;
 *   - emits bounded machine tokens only — never a coordinate, city, timezone,
 *     hostname, URL, forecast body, NTP server, pool identity or session id;
 *   - reports an invariant violation but never recovers, restarts, clears
 *     state or authorizes anything.
 *
 * Everything here is PURE and deterministic. The only impure part of Gate W6
 * is a flag-gated logging adapter that formats these values with ESP_LOG.
 *
 * WHY THE TOKENS MATTER MORE THAN THE VALUES. A pilot runs on the owner's
 * private hardware at a private location. The whole diagnostic surface is
 * therefore an ENUM SURFACE: a reader learns the state machine's position and
 * the health of every safety invariant, and learns nothing about where the
 * device is, which endpoint it used or what the weather actually was.
 */

/* ------------------------------------------------------------------ */
/* Bounded event vocabulary                                            */
/* ------------------------------------------------------------------ */

/*
 * The complete set of observable pilot transitions. Edge-triggered: an event
 * is emitted when the pilot ENTERS a condition, not on every step, so a
 * multi-hour wait produces one line rather than thousands.
 */
typedef enum {
    WX_EV_NONE = 0,
    WX_EV_PILOT_BOOT,
    WX_EV_CONFIG_UNCONFIGURED,
    WX_EV_CONFIG_INVALID,
    WX_EV_CONFIG_READY,
    WX_EV_WAIT_TRUSTED_TIME,
    WX_EV_TRUSTED_TIME_READY,
    WX_EV_WAIT_SCHEDULE,
    WX_EV_SCHEDULE_DUE,
    WX_EV_FETCH_START,
    WX_EV_FETCH_SUCCESS,
    WX_EV_FETCH_TIMEOUT,
    WX_EV_FETCH_REJECTED,
    WX_EV_FORECAST_STALE,
    WX_EV_POLICY_EVALUATED,
    WX_EV_RECOMMENDATION_READY,
    WX_EV_RECOMMENDATION_NOT_ACTIONABLE,
    WX_EV_NO_RECOMMENDATION,
    WX_EV_PILOT_SUMMARY,
    WX_EV_INVARIANT_VIOLATION,
    WX_EV__COUNT
} NxWeatherPilotEvent;

/* Stable machine token. Total; unknown values map to the zero token. */
const char *nx_weather_pilot_event_str(NxWeatherPilotEvent e);

/* ------------------------------------------------------------------ */
/* Invariant monitor (read-only)                                       */
/* ------------------------------------------------------------------ */

/*
 * FACT AVAILABILITY — the core of the Gate W6 fail-closed rule.
 *
 * A posture field is only evidence when an AUTHORITY actually produced it.
 * A hardcoded false, a zero-initialised struct, a configuration-derived
 * assumption or a caller default is NOT evidence, and this enum exists so
 * the difference cannot be lost:
 *
 *   ABSENT (0)    - the zero value. Nobody supplied this fact. A zeroed
 *                   posture is therefore entirely ABSENT and can never be
 *                   healthy, which is what makes "no structure silently
 *                   defaults required facts to zero" enforceable.
 *   OBSERVED      - read this boot from the owning subsystem's read-only API.
 *   STRUCTURAL    - decided at COMPILE time by which flags exist, so the
 *                   symbol is provably absent from the image. This is
 *                   stronger than a runtime read, not weaker.
 *   UNAVAILABLE   - the owning subsystem exposes no safe observable for it.
 *                   Explicitly recorded rather than guessed.
 *   STALE         - obtained, but older than the freshness this pilot needs.
 */
typedef enum {
    NX_WX_FACT_ABSENT = 0,
    NX_WX_FACT_OBSERVED,
    NX_WX_FACT_STRUCTURAL,
    NX_WX_FACT_UNAVAILABLE,
    NX_WX_FACT_STALE,
    NX_WX_FACT__COUNT
} NxWeatherFactState;

const char *nx_weather_fact_state_str(NxWeatherFactState s);

/* True only for a fact that may be relied upon. */
bool nx_weather_fact_usable(NxWeatherFactState s);

/*
 * Per-field provenance for the posture below. EVERY required field has an
 * entry; a field whose entry is not usable makes the whole posture unhealthy,
 * regardless of the value that accompanies it.
 */
typedef struct {
    NxWeatherFactState feature_posture;   /* weather/policy/recommendation-only */
    NxWeatherFactState execution;         /* B7 availability + action           */
    NxWeatherFactState command_api;       /* B8 route availability              */
    NxWeatherFactState ownership;         /* B5 current owner                   */
    NxWeatherFactState hardware_counters; /* frequency/voltage/fan/thermal      */
    NxWeatherFactState pool_counters;     /* pool/protocol mutation             */
    NxWeatherFactState restart_counters;  /* restart/OTA request                */
    NxWeatherFactState session_counters;  /* timed-session mutation             */
    NxWeatherFactState tuning_snapshot;   /* vs the captured pilot baseline     */
    NxWeatherFactState mining_posture;    /* source mining still allowed        */
    NxWeatherFactState resources;         /* heap/min-heap/stack/uptime         */
} NxWeatherPilotFacts;

/*
 * The posture a healthy recommendation-only pilot must hold at ALL times.
 * Every field is an observation the integrator reads from an authority that
 * already exists — this module owns none of them and can change none of them.
 *
 * `facts` states WHERE each value came from. A value without a usable
 * provenance is not evidence and cannot make the pilot healthy.
 */
typedef struct {
    NxWeatherPilotFacts facts;
    /* Feature posture. */
    bool weather_enabled;
    bool source_policy_valid;
    bool recommendation_only;

    /* Timed-session surfaces that must stay unavailable in this pilot. */
    bool execution_available;      /* B7 executor compiled AND reachable   */
    bool command_api_available;    /* B8 timed-session command routes      */
    bool b5_owner_present;         /* any mutating operation owner         */
    bool b7_action_observed;       /* any executor action this boot        */

    /*
     * Mutation counters the integrator samples from the authorities that own
     * them. A recommendation-only pilot requires every one to be ZERO: this
     * module compares, it does not count.
     *
     * They are counts SINCE THE PILOT BASELINE, not since power-on. The
     * baseline is captured once the boot has settled, and the pilot claims
     * nothing about the interval before it — the observation window is
     * reported rather than assumed to start at zero.
     */
    bool counter_history_lost;     /* regressed or saturated: unprovable    */
    uint32_t hardware_write_count; /* frequency/voltage/fan/thermal applies */
    uint32_t pool_write_count;
    uint32_t protocol_write_count;
    uint32_t restart_request_count;
    uint32_t ota_request_count;
    uint32_t session_mutation_count;

    /* Operating posture that must be preserved. */
    bool source_mining_allowed;
    bool tuning_unchanged;
} NxWeatherPilotPosture;

/*
 * Stable violation codes. The FIRST violation in this fixed order is
 * reported, so a given broken posture always yields the same code — a
 * property tests can pin. Unknown/uninitialised input fails closed.
 */
typedef enum {
    WX_INV_OK = 0,
    WX_INV_WEATHER_DISABLED,
    WX_INV_SOURCE_POLICY_INVALID,
    WX_INV_NOT_RECOMMENDATION_ONLY,
    WX_INV_EXECUTION_AVAILABLE,
    WX_INV_COMMAND_API_AVAILABLE,
    WX_INV_OWNER_PRESENT,
    WX_INV_EXECUTOR_ACTION,
    WX_INV_HARDWARE_WRITE,
    WX_INV_POOL_WRITE,
    WX_INV_PROTOCOL_WRITE,
    WX_INV_RESTART_REQUESTED,
    WX_INV_OTA_REQUESTED,
    WX_INV_SESSION_MUTATION,
    WX_INV_MINING_BLOCKED,
    WX_INV_TUNING_CHANGED,
    /*
     * FAIL-CLOSED PROVENANCE CODES. A required fact that was never supplied,
     * cannot be obtained from its owning subsystem, or is too old is NOT a
     * healthy pilot — it is an unobservable one, which is a different and
     * equally reportable condition.
     */
    WX_INV_FACT_ABSENT,
    WX_INV_FACT_UNAVAILABLE,
    WX_INV_FACT_STALE,
    WX_INV_COUNTER_REGRESSION,
    WX_INV__COUNT
} NxWeatherPilotInvariant;

const char *nx_weather_pilot_invariant_str(NxWeatherPilotInvariant v);

/*
 * Evaluate the posture. Pure, total, side-effect free, NULL-safe (NULL is a
 * violation, never OK). Returns WX_INV_OK only when every invariant holds.
 * This function CANNOT recover, restart, clear state, change mining or
 * authorize anything — it returns a code and nothing else.
 */
NxWeatherPilotInvariant nx_weather_pilot_check(const NxWeatherPilotPosture *p);

/*
 * Counter-regression guard. A mutation counter may only ever rise; a value
 * lower than one previously observed means the counter was reset, wrapped or
 * came from a different authority, and the pilot can no longer prove that
 * nothing was mutated in between. That ambiguity is a violation, not a pass.
 * `prev` is the caller's last observation; both must be usable facts.
 */
bool nx_weather_pilot_counter_sane(uint32_t prev, uint32_t now);

/* True only for WX_INV_OK. */
bool nx_weather_pilot_healthy(NxWeatherPilotInvariant v);

/* ------------------------------------------------------------------ */
/* Bounded diagnostic state                                            */
/* ------------------------------------------------------------------ */

/* Minimum spacing between periodic summaries, in microseconds. */
#define NX_WX_PILOT_SUMMARY_MIN_INTERVAL_US (60ull * 1000000ull)

/*
 * The sanitized line the logging adapter prints. Every member is a bounded
 * scalar or an enum; there is no pointer and no character array, so a private
 * value cannot be carried here even by mistake.
 */
typedef struct {
    uint32_t sequence;             /* monotonic; proves no gap or flood    */
    NxWeatherPilotEvent event;

    /* Configuration shape — booleans only, never the values. */
    bool provider_configured;
    bool location_configured;
    bool timezone_configured;
    bool recommendation_only;
    NxWeatherSourceStatus source_status;

    /* Runtime position. */
    WeatherRuntimeState   runtime_state;
    WeatherFreshnessClass freshness;
    WeatherProviderResult provider_result;
    WeatherNotExecutedReason not_executed;
    bool trusted_time_available;
    /*
     * GATE W6.3.2 — AUTHORIZATION and DUE-NESS are two different facts and are
     * reported separately on purpose. `schedule_enabled` says the owner built
     * an image permitted to ask for weather at all; `schedule_due` says the
     * committed Brussels schedule has actually opened a window right now.
     * Conflating them would make "nothing is happening" unreadable: an
     * unauthorized image and an authorized one between windows look identical
     * from the outside, and only the first is a configuration decision.
     */
    bool schedule_enabled;
    bool schedule_due;
    bool policy_evaluated;
    bool recommendation_present;
    bool actionable_in_future_gate;

    /* The load-bearing assertion of the whole pilot. */
    bool executed;                 /* ALWAYS false by construction         */

    /* Bounded counters. */
    uint32_t fetch_attempts;
    uint32_t recommendations;
    NxWeatherPilotInvariant invariant;

    /* Resource observations (no private meaning). */
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t stack_high_water;
    uint64_t uptime_us;

    /*
     * Gate W6.1 evidence. Bounded scalars and one enum, so a reviewer can see
     * WHY the pilot is (un)healthy without any private value being expressible
     * here. `baseline_state` is the token; `baseline_us` is the MONOTONIC
     * uptime at which the observation window opened, so the interval the
     * pilot does not claim to have observed is explicit rather than implied.
     */
    uint8_t  baseline_state;       /* NxMutationBaselineState as a token id  */
    uint8_t  baseline_block;       /* NxBaselineBlock: WHY not ready yet     */
    uint64_t baseline_us;
    bool     baseline_zero_at_capture;
    bool     mutation_observability_present;
    bool     counter_history_lost;
    uint32_t mut_hardware;
    uint32_t mut_pool;
    uint32_t mut_protocol;
    uint32_t mut_restart;
    uint32_t mut_ota;
    uint32_t mut_session;
    bool     tuning_unchanged;

    /*
     * Gate W6.2 — the READ-ONLY projection of the committed Gate B10
     * sanitized trusted-time diagnostics.
     *
     * WHY IT EXISTS. `trusted_time_available` is ONE bit read from the W4
     * runtime recommendation: whether the anchor was trusted at the moment of
     * the last evaluation. It says nothing about how the provider reached that
     * position, so from it alone an owner watching a real pilot could not
     * distinguish "SNTP never started", "SNTP started and is syncing", "the
     * candidate was rejected" and "the runtime was never bound at all" — every
     * one of those prints trusted_time=0.
     *
     * These fields answer that question from the ONE authority that owns it:
     * the B10 diagnostics published by the single Gate B6 owner task. In the
     * bound posture they are a live projection of that authority, re-read on
     * every emitted line; when it is structurally or temporarily unreadable,
     * `time_fact` says which rather than inventing a value. They create no
     * second provider, start nothing, change no trust policy and feed no
     * decision — nx_weather_pilot_check() does not read them, so the invariant
     * verdict is byte-for-byte what it was.
     *
     * PRIVACY. Every member is a bounded scalar or a token id. There is no
     * string field, so the configured hostname, a resolved address, DNS error
     * text, a raw epoch or a sync generation cannot be expressed here.
     */
    NxWeatherFactState time_fact;   /* provenance of the eight fields below */
    bool     time_source_configured;/* a VALID source exists (never which)  */
    uint8_t  time_source_state;     /* PoolTimeSourceState as a token id    */
    uint32_t time_sync_attempts;    /* bounded start attempts               */
    bool     time_operational;      /* available AND the service is healthy */
    bool     time_available;        /* B2 says trusted right now            */
    bool     time_sync_age_valid;
    uint32_t time_sync_age_s;       /* MONOTONIC age, saturating, never epoch */
    uint8_t  last_time_sync_result; /* PoolTimeError as a token id          */

    /*
     * Gate W6.3 — the READ-ONLY projection of the bounded asynchronous
     * weather I/O machine.
     *
     * WHY IT EXISTS. W6.2 made trusted time observable but left the transport
     * unwired, so a pilot could reach "time is trusted, the schedule is due"
     * and then simply stop, with no way to see why. These fields make the one
     * remaining step visible: whether a request was submitted, whether it is
     * in flight, whether a result is waiting, and how the last attempt ended.
     *
     * `io_fact` is the provenance, exactly as `time_fact` is for B10:
     * STRUCTURAL when CONFIG_NX_WEATHER_IO_WORKER is not in the image at all,
     * UNAVAILABLE when the machine exists but cannot be read, OBSERVED
     * otherwise. A zeroed line is ABSENT and can never look healthy.
     *
     * `io_worker_stack_free` is the honest half of the stack-size decision.
     * The 12 KB default in Kconfig is conservative, NOT measured; this number
     * is the measurement, and a physical pilot must read it before that size
     * is treated as proven.
     *
     * PRIVACY. Every member is a bounded scalar or a token id. There is no
     * string field, so a coordinate, provider host, URL, query string,
     * response body or raw epoch cannot be expressed here.
     */
    NxWeatherFactState io_fact;
    uint8_t  io_state;              /* NxWeatherIoState as a token id       */
    uint8_t  io_last_event;         /* NxWeatherIoState as a token id       */
    uint32_t io_generation;         /* last accepted request id             */
    bool     io_in_flight;
    bool     io_result_pending;
    uint32_t io_submit_count;
    uint32_t io_reject_busy_count;
    uint32_t io_reject_pending_count;
    uint32_t io_success_count;
    uint32_t io_failure_count;
    uint32_t io_timeout_count;
    uint32_t io_discard_count;
    uint32_t io_consume_count;
    uint8_t  io_last_result;        /* WeatherProviderResult as a token id  */
    uint32_t io_request_age_s;      /* MONOTONIC, saturating, never an epoch */
    uint32_t io_worker_count;       /* structurally 0 or 1; proves unicity  */
    uint32_t io_worker_stack_free;  /* high-water headroom, 0 when no worker */

    /*
     * GATE W6.4 — the crash-safe window deduplication, made visible.
     *
     * Without these, a pilot that stops submitting is indistinguishable from a
     * pilot that is correctly suppressing an already-served window, and the
     * two demand opposite responses. Worse, the FAILURE direction is silent by
     * design: an unreadable store refuses outbound service and produces
     * exactly the same "nothing happened" as a healthy quiet device.
     *
     * `window_fact` is the provenance, exactly as `io_fact` is for W6.3:
     * STRUCTURAL when CONFIG_NX_WEATHER_PILOT_WINDOW_DEDUP is not in the image
     * at all, UNAVAILABLE when the authority exists but is not serviceable,
     * OBSERVED otherwise. A zeroed line is ABSENT and can never look healthy.
     *
     * `window_store_fact` is the NxWeatherWindowStoreFact token, whose zero is
     * NOT_LOADED — so a zeroed line never claims a usable store either.
     *
     * `window_served_mask` is the DURABLE slot mask for the claimed service
     * day, not a RAM view of it: it is what would survive a power cut.
     *
     * PRIVACY. Scalars and token ids only, as above. The service day and slot
     * mask are governed scheduling state, not owner-private data — they say
     * WHEN this device was permitted to ask, never where it is or what it
     * asked. There is no host, no URL, no coordinate and no credential here,
     * and no string field in which one could be smuggled.
     */
    NxWeatherFactState window_fact;
    bool     window_dedup_enabled;  /* the capability is compiled in        */
    uint8_t  window_store_fact;     /* NxWeatherWindowStoreFact token id    */
    bool     window_ready;          /* persistence permits outbound service */
    bool     window_recovered;      /* a durable claim was read at boot     */
    bool     window_day_present;    /* a service day is durably claimed     */
    uint16_t window_service_year;   /* 0 when absent                        */
    uint8_t  window_service_month;
    uint8_t  window_service_day;
    uint8_t  window_served_mask;    /* durably claimed slots for that day   */
    uint8_t  window_last_decision;  /* NxWeatherWindowDecision token id     */
    uint32_t window_claim_count;        /* saturating                       */
    uint32_t window_suppressed_count;   /* saturating                       */
    uint32_t window_persist_fail_count; /* saturating                       */
} NxWeatherPilotLine;

/*
 * The diagnostic accumulator. Caller-owned, RAM-only, never persisted.
 * `last_event` drives edge triggering; `last_summary_us` drives the rate
 * limit. Both are monotonic-time based — never a wall clock.
 */
typedef struct {
    bool     initialized;
    uint32_t sequence;
    NxWeatherPilotEvent last_event;
    uint64_t last_summary_us;
    bool     summary_ever;
    uint32_t fetch_attempts;
    uint32_t recommendations;
    uint32_t violations;
    NxWeatherPilotInvariant first_violation;
} NxWeatherPilotDiag;

void nx_weather_pilot_diag_init(NxWeatherPilotDiag *d);

/*
 * Gate W6.2 — PURE projection of the Gate B10 sanitized trusted-time
 * diagnostics onto the pilot line. Total, side-effect free and NULL-safe.
 *
 * `runtime_linked` states whether CONFIG_NX_TIMED_SESSIONS put a runtime
 * instance in this image at all; the caller knows, this function does not.
 *
 *   runtime_linked == false          -> STRUCTURAL: no provider can exist.
 *   `d` NULL or structurally invalid -> UNAVAILABLE: nothing may be quoted.
 *   otherwise                        -> OBSERVED, copied field for field.
 *
 * It never derives, smooths or infers a state: an unreadable authority is
 * reported as unreadable, never as a reassuring zero. It authorizes nothing
 * and is deliberately NOT consulted by nx_weather_pilot_check() — the pilot's
 * invariant verdict does not depend on trusted time and must not begin to.
 */
void nx_weather_pilot_time_project(const PoolTimeSourceDiagnostics *d,
                                   bool runtime_linked,
                                   NxWeatherPilotLine *out);

/*
 * Gate W6.3 — PURE projection of the bounded async weather I/O diagnostics
 * onto the pilot line. Total, side-effect free and NULL-SAFE, with exactly the
 * same fail-closed shape as the W6.2 trusted-time projection above.
 *
 * `worker_linked` states whether CONFIG_NX_WEATHER_IO_WORKER put an execution
 * context in this image at all; the caller knows, this function does not.
 *
 *   worker_linked == false     -> STRUCTURAL: no weather I/O can occur.
 *   `d` NULL or not present    -> UNAVAILABLE: nothing may be quoted.
 *   otherwise                  -> OBSERVED, copied field for field.
 *
 * It authorizes nothing, starts nothing and is deliberately NOT consulted by
 * nx_weather_pilot_check(): the pilot's invariant verdict does not depend on
 * whether a forecast was fetched, and must not begin to.
 */
void nx_weather_pilot_io_project(const NxWeatherIoDiag *d, bool worker_linked,
                                 uint32_t worker_count, uint32_t worker_stack_free,
                                 NxWeatherPilotLine *out);

/*
 * Project the Gate W6.4 window-deduplication observation onto the line.
 *
 * `dedup_linked` states whether CONFIG_NX_WEATHER_PILOT_WINDOW_DEDUP put the
 * authority in the image at all:
 *   dedup_linked == false  -> STRUCTURAL: no durable claim exists, and
 *                             because the schedule DEPENDS on this flag, no
 *                             outbound window service exists either.
 *   dedup_linked == true   -> OBSERVED when the authority published a reading,
 *                             UNAVAILABLE when it did not.
 *
 * Pure and total: `d` may be NULL, and every field is written on every path so
 * a refusal can never be read as a healthy zero.
 *
 * It takes the observation as an ARGUMENT rather than reading the authority
 * itself. That keeps the projection pure — every posture, including the
 * unavailable one, is reachable in a test with no store, no backend and no
 * flash model at all — and it keeps this component from becoming a second
 * reader of persistence state.
 */
void nx_weather_pilot_window_project(const NxWeatherWindowDiag *d,
                                     bool dedup_linked,
                                     NxWeatherPilotLine *out);

/*
 * Classify one runtime observation into an event. Pure: it maps the W5 status
 * and the W4 runtime state/recommendation onto exactly one token, in the same
 * order the runtime gate itself evaluates them.
 */
NxWeatherPilotEvent nx_weather_pilot_classify(NxWeatherSourceStatus source,
                                              WeatherRuntimeState state,
                                              const WeatherRecommendation *rec,
                                              bool schedule_due);

/*
 * Should this event be emitted now? Edge-triggered for transitions, rate
 * limited for summaries, and ALWAYS true for an invariant violation (a
 * violation is never suppressed). `now_us` is monotonic.
 */
bool nx_weather_pilot_should_emit(const NxWeatherPilotDiag *d,
                                  NxWeatherPilotEvent e, uint64_t now_us);

/*
 * Record an emitted event and produce the sanitized line. Advances the
 * sequence, updates edge/rate state and the bounded counters. Returns false
 * (writing nothing) for NULL arguments or an out-of-range event.
 */
bool nx_weather_pilot_record(NxWeatherPilotDiag *d,
                             NxWeatherPilotEvent e,
                             uint64_t now_us,
                             const NxWeatherSourceConfig *cfg,
                             NxWeatherSourceStatus source,
                             WeatherRuntimeState state,
                             const WeatherRecommendation *rec,
                             bool schedule_due,
                             NxWeatherPilotInvariant invariant,
                             NxWeatherPilotLine *out);

_Static_assert(WX_EV_NONE == 0, "a zeroed event must be the inert token");
_Static_assert(WX_INV_OK == 0, "a zeroed invariant code must mean healthy "
                               "only when explicitly evaluated");
_Static_assert(WX_EV__COUNT == 20 && WX_INV__COUNT == 20,
               "W6 enums changed — review tokens, adapter and tests");

#endif /* NX_WEATHER_PILOT_DIAG_H_ */
