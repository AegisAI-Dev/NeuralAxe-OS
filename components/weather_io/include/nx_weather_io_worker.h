#ifndef NX_WEATHER_IO_WORKER_H_
#define NX_WEATHER_IO_WORKER_H_

#include <stdint.h>
#include <stdbool.h>

#include "nx_weather_io.h"
#include "weather_transport.h"

/*
 * NeuralAxe Weather-Aware Tuning — the ONE weather I/O execution context
 * (Phase 2W, Gate W6.3).
 *
 * This is the only file in the product that may run the committed Gate W3
 * synchronous transport. It owns exactly one statically allocated FreeRTOS
 * task, guarded module-wide so a second one cannot be created however many
 * times start() is called — the same single-owner doctrine, and the same
 * xTaskCreateStatic + counter-under-spinlock mechanism, that the committed
 * Gate B6 runtime task uses.
 *
 * The whole implementation is compiled out unless
 * CONFIG_NX_WEATHER_IO_WORKER is set. With the flag off every entry point
 * below is a static inline no-op, so a disabled build contains no worker, no
 * task, no stack, no transport reference and no out-of-line symbol at all.
 *
 * IT IS NOT A SCHEDULER. It never decides that a fetch is due, never retries on
 * its own and never wakes on a timer. It sleeps until the submitter notifies
 * it, performs at most ONE bounded fetch, publishes one bounded result and goes
 * back to sleep. Due-ness belongs to the committed W3 local_schedule; the retry
 * budget and backoff belong to the committed W3 weather_retry; neither is
 * duplicated here.
 */

#ifdef CONFIG_NX_WEATHER_IO_WORKER

/*
 * Worker task stack, in the byte units ESP-IDF's xTaskCreateStatic uses
 * (StackType_t is uint8_t on this port, and the committed Gate B6 task is
 * declared the same way).
 *
 * WHY THIS IS NOT THE 8192 HOUSE SIZE. Every other NeuralAxe task does bounded
 * work on its own data. This one runs a full mbedTLS handshake through
 * esp_crt_bundle_attach: certificate chain parsing and signature verification
 * are the peak stack consumers in the whole firmware, and TLS 1.3 with
 * ChaCha20-Poly1305 is enabled in sdkconfig.defaults. 12 KB is chosen as a
 * conservative margin over the 8 KB house size rather than measured, and the
 * honest consequence is stated in the header of the diagnostics: the worker
 * reports its stack high-water mark on every observation, and a physical pilot
 * must READ that number before this size is treated as proven. It is a
 * deliberately generous starting point to be tightened with evidence, not a
 * value anyone should trust because it appears in a header.
 */
#ifndef CONFIG_NX_WEATHER_IO_WORKER_STACK
#define CONFIG_NX_WEATHER_IO_WORKER_STACK 12288
#endif

/*
 * Priority. The statistics task runs at 3 and the Gate B6 runtime owner at 4;
 * mining (stratum/ASIC) runs at 15-20. The worker sits at 2 — BELOW every one
 * of them — because a weather fetch is the least important work on the device
 * and must never preempt mining, statistics or the trusted-time owner. It
 * blocks on the network, so a low priority costs it nothing.
 */
#define NX_WX_IO_WORKER_PRIORITY 2
#define NX_WX_IO_WORKER_NAME     "nx_wx_io"

/*
 * Start the single worker.
 *
 * THE MACHINE LIVES HERE, NOT IN THE CALLER. The NxWeatherIo state machine is
 * a module-static of the worker translation unit, because the worker module is
 * the one that owns the mutual exclusion. Every entry point below serialises
 * with the SAME portMUX critical section the worker itself uses, so the
 * statistics task and the worker task can never interleave a half-written
 * request, result, state or counter. Handing the machine out to a caller — as
 * an earlier draft of this gate did — left the submitter side unsynchronised
 * and is exactly the cross-task race this arrangement removes.
 *
 * `ops` is the committed W3 transport: production passes
 * weather_open_meteo_http_ops() and every test passes a fake. The worker never
 * selects a transport itself.
 *
 * Idempotent: a second call while the worker runs returns false and creates
 * nothing. Returns true only when this call created the task.
 */
bool nx_weather_io_worker_start(const WeatherTransportOps *ops,
                                void *transport_ctx);

/* ------------------------------------------------------------------ */
/* Serialised submitter-side operations (statistics task)              */
/* ------------------------------------------------------------------ */

/*
 * All four are O(1), never block, never allocate and never touch the network.
 * Each takes the module critical section for the duration of one bounded
 * struct operation — no call below performs I/O, waits on a queue or sleeps.
 */

/* Enforce the bounded in-flight deadline. True when a timeout was recorded. */
bool nx_weather_io_worker_tick(uint64_t now_us);

/* Submit one bounded request and, on acceptance, wake the worker. */
NxWeatherIoSubmitResult nx_weather_io_worker_submit(const NxWeatherIoRequest *req,
                                                    uint64_t now_us);

/* Take the ready result exactly once. `expect` may be NULL. */
bool nx_weather_io_worker_consume(const NxWeatherIoWindow *expect,
                                  NxWeatherIoResult *out, uint64_t now_us);

/* Read the bounded privacy-safe diagnostics. */
void nx_weather_io_worker_observe(uint64_t now_us, NxWeatherIoDiag *out);

/* True while the single worker task exists. */
bool nx_weather_io_worker_running(void);

/* Module-wide worker count. Structurally 0 or 1; exposed so the pilot can
 * PROVE uniqueness rather than assert it. */
uint32_t nx_weather_io_worker_count(void);

/*
 * Wake the worker to look for a submitted request. Called by the submitter
 * immediately after a successful nx_weather_io_submit(). O(1), never blocks:
 * it is a FreeRTOS direct-to-task notification, not a queue send, so there is
 * no capacity to exhaust and nothing to copy.
 */
void nx_weather_io_worker_notify(void);

/*
 * Stack high-water mark of the worker task, in the units FreeRTOS reports
 * (bytes remaining, never used). 0 when no worker exists. Read-only.
 */
uint32_t nx_weather_io_worker_stack_high_water(void);

#else /* !CONFIG_NX_WEATHER_IO_WORKER */

/*
 * Flag off: no worker can exist. These no-ops keep the integrator free of
 * #ifdefs while guaranteeing the disabled image contains no Gate W6.3 worker
 * symbol, exactly as the committed Gate B8 route registration does.
 */
static inline bool nx_weather_io_worker_start(const WeatherTransportOps *ops,
                                              void *transport_ctx)
{
    (void)ops; (void)transport_ctx;
    return false;
}
static inline bool     nx_weather_io_worker_running(void)  { return false; }
static inline uint32_t nx_weather_io_worker_count(void)    { return 0u; }
static inline void     nx_weather_io_worker_notify(void)   { }
static inline uint32_t nx_weather_io_worker_stack_high_water(void) { return 0u; }

static inline bool nx_weather_io_worker_tick(uint64_t now_us)
{
    (void)now_us;
    return false;
}
static inline NxWeatherIoSubmitResult
nx_weather_io_worker_submit(const NxWeatherIoRequest *req, uint64_t now_us)
{
    (void)req; (void)now_us;
    return WX_IO_SUBMIT_REJECTED_UNINITIALIZED;
}
static inline bool nx_weather_io_worker_consume(const NxWeatherIoWindow *expect,
                                                NxWeatherIoResult *out,
                                                uint64_t now_us)
{
    (void)expect; (void)out; (void)now_us;
    return false;
}
static inline void nx_weather_io_worker_observe(uint64_t now_us,
                                                NxWeatherIoDiag *out)
{
    (void)now_us;
    /* Fail closed: present=false, so nothing may be quoted. */
    nx_weather_io_observe(NULL, 0u, out);
}

#endif /* CONFIG_NX_WEATHER_IO_WORKER */

#endif /* NX_WEATHER_IO_WORKER_H_ */
