/*
 * NeuralAxe Weather-Aware Tuning — bounded asynchronous weather I/O, pure core
 * (Phase 2W, Gate W6.3).
 *
 * No FreeRTOS, no esp_*, no network, no allocation, no clock read. Monotonic
 * microseconds arrive as arguments. Every function is total and NULL-safe, and
 * every refusal leaves the machine in a state a later tick can make progress
 * from — there is no path here that can latch.
 */

#include <string.h>

#include "nx_weather_io.h"

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_weather_io_state_str(NxWeatherIoState s)
{
    switch (s) {
    case WX_IO_UNINITIALIZED:    return "WX_IO_UNINITIALIZED";
    case WX_IO_IDLE:             return "WX_IO_IDLE";
    case WX_IO_SUBMIT:           return "WX_IO_SUBMIT";
    case WX_IO_BUSY:             return "WX_IO_BUSY";
    case WX_IO_SUBMITTED:        return "WX_IO_SUBMITTED";
    case WX_IO_RUNNING:          return "WX_IO_RUNNING";
    case WX_IO_RESULT_READY:     return "WX_IO_RESULT_READY";
    case WX_IO_RESULT_ACCEPTED:  return "WX_IO_RESULT_ACCEPTED";
    case WX_IO_RESULT_DISCARDED: return "WX_IO_RESULT_DISCARDED";
    case WX_IO_TIMEOUT:          return "WX_IO_TIMEOUT";
    case WX_IO_FAILED:           return "WX_IO_FAILED";
    case WX_IO_RETURN_IDLE:      return "WX_IO_RETURN_IDLE";
    default:                     return "WX_IO_UNINITIALIZED";
    }
}

const char *nx_weather_io_submit_str(NxWeatherIoSubmitResult r)
{
    switch (r) {
    case WX_IO_SUBMIT_ACCEPTED:              return "ACCEPTED";
    case WX_IO_SUBMIT_REJECTED_UNINITIALIZED:return "REJ_UNINIT";
    case WX_IO_SUBMIT_REJECTED_INVALID:      return "REJ_INVALID";
    case WX_IO_SUBMIT_REJECTED_BUSY:         return "REJ_BUSY";
    case WX_IO_SUBMIT_REJECTED_RESULT_PENDING: return "REJ_PENDING";
    default:                                 return "REJ_INVALID";
    }
}

bool nx_weather_io_state_accepts(NxWeatherIoState s)
{
    /* Only a genuinely empty machine accepts work. TIMEOUT and FAILED are
     * durable *reports* of the previous attempt and still accept the next
     * one — that is what prevents a failure from becoming a latch. */
    return s == WX_IO_IDLE || s == WX_IO_TIMEOUT || s == WX_IO_FAILED;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Saturating increment. A counter at the ceiling stays there rather than
 * wrapping to zero, so "nothing happened" can never be forged by overflow. */
static void bump(uint32_t *c)
{
    if (*c < NX_WX_IO_COUNTER_MAX) {
        (*c)++;
    }
}

static void note(NxWeatherIo *io, NxWeatherIoState ev)
{
    io->last_event = ev;
}

/* Release the in-flight slot without touching the result slot. */
static void release_in_flight(NxWeatherIo *io)
{
    io->in_flight            = false;
    io->in_flight_generation = 0u;
    io->in_flight_started_us = 0u;
    io->request_present      = false;
    memset(&io->in_flight_window, 0, sizeof(io->in_flight_window));
    memset(&io->request, 0, sizeof(io->request));
}

bool nx_weather_io_window_equal(const NxWeatherIoWindow *a,
                                const NxWeatherIoWindow *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return a->slot_index == b->slot_index &&
           a->provider   == b->provider   &&
           a->date.year  == b->date.year  &&
           a->date.month == b->date.month &&
           a->date.day   == b->date.day;
}

/* A request is structurally usable only with a real identity and window. */
static bool request_valid(const NxWeatherIoRequest *r)
{
    if (r == NULL || r->generation == 0u) {
        return false;
    }
    if (r->window.slot_index < 0) {
        return false;   /* not a DUE / CATCH_UP_DUE slot                    */
    }
    if (r->window.provider == WEATHER_PROVIDER_UNCONFIGURED) {
        return false;   /* an unconfigured provider can never be fetched    */
    }
    /* The committed W3 builder owns host/path; a request without them never
     * came from that builder and is refused rather than repaired. */
    if (r->request.host == NULL || r->request.path == NULL) {
        return false;
    }
    /* Untrusted fetch epochs are refused here as well as in the parser: a
     * result that cannot be dated cannot be matched to a window. */
    if (!r->parse.fetch_epoch_trusted) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void nx_weather_io_init(NxWeatherIo *io)
{
    if (io == NULL) {
        return;
    }
    memset(io, 0, sizeof(*io));
    io->initialized = true;
    io->state       = WX_IO_IDLE;
    io->last_event  = WX_IO_RETURN_IDLE;
    io->last_result = WEATHER_PROVIDER_ERR_UNCONFIGURED;
}

/* ------------------------------------------------------------------ */
/* Submitter side                                                      */
/* ------------------------------------------------------------------ */

NxWeatherIoSubmitResult nx_weather_io_submit(NxWeatherIo *io,
                                             const NxWeatherIoRequest *req,
                                             uint64_t now_us)
{
    if (io == NULL || !io->initialized) {
        return WX_IO_SUBMIT_REJECTED_UNINITIALIZED;
    }
    if (!request_valid(req)) {
        note(io, WX_IO_BUSY);   /* nothing accepted; the machine is unchanged */
        return WX_IO_SUBMIT_REJECTED_INVALID;
    }
    /*
     * A generation must strictly advance. This is what makes a late publish
     * from a superseded attempt structurally identifiable rather than a race
     * the caller has to reason about.
     */
    if (req->generation <= io->generation) {
        return WX_IO_SUBMIT_REJECTED_INVALID;
    }
    /*
     * ORDER MATTERS. An unread result is reported before BUSY, because the
     * two have different cures: a pending result needs a consume, an in-flight
     * request needs time. Collapsing them would hide which one is stuck.
     */
    if (io->result_present) {
        bump(&io->reject_pending_count);
        note(io, WX_IO_BUSY);
        return WX_IO_SUBMIT_REJECTED_RESULT_PENDING;
    }
    if (io->in_flight || io->request_present) {
        bump(&io->reject_busy_count);
        note(io, WX_IO_BUSY);
        return WX_IO_SUBMIT_REJECTED_BUSY;
    }
    if (!nx_weather_io_state_accepts(io->state)) {
        bump(&io->reject_busy_count);
        note(io, WX_IO_BUSY);
        return WX_IO_SUBMIT_REJECTED_BUSY;
    }

    io->request              = *req;          /* pure value copy            */
    io->request_present      = true;
    io->generation           = req->generation;
    io->in_flight            = true;
    io->in_flight_generation = req->generation;
    io->in_flight_window     = req->window;   /* survives the claim         */
    io->in_flight_started_us = now_us;
    io->state                = WX_IO_SUBMITTED;
    bump(&io->submit_count);
    note(io, WX_IO_SUBMIT);
    return WX_IO_SUBMIT_ACCEPTED;
}

bool nx_weather_io_tick(NxWeatherIo *io, uint64_t now_us)
{
    if (io == NULL || !io->initialized || !io->in_flight) {
        return false;
    }
    /* Monotonic only. A now_us behind the start (which a correct monotonic
     * source never produces) is treated as zero elapsed, never as expired. */
    if (now_us <= io->in_flight_started_us) {
        return false;
    }
    if ((now_us - io->in_flight_started_us) < NX_WX_IO_DEADLINE_US) {
        return false;
    }
    /*
     * The deadline fired. Release the slot WITHOUT signalling the worker: this
     * module has no authority over a task and must not pretend to. The worker's
     * eventual publish carries a generation that is no longer in flight and is
     * therefore discarded on arrival.
     */
    release_in_flight(io);
    io->state       = WX_IO_TIMEOUT;
    io->last_result = WEATHER_PROVIDER_ERR_READ_TIMEOUT;
    bump(&io->timeout_count);
    note(io, WX_IO_TIMEOUT);
    return true;
}

bool nx_weather_io_consume(NxWeatherIo *io, const NxWeatherIoWindow *expect,
                           NxWeatherIoResult *out, uint64_t now_us)
{
    (void)now_us;

    if (io == NULL || !io->initialized || !io->result_present) {
        return false;
    }
    /*
     * Window binding. A result produced for a window the caller has moved on
     * from is DISCARDED, never returned: applying it would let yesterday's
     * forecast decide today's recommendation.
     */
    if (expect != NULL && !nx_weather_io_window_equal(expect, &io->result.window)) {
        memset(&io->result, 0, sizeof(io->result));
        io->result_present = false;
        io->state          = WX_IO_IDLE;
        bump(&io->discard_count);
        note(io, WX_IO_RESULT_DISCARDED);
        return false;
    }
    if (out != NULL) {
        *out = io->result;
    }
    /* Emptied BEFORE returning, so a duplicate tick finds nothing and the
     * observation can be applied exactly once. */
    memset(&io->result, 0, sizeof(io->result));
    io->result_present = false;
    io->state          = WX_IO_IDLE;
    bump(&io->consume_count);
    note(io, WX_IO_RESULT_ACCEPTED);
    return true;
}

/* ------------------------------------------------------------------ */
/* Worker side                                                         */
/* ------------------------------------------------------------------ */

bool nx_weather_io_claim(NxWeatherIo *io, NxWeatherIoRequest *out,
                         uint64_t now_us)
{
    (void)now_us;

    if (io == NULL || !io->initialized || !io->request_present ||
        io->state != WX_IO_SUBMITTED) {
        return false;
    }
    if (out != NULL) {
        *out = io->request;
    }
    /*
     * The request slot is emptied at claim time. The identity the worker must
     * quote survives in `in_flight_generation`, so nothing is lost, and a
     * second claim of the same submission is impossible.
     */
    io->request_present = false;
    memset(&io->request, 0, sizeof(io->request));
    io->state = WX_IO_RUNNING;
    bump(&io->claim_count);
    note(io, WX_IO_RUNNING);
    return true;
}

bool nx_weather_io_publish(NxWeatherIo *io, uint32_t generation,
                           WeatherProviderResult result,
                           const WeatherObservation *obs, uint64_t now_us)
{
    if (io == NULL || !io->initialized) {
        return false;
    }
    /*
     * STALE GENERATION. The consumer timed this attempt out, or a newer
     * request superseded it. Either way the answer is the same: count it,
     * report it, and let nothing about it reach the runtime.
     */
    if (!io->in_flight || generation == 0u ||
        generation != io->in_flight_generation) {
        bump(&io->discard_count);
        note(io, WX_IO_RESULT_DISCARDED);
        return false;
    }
    /*
     * The result slot is already occupied. This cannot arise from the single
     * in-flight rule, so it means the machine was driven out of contract;
     * refusing keeps the UNREAD result intact rather than overwriting it.
     */
    if (io->result_present) {
        bump(&io->discard_count);
        note(io, WX_IO_RESULT_DISCARDED);
        return false;
    }

    memset(&io->result, 0, sizeof(io->result));
    io->result.generation  = generation;
    /* From the in-flight identity, NOT from the request slot: the claim
     * already released that slot, and a result must never be stamped from
     * storage whose lifetime ended. */
    io->result.window      = io->in_flight_window;
    io->result.result      = result;
    io->result.started_us  = io->in_flight_started_us;
    io->result.finished_us = now_us;
    if (obs != NULL) {
        io->result.observation = *obs;   /* scalar-only value copy          */
    } else {
        io->result.observation.provider_result = result;
    }
    io->result_present = true;

    if (result == WEATHER_PROVIDER_OK) {
        bump(&io->success_count);
        io->state = WX_IO_RESULT_READY;
        note(io, WX_IO_RESULT_READY);
    } else {
        bump(&io->failure_count);
        /*
         * A FAILED result is still a result: the runtime must see the provider
         * code so the committed W3 retry/backoff can account for the attempt.
         * The slot therefore becomes READY, and `state` reports FAILED only
         * after the consumer has taken it.
         */
        io->state = WX_IO_RESULT_READY;
        note(io, WX_IO_FAILED);
    }
    io->last_result = result;

    /* The attempt is over either way; the slot is free for the next window.
     * `in_flight_window` is cleared with it so a later publish cannot borrow
     * a stale identity. */
    io->in_flight            = false;
    io->in_flight_generation = 0u;
    io->in_flight_started_us = 0u;
    memset(&io->in_flight_window, 0, sizeof(io->in_flight_window));
    return true;
}

/* ------------------------------------------------------------------ */
/* Observation                                                         */
/* ------------------------------------------------------------------ */

void nx_weather_io_observe(const NxWeatherIo *io, uint64_t now_us,
                           NxWeatherIoDiag *out)
{
    uint64_t age_us;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state      = (uint8_t)WX_IO_UNINITIALIZED;
    out->last_event = (uint8_t)WX_IO_UNINITIALIZED;
    out->last_result = (uint8_t)WEATHER_PROVIDER_ERR_UNCONFIGURED;

    if (io == NULL || !io->initialized) {
        return;   /* present stays false: nothing here may be quoted        */
    }

    out->present              = true;
    out->state                = (uint8_t)io->state;
    out->last_event           = (uint8_t)io->last_event;
    out->generation           = io->generation;
    out->in_flight            = io->in_flight;
    out->result_pending       = io->result_present;
    out->submit_count         = io->submit_count;
    out->reject_busy_count    = io->reject_busy_count;
    out->reject_pending_count = io->reject_pending_count;
    out->claim_count          = io->claim_count;
    out->success_count        = io->success_count;
    out->failure_count        = io->failure_count;
    out->timeout_count        = io->timeout_count;
    out->discard_count        = io->discard_count;
    out->consume_count        = io->consume_count;
    out->last_result          = (uint8_t)io->last_result;

    /* MONOTONIC age, saturating, in seconds. Never an epoch, so it discloses
     * nothing about when the device is or where it sits. */
    if (io->in_flight && now_us > io->in_flight_started_us) {
        age_us = now_us - io->in_flight_started_us;
        out->request_age_s = (age_us / 1000000ull) > 0xFFFFFFFFull
                                 ? 0xFFFFFFFFu
                                 : (uint32_t)(age_us / 1000000ull);
    }
}
