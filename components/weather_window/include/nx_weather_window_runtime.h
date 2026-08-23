#ifndef NX_WEATHER_WINDOW_RUNTIME_H_
#define NX_WEATHER_WINDOW_RUNTIME_H_

#include <stdint.h>
#include <stdbool.h>

#include "nx_weather_window.h"
#include "tuning_store.h"

/*
 * NeuralAxe — schedule-window dedup PERSISTENCE OWNER (Phase 2W, Gate W6.4,
 * routed to the internal-RAM NVS owner by Gate W6.4.1).
 *
 * This is the single owner of the committed Gate W2 crash-safe store when it
 * is used for schedule-window deduplication. It holds one TuningStore
 * instance, performs the load/recovery classification once, and performs the
 * durable claim commit. It decides nothing: the decision belongs to the pure
 * nx_weather_window module, and the submission belongs to the Gate W6.3
 * worker.
 *
 * ================== WHY THIS IS ASYNCHRONOUS (Gate W6.4.1) ==================
 *
 * Gate W6.4 originally performed the claim synchronously on whichever task
 * called it. On this product that caller is the statistics task, which
 * main.c creates with xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM) — so ITS
 * STACK LIVES IN PSRAM. Every SPI-flash operation in this build disables the
 * shared flash/PSRAM cache (SPI_FLASH_CACHE_NO_DISABLE is 0: neither flash
 * auto-suspend nor XIP-from-PSRAM is enabled), and ESP-IDF states that a PSRAM
 * task stack may only be used by tasks "where the stack is never accessed
 * while the cache is disabled". A flash write — or read — from that task is
 * therefore an INVALID EXECUTION CONTEXT, not merely a latency concern.
 *
 * The committed project already answers this question: main/nvs_config.c
 * carries the comment "nvs_task heap _must_ be internal memory" above a
 * dedicated internal-RAM writer task fed by a bounded queue. Gate W6.4.1
 * routes the window transaction through that SAME owner rather than inventing
 * a second one.
 *
 * So this module now splits in two halves that never run on the same task:
 *
 *   REQUEST SIDE (the observation task, e.g. statistics): decides, posts a
 *   bounded job, consumes a bounded completion. It NEVER touches flash and
 *   NEVER blocks waiting for one.
 *
 *   EXECUTOR SIDE (the internal-RAM NVS owner task): runs load and claim
 *   transactions through the committed W2 dual-slot path and publishes a
 *   bounded result. It never composes, submits or knows about weather.
 *
 * IT IS NOT A SECOND PERSISTENCE AUTHORITY. Every byte goes through the
 * committed W2 dual-slot algorithm in the existing "nx_wtp" namespace. The B3
 * timed-session namespace "nx_tps" is never opened, named or linked from here.
 *
 * IT CREATES NO TASK, NO QUEUE AND NO TIMER, and it never calls nvs_flash_init
 * or erases anything. The transport to the owner task is INJECTED, so this
 * component depends on nothing in main/.
 *
 * WRITE BUDGET. At most ONE durable claim transaction per newly claimed
 * schedule slot — bounded by the committed slot count, not by tick rate. There
 * is deliberately NO completion write after a successful fetch: the pre-claim
 * already provides the at-most-once property, so a second write would double
 * the flash cost and buy nothing.
 */

/* ------------------------------------------------------------------ */
/* Store lifecycle facts                                               */
/* ------------------------------------------------------------------ */

/*
 * What the persistence authority currently is. The zero is the fail-closed
 * one, so a zeroed observation never reports a usable store.
 *
 * NOT_LOADED now also covers "the load has been requested but the owner task
 * has not run it yet". That is deliberate: a store whose contents are still
 * unknown must never be mistaken for a store that is known to hold no claim.
 */
typedef enum {
    NX_WX_WSTORE_NOT_LOADED = 0,   /* fail-closed zero: nothing loaded yet */
    NX_WX_WSTORE_READY_EMPTY,      /* virgin store — NOT corruption        */
    NX_WX_WSTORE_READY_RECOVERED,  /* a committed record was recovered     */
    NX_WX_WSTORE_UNAVAILABLE,      /* corrupt/unsupported/IO — fail closed */
    NX_WX_WSTORE_INHIBITED,        /* bounded load retries spent; RAM only */
    NX_WX_WSTORE__COUNT
} NxWeatherWindowStoreFact;

const char *nx_weather_window_store_fact_str(NxWeatherWindowStoreFact f);

/* True only for a fact that permits outbound schedule service. A virgin store
 * qualifies: it is a legitimate starting point, and the first window may
 * proceed only by first successfully creating its durable claim. */
bool nx_weather_window_store_serviceable(NxWeatherWindowStoreFact f);

/* ------------------------------------------------------------------ */
/* The canonical window identity                                       */
/* ------------------------------------------------------------------ */

/*
 * WHICH window a job or a credit refers to. Service day plus slot index is the
 * whole identity — the same local time on a different day is a different
 * window, and the served mask is a consequence rather than an identity.
 */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    int8_t   slot;     /* -1 when absent */
} NxWeatherWindowId;

/* ------------------------------------------------------------------ */
/* The asynchronous job and its completion                             */
/* ------------------------------------------------------------------ */

typedef enum {
    NX_WX_WJOB_NONE = 0,           /* no job pending                       */
    NX_WX_WJOB_LOAD,               /* open + load + classify               */
    NX_WX_WJOB_CLAIM,              /* the durable dual-slot claim          */
    NX_WX_WJOB__COUNT
} NxWeatherWindowJobKind;

/*
 * What the executor observed. The zero means "nothing to consume", so a
 * zeroed completion slot can never be read as a durable success.
 *
 * CLAIM_FAILED covers BOTH an outright refusal and W2's
 * TUNING_STORE_COMMIT_UNCERTAIN. They are deliberately not distinguished at
 * this boundary: "uncertain" must never authorize a submission, and the only
 * authority able to resolve it is W2 recovery on the next boot.
 */
typedef enum {
    NX_WX_WDONE_NONE = 0,
    NX_WX_WDONE_LOADED,            /* store classified; see the fact       */
    NX_WX_WDONE_LOAD_FAILED,       /* fail closed; nothing was erased      */
    NX_WX_WDONE_CLAIM_DURABLE,     /* verified commit — and ONLY this      */
    NX_WX_WDONE_CLAIM_FAILED,      /* refused or UNCERTAIN — never submit  */
    NX_WX_WDONE__COUNT
} NxWeatherWindowOutcome;

const char *nx_weather_window_outcome_str(NxWeatherWindowOutcome o);

/*
 * THE GATE VERDICT returned to the caller each tick.
 *
 * REFUSE is the zero so an uninitialised verdict never authorizes anything.
 * WAIT means a transaction is in flight: the caller must do nothing at all,
 * neither submit nor treat the window as spent.
 */
typedef enum {
    NX_WX_GATE_REFUSE = 0,         /* fail-closed zero: submit nothing     */
    NX_WX_GATE_WAIT,               /* a claim is in flight; try next tick  */
    NX_WX_GATE_SUBMIT,             /* durable claim observed — submit ONCE */
    NX_WX_GATE__COUNT
} NxWeatherWindowGate;

const char *nx_weather_window_gate_str(NxWeatherWindowGate g);

/* ------------------------------------------------------------------ */
/* THE GRANT — the durable identity the caller must submit for         */
/* ------------------------------------------------------------------ */

/*
 * What was ACTUALLY committed to flash, as the consumer sees it.
 *
 * The caller MUST build its outbound request from these fields and MUST NOT
 * re-derive the window from a fresh clock or a fresh plan. The flash record
 * names the window; re-deriving it is exactly what would let a midnight
 * rollover between the claim and the submission send a request for a window
 * that was never claimed — a duplicate by another name.
 *
 * `generation` is non-zero for a valid grant, so a zeroed grant authorizes
 * nothing.
 */
typedef struct {
    uint32_t         generation;
    WeatherLocalDate date;         /* EXACTLY what was claimed */
    int8_t           slot_index;   /* EXACTLY what was claimed */
    uint8_t          served_mask;  /* the mask that is now durable */
} NxWeatherWindowGrant;

/* ------------------------------------------------------------------ */
/* The executor seam                                                   */
/* ------------------------------------------------------------------ */

/*
 * How a job reaches the persistence owner task.
 *
 * This is a DOORBELL, not a payload channel: the job itself lives in this
 * module's single pending slot, so nothing that crosses the seam can be torn,
 * no caller-stack pointer can escape, and "at most one pending job" is
 * structural rather than a rule someone has to remember.
 *
 * wake() MUST NOT BLOCK. Returning false means the owner could not accept the
 * wake right now — an ADMISSION failure, categorically different from a
 * persistence failure: NO flash operation has occurred, nothing is claimed,
 * and a later tick simply wakes again. Because the wake carries no payload, a
 * dropped one loses nothing at all: the job is still sitting in the slot.
 */
typedef struct {
    bool (*wake)(void *ctx);
} NxWeatherWindowExecutorOps;

/*
 * Bind the owner-task transport. Must be called before any request, from a
 * context that has already created the owner task and its queue.
 */
void nx_weather_window_runtime_bind_executor(const NxWeatherWindowExecutorOps *ops,
                                             void *ctx);

/* True once an executor transport is bound. */
bool nx_weather_window_runtime_executor_bound(void);

/* ------------------------------------------------------------------ */
/* Bounded, privacy-safe observation                                   */
/* ------------------------------------------------------------------ */

/*
 * Scalars and token ids only. No pointer, no character array, no NVS key, no
 * NVS error text — a coordinate, host, URL or credential is not expressible
 * here. The service day and slot mask are governed scheduling state, not
 * owner-private data.
 */
typedef struct {
    uint8_t  fact;              /* NxWeatherWindowStoreFact token id      */
    bool     ready;             /* service permitted by the store         */
    bool     recovered;         /* a durable claim was read at boot       */
    bool     day_present;       /* a service day is durably claimed       */
    uint16_t service_year;      /* 0 when absent                          */
    uint8_t  service_month;
    uint8_t  service_day;
    uint8_t  served_mask;       /* durably claimed slots for that day     */
    uint8_t  last_result;       /* TuningStoreResult token id             */
    uint8_t  last_decision;     /* NxWeatherWindowDecision token id       */
    uint32_t claim_count;       /* saturating                             */
    uint32_t suppressed_count;  /* saturating                             */
    uint32_t persist_fail_count;/* saturating                             */
    /* Gate W6.4.1 asynchronous routing. */
    uint8_t  job_pending;       /* NxWeatherWindowJobKind token id        */
    uint8_t  last_outcome;      /* NxWeatherWindowOutcome token id        */
    uint32_t enqueue_busy_count;/* owner could not accept — saturating    */
    uint32_t executed_count;    /* jobs the owner actually ran            */
    bool     submit_credit;     /* a durable claim is awaiting its submit */
} NxWeatherWindowDiag;

#define NX_WX_WINDOW_COUNTER_MAX 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* REQUEST SIDE — observation task only                                */
/* ------------------------------------------------------------------ */

/*
 * Bind the injected W2 backend and REQUEST the load. This performs NO flash
 * access: it posts a LOAD job to the owner task and returns immediately.
 *
 * Returns true when the store is already serviceable (a later call after the
 * load completed), false while the answer is still unknown or the store is
 * unusable. A false return is NOT an error — call it again on the next tick.
 *
 * Idempotent: once loaded it is a no-op, and while a load is pending it will
 * not post a second one.
 */
bool nx_weather_window_runtime_begin(const TuningStoreBackendOps *ops, void *ctx);

/* Release the backend and clear the instance. Idempotent. Owner-side. */
void nx_weather_window_runtime_end(void);

NxWeatherWindowStoreFact nx_weather_window_runtime_fact(void);

/* True only when outbound schedule service is permitted by persistence. */
bool nx_weather_window_runtime_ready(void);

/*
 * Copy out the durable window state. Returns false (writing an all-zero state)
 * when nothing trustworthy is published.
 *
 * COPY-OUT, not a borrowed pointer: the record it is derived from is owned by
 * the executor task, so handing out a pointer would let the caller read it
 * while the owner rewrote it.
 */
bool nx_weather_window_runtime_state(TuningScheduleWindowState *out);

/*
 * THE AUTHORIZATION GATE. Decide, request the durable claim, and — one or more
 * ticks later — report that the claim is durable so the caller may submit
 * exactly once.
 *
 * Returns SUBMIT at most once per durable claim. WAIT means a transaction is
 * in flight and the caller must not submit. REFUSE covers every other case and
 * always means zero outbound submissions.
 *
 * This function never blocks and never touches flash.
 */
NxWeatherWindowGate nx_weather_window_authorize(const WeatherSchedulePlan *plan,
                                                uint8_t slot_count,
                                                NxWeatherWindowGrant *out_grant);

/* Record that a window was suppressed (already claimed / regressed), for the
 * bounded diagnostics only. Saturating. */
void nx_weather_window_runtime_note_suppressed(NxWeatherWindowDecision d);

void nx_weather_window_runtime_observe(NxWeatherWindowDiag *out);

/* ------------------------------------------------------------------ */
/* EXECUTOR SIDE — the internal-RAM persistence owner task only        */
/* ------------------------------------------------------------------ */

/*
 * Run the single pending job, if any, and publish its bounded result.
 *
 * MUST be called only from the persistence owner task — the one whose stack is
 * in internal RAM. This is the ONLY function in this component that touches
 * flash. It performs no allocation, holds no lock across the transaction, and
 * never submits, composes or inspects a weather request.
 *
 * Safe to call spuriously: with no pending job it returns immediately.
 */
void nx_weather_window_runtime_execute(void);

/* Test-only: return the owner to its pre-begin state. Never called from
 * production code. */
void nx_weather_window_runtime_reset_for_test(void);

_Static_assert(NX_WX_WSTORE_NOT_LOADED == 0,
               "a zeroed store fact must be the fail-closed token");
_Static_assert(NX_WX_WSTORE__COUNT == 5,
               "W6.4 store facts changed — review tokens and tests");
_Static_assert(NX_WX_WJOB_NONE == 0,
               "a zeroed job slot must mean no job is pending");
_Static_assert(NX_WX_WDONE_NONE == 0,
               "a zeroed completion must never read as a durable success");
_Static_assert(NX_WX_GATE_REFUSE == 0,
               "a zeroed gate verdict must authorize nothing");

#endif /* NX_WEATHER_WINDOW_RUNTIME_H_ */
