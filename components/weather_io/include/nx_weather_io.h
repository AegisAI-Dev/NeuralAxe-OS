#ifndef NX_WEATHER_IO_H_
#define NX_WEATHER_IO_H_

#include <stdint.h>
#include <stdbool.h>

#include "weather_provider.h"   /* WeatherRequest, WeatherParseContext, result */
#include "weather_runtime.h"    /* WeatherObservation                          */
#include "local_schedule.h"     /* WeatherLocalDate, slot identity             */

/*
 * NeuralAxe Weather-Aware Tuning — BOUNDED ASYNCHRONOUS WEATHER I/O
 * (Phase 2W, Gate W6.3). Board 601 / BM1370 only.
 *
 * THE PROBLEM THIS EXISTS TO SOLVE. The committed Gate W3 transport
 * (weather_open_meteo_http.c) is SYNCHRONOUS. Its bound, WEATHER_HTTP_TIMEOUT_MS
 * = 8000, is an esp_http_client socket timeout — it applies to EACH blocking
 * operation, not to the call as a whole. One fetch performs, in order: an
 * esp_http_client_open() covering DNS + TCP + the TLS handshake, one
 * fetch_headers(), up to WEATHER_RESPONSE_CAP / buffer_size body reads, and one
 * 1-byte overflow probe. Each may consume the full 8 s, so the WORST-CASE wall
 * time of one committed fetch is tens of seconds, not eight.
 *
 * The only host Gate W6 offers is the ~1 s statistics task. Running that fetch
 * there would stall the task for dozens of its own periods and run a TLS
 * handshake on a stack it shares with the statistics work. Gate W6.2 therefore
 * left `deps.transport = NULL` and said so plainly. W6.3 supplies the missing
 * execution context, and nothing else.
 *
 * WHAT THIS MODULE IS. A PURE, bounded, single-producer/single-consumer handoff
 * with EXACTLY ONE request slot and EXACTLY ONE result slot. It has no task, no
 * queue, no timer, no allocation and no network symbol of any kind. It is a
 * state machine over caller-owned storage, so every transition in the failure
 * matrix is reachable in a host/QEMU test with no FreeRTOS and no transport.
 *
 * The impure half — one static FreeRTOS worker task that executes the committed
 * W3 transport and publishes into this structure — lives in the flag-gated
 * adapter nx_weather_io_worker.c and is the ONLY file that includes esp_*.
 *
 * WHAT THIS MODULE IS NOT. It is not a scheduler (the committed W3
 * local_schedule decides due-ness), not a retry policy (the committed W3
 * weather_retry owns the budget and backoff), not a second transport, not a
 * time source and not an executor. It cannot change a frequency, a voltage, a
 * fan, a pool, a protocol or a persisted byte, because it has no API that could
 * and no dependency that would let it.
 *
 * FAIL PASSIVE, ALWAYS. Every refusal here is a refusal: a full slot, an
 * unknown generation, a stale window and an expired deadline all leave mining,
 * the schedule and the owner's settings exactly as they were.
 *
 * PRIVACY. NxWeatherIoDiag carries bounded scalars and token ids only. It has
 * no pointer and no character array, so a coordinate, host, URL, query string,
 * response body or raw epoch cannot be expressed through it even by mistake.
 */

/* ------------------------------------------------------------------ */
/* Bounded vocabulary                                                  */
/* ------------------------------------------------------------------ */

/*
 * The complete observable vocabulary. `state` below only ever holds a DURABLE
 * condition (UNINITIALIZED, IDLE, SUBMITTED, RUNNING, RESULT_READY, FAILED,
 * TIMEOUT); the remaining tokens are TRANSITIONS and are observable through
 * `last_event`. Both are reported, because "a result was discarded" is a fact
 * an owner needs and a durable state cannot express.
 */
typedef enum {
    WX_IO_UNINITIALIZED = 0,    /* fail-closed zero: nothing may be trusted */
    WX_IO_IDLE,                 /* durable: ready to accept one request     */
    WX_IO_SUBMIT,               /* transition: a request was accepted       */
    WX_IO_BUSY,                 /* transition: refused, one already in play */
    WX_IO_SUBMITTED,            /* durable: queued, worker has not claimed  */
    WX_IO_RUNNING,              /* durable: worker claimed it; in flight    */
    WX_IO_RESULT_READY,         /* durable: exactly one result awaits       */
    WX_IO_RESULT_ACCEPTED,      /* transition: consumed exactly once        */
    WX_IO_RESULT_DISCARDED,     /* transition: stale/foreign, never applied */
    WX_IO_TIMEOUT,              /* durable: deadline passed; slot released  */
    WX_IO_FAILED,               /* durable: transport/parse refused it      */
    WX_IO_RETURN_IDLE,          /* transition: back to IDLE, nothing held   */
    WX_IO_STATE__COUNT
} NxWeatherIoState;

const char *nx_weather_io_state_str(NxWeatherIoState s);

/* True only for a state in which a NEW request may be accepted. */
bool nx_weather_io_state_accepts(NxWeatherIoState s);

/* Outcome of one submission attempt. Every non-ACCEPTED value is passive. */
typedef enum {
    WX_IO_SUBMIT_ACCEPTED = 0,
    WX_IO_SUBMIT_REJECTED_UNINITIALIZED,
    WX_IO_SUBMIT_REJECTED_INVALID,      /* malformed request or identity    */
    WX_IO_SUBMIT_REJECTED_BUSY,         /* one already submitted/running    */
    WX_IO_SUBMIT_REJECTED_RESULT_PENDING, /* result slot occupied, unread   */
    WX_IO_SUBMIT__COUNT
} NxWeatherIoSubmitResult;

const char *nx_weather_io_submit_str(NxWeatherIoSubmitResult r);

/* ------------------------------------------------------------------ */
/* Request identity (Gate W6.3 §7) — RAM-ONLY                          */
/* ------------------------------------------------------------------ */

/*
 * The window a request belongs to. This is NOT Gate W6.4 persistence: it lives
 * in RAM, it is never written to NVS, and a reboot deliberately erases it. It
 * exists so a result can be matched to the request that created it and to
 * nothing else.
 *
 * `slot_index` is the committed W3 schedule slot (>= 0 for DUE / CATCH_UP_DUE);
 * `date` is the trusted Brussels local date the slot belongs to. Together with
 * the monotonic `generation` and the selected `provider` they make a result
 * that belongs to a superseded window structurally unusable.
 */
typedef struct {
    WeatherLocalDate  date;
    int8_t            slot_index;
    WeatherProviderId provider;
} NxWeatherIoWindow;

/* Exact window equality. NULL-safe; a NULL operand never matches. */
bool nx_weather_io_window_equal(const NxWeatherIoWindow *a,
                                const NxWeatherIoWindow *b);

/*
 * One bounded request. A VALUE type by design: `WeatherRequest` carries only
 * compile-constant rodata pointers for host/path plus an inline bounded query,
 * and WeatherParseContext is entirely scalar — so a copy of this struct
 * survives the handoff with no pointer into the submitter's stack.
 */
typedef struct {
    uint32_t            generation;   /* strictly increasing; 0 is invalid  */
    NxWeatherIoWindow   window;
    WeatherRequest      request;      /* built by the committed W3 builder  */
    WeatherParseContext parse;        /* trusted date/epoch, caller-supplied */
} NxWeatherIoRequest;

/* One bounded result. Also a pure value type. */
typedef struct {
    uint32_t              generation;   /* echoes the request that made it  */
    NxWeatherIoWindow     window;
    WeatherProviderResult result;
    WeatherObservation    observation;  /* forecast is a scalar-only struct */
    uint64_t              started_us;   /* monotonic                        */
    uint64_t              finished_us;  /* monotonic                        */
} NxWeatherIoResult;

/* ------------------------------------------------------------------ */
/* Bounds                                                              */
/* ------------------------------------------------------------------ */

/*
 * The in-flight DEADLINE, enforced by the CONSUMER, not by the worker.
 *
 * WHY IT IS 120 s AND WHY IT IS NOT 8 s. As described above, the committed
 * transport's 8 s is per socket operation. A pathological but legal fetch is
 * open(8) + fetch_headers(8) + ceil(4096/1024)=4 reads(32) + probe(8) = 56 s,
 * plus scheduling. 120 s clears that worst case with better than 2x headroom
 * while still being a hard, monotonic, bounded ceiling.
 *
 * This deadline is what makes "no permanent BUSY latch" structural rather than
 * dependent on the worker behaving: if the worker were to wedge forever, the
 * statistics task still returns this state machine to IDLE, and the wedged
 * worker's eventual publish is rejected as a stale generation.
 */
#define NX_WX_IO_DEADLINE_US (120ull * 1000000ull)

/* Saturating ceiling for every counter below. Counters never wrap. */
#define NX_WX_IO_COUNTER_MAX 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* The state machine                                                   */
/* ------------------------------------------------------------------ */

/*
 * Caller-owned, RAM-only, never persisted. ONE request slot, ONE result slot,
 * both inline: there is no pointer to own, nothing to free and nothing that can
 * grow. Storage is O(1) and fixed at compile time.
 *
 * CONCURRENCY. This structure is not self-synchronising: it is a plain data
 * structure whose invariants hold under a caller-provided mutual exclusion.
 * The flag-gated adapter serialises every entry point with the same
 * portMUX critical section, exactly as the committed Gate B6 runtime does for
 * its own snapshot. Host and QEMU tests drive it single-threaded, which is why
 * every transition below is deterministically reachable without a scheduler.
 */
typedef struct {
    bool             initialized;
    NxWeatherIoState state;
    NxWeatherIoState last_event;

    /* The single request slot. Valid only in SUBMITTED. */
    bool               request_present;
    NxWeatherIoRequest request;

    /* The single result slot. Valid only in RESULT_READY. */
    bool              result_present;
    NxWeatherIoResult result;

    /*
     * Identity. `generation` is the last ACCEPTED request's id and only ever
     * increases; `in_flight_generation` is the one the worker may publish.
     *
     * `in_flight_window` is held SEPARATELY from the request slot on purpose:
     * the request slot is emptied the moment the worker claims it (so a second
     * claim is impossible), but the window must still be known at publish time
     * to stamp the result. Keeping it here means the identity a result carries
     * never depends on storage the claim already released.
     */
    uint32_t          generation;
    uint32_t          in_flight_generation;
    NxWeatherIoWindow in_flight_window;
    bool              in_flight;
    uint64_t          in_flight_started_us;

    /* Saturating counters. Never wrap; see NX_WX_IO_COUNTER_MAX. */
    uint32_t submit_count;
    uint32_t reject_busy_count;
    uint32_t reject_pending_count;
    uint32_t claim_count;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t timeout_count;
    uint32_t discard_count;
    uint32_t consume_count;

    WeatherProviderResult last_result;
} NxWeatherIo;

/* Initialise to a clean IDLE. Total; NULL is a no-op. */
void nx_weather_io_init(NxWeatherIo *io);

/*
 * SUBMITTER SIDE (the statistics/weather tick). O(1), never blocks, never
 * allocates and never touches the network. A refusal is passive: the caller
 * keeps its own state and simply tries again on a later tick.
 *
 * `req->generation` must be strictly greater than every previously accepted
 * generation, and the window must carry a usable slot and a configured
 * provider; anything else is REJECTED_INVALID and changes nothing.
 */
NxWeatherIoSubmitResult nx_weather_io_submit(NxWeatherIo *io,
                                             const NxWeatherIoRequest *req,
                                             uint64_t now_us);

/*
 * WORKER SIDE. Claim the pending request, moving SUBMITTED -> RUNNING and
 * copying it out. False when there is nothing to claim. Exactly one claim per
 * submission is possible, which is what bounds in-flight work to one.
 */
bool nx_weather_io_claim(NxWeatherIo *io, NxWeatherIoRequest *out,
                         uint64_t now_us);

/*
 * WORKER SIDE. Publish the outcome of the claimed request.
 *
 * The publish is ACCEPTED into the result slot only when `generation` still
 * matches the in-flight generation AND the result slot is empty. A publish for
 * a superseded generation — the case that arises when the consumer already
 * timed the request out — is DISCARDED: counted, reported, and never applied.
 * Returns true only when the result was stored.
 */
bool nx_weather_io_publish(NxWeatherIo *io, uint32_t generation,
                           WeatherProviderResult result,
                           const WeatherObservation *obs, uint64_t now_us);

/*
 * SUBMITTER SIDE. Consume the ready result EXACTLY ONCE. On true the slot is
 * emptied and the machine returns to IDLE, so a duplicate tick cannot apply the
 * same observation twice. `expect` may be NULL; when supplied, a result whose
 * window does not match is DISCARDED rather than returned.
 */
bool nx_weather_io_consume(NxWeatherIo *io, const NxWeatherIoWindow *expect,
                           NxWeatherIoResult *out, uint64_t now_us);

/*
 * SUBMITTER SIDE. Enforce the bounded deadline. Called on every tick. When an
 * in-flight request has exceeded NX_WX_IO_DEADLINE_US the machine records a
 * timeout, releases the slot and returns to IDLE — WITHOUT touching the worker,
 * which is why a wedged worker can never latch this module BUSY. Returns true
 * when a timeout was recorded on this call.
 */
bool nx_weather_io_tick(NxWeatherIo *io, uint64_t now_us);

/* ------------------------------------------------------------------ */
/* Privacy-safe observation                                            */
/* ------------------------------------------------------------------ */

/*
 * Everything an owner may see. Bounded scalars and token ids only — no pointer,
 * no character array, so a coordinate, host, URL, query, body, TLS error string
 * or raw epoch is not expressible here.
 */
typedef struct {
    bool     present;             /* false => nothing may be quoted        */
    uint8_t  state;               /* NxWeatherIoState token id             */
    uint8_t  last_event;          /* NxWeatherIoState token id             */
    uint32_t generation;          /* last accepted request id              */
    bool     in_flight;
    bool     result_pending;
    uint32_t submit_count;
    uint32_t reject_busy_count;
    uint32_t reject_pending_count;
    uint32_t claim_count;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t timeout_count;
    uint32_t discard_count;
    uint32_t consume_count;
    uint8_t  last_result;         /* WeatherProviderResult token id        */
    uint32_t request_age_s;       /* MONOTONIC, saturating, never an epoch */
} NxWeatherIoDiag;

/* Total, side-effect free, NULL-safe. A NULL or uninitialised machine yields
 * present=false rather than a reassuring zero. */
void nx_weather_io_observe(const NxWeatherIo *io, uint64_t now_us,
                           NxWeatherIoDiag *out);

_Static_assert(WX_IO_UNINITIALIZED == 0,
               "a zeroed I/O machine must be the fail-closed token");
_Static_assert(WX_IO_SUBMIT_ACCEPTED == 0,
               "submit result zero must mean accepted only when evaluated");
_Static_assert(WX_IO_STATE__COUNT == 12 && WX_IO_SUBMIT__COUNT == 5,
               "W6.3 enums changed — review tokens, adapter and tests");

#endif /* NX_WEATHER_IO_H_ */
