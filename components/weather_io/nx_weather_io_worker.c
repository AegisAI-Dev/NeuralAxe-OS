/*
 * NeuralAxe Weather-Aware Tuning — the ONE weather I/O worker task
 * (Phase 2W, Gate W6.3).
 *
 * The entire translation unit is compiled out unless
 * CONFIG_NX_WEATHER_IO_WORKER is set, so neither a default image nor a
 * W4-only image nor a W6.2 pilot image contains a byte of it.
 *
 * This file is the ONLY place in the product permitted to execute the
 * committed Gate W3 synchronous transport. It creates no second transport, no
 * second scheduler, no second trusted-time provider and no persistence; it
 * opens no NVS namespace and has no API through which it could change a
 * frequency, a voltage, a fan, a pool, a protocol or a stored byte.
 */

#include "sdkconfig.h"

#ifdef CONFIG_NX_WEATHER_IO_WORKER

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nx_weather_io.h"
#include "nx_weather_io_worker.h"
#include "weather_runtime.h"    /* weather_fetch_execute                    */
#include "weather_transport.h"

static const char *TAG = "nx_wx_io";

/*
 * Module-wide singleton state. `s_worker_count` under the spinlock is the
 * uniqueness proof: it is checked and claimed atomically, exactly as the
 * committed Gate B6 runtime does with s_task_count, so two concurrent start()
 * calls cannot both create a task.
 */
static portMUX_TYPE  s_io_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_worker_count;
static TaskHandle_t  s_worker_handle;
static const WeatherTransportOps *s_ops;
static void         *s_ctx;

/*
 * THE state machine, owned by the module that owns the lock.
 *
 * Both tasks reach it only through the helpers below, every one of which holds
 * s_io_lock for the duration of one bounded struct operation. Nothing inside a
 * critical section performs I/O, blocks, allocates or waits: the longest is a
 * ~260 byte request copy, which is well under a microsecond at 240 MHz and is
 * the same shape of bounded snapshot copy the committed Gate B6 runtime makes
 * under its own portMUX.
 *
 * This is why `volatile` appears nowhere in this gate: mutual exclusion plus
 * the memory barriers portENTER/EXIT_CRITICAL already imply is the actual
 * synchronisation, and volatile would neither add ordering nor make a
 * multi-field struct update atomic.
 */
static NxWeatherIo   s_io;

/* Static task storage. No heap is used for the task, its stack or its TCB, so
 * a completed request cannot leak and repeated starts cannot fragment. */
static StaticTask_t s_worker_tcb;
static StackType_t  s_worker_stack[CONFIG_NX_WEATHER_IO_WORKER_STACK];

/*
 * The single bounded response staging buffer (~4 KB), owned by this module and
 * touched ONLY by the worker task. It is deliberately NOT the function-static
 * buffer inside weather_runtime_fetch(): that one belongs to the synchronous
 * integrator path, and sharing it would reintroduce exactly the cross-task
 * aliasing this gate exists to remove.
 */
static WeatherHttpResponse s_response;

/* Serialised entry into the pure state machine. Every touch of *s_io goes
 * through one of these two helpers, so the machine's invariants hold without
 * the pure core needing to know a scheduler exists. */
static bool io_claim(NxWeatherIoRequest *out, uint64_t now_us)
{
    bool got;

    portENTER_CRITICAL(&s_io_lock);
    got = nx_weather_io_claim(&s_io, out, now_us);
    portEXIT_CRITICAL(&s_io_lock);
    return got;
}

static void io_publish(uint32_t generation, WeatherProviderResult r,
                       const WeatherObservation *obs, uint64_t now_us)
{
    portENTER_CRITICAL(&s_io_lock);
    (void)nx_weather_io_publish(&s_io, generation, r, obs, now_us);
    portEXIT_CRITICAL(&s_io_lock);
}

/* ------------------------------------------------------------------ */
/* Serialised submitter-side operations                                */
/* ------------------------------------------------------------------ */

bool nx_weather_io_worker_tick(uint64_t now_us)
{
    bool timed_out;

    portENTER_CRITICAL(&s_io_lock);
    timed_out = nx_weather_io_tick(&s_io, now_us);
    portEXIT_CRITICAL(&s_io_lock);
    return timed_out;
}

NxWeatherIoSubmitResult nx_weather_io_worker_submit(const NxWeatherIoRequest *req,
                                                    uint64_t now_us)
{
    NxWeatherIoSubmitResult r;

    portENTER_CRITICAL(&s_io_lock);
    r = nx_weather_io_submit(&s_io, req, now_us);
    portEXIT_CRITICAL(&s_io_lock);

    /*
     * The wake happens OUTSIDE the critical section. xTaskNotifyGive can cause
     * a yield, and yielding with interrupts disabled is exactly the kind of
     * thing that turns a diagnostic feature into a stability bug on a mining
     * device. The notification is idempotent and cannot be lost: the worker
     * re-checks the slot after every take.
     */
    if (r == WX_IO_SUBMIT_ACCEPTED) {
        nx_weather_io_worker_notify();
    }
    return r;
}

bool nx_weather_io_worker_consume(const NxWeatherIoWindow *expect,
                                  NxWeatherIoResult *out, uint64_t now_us)
{
    bool got;

    portENTER_CRITICAL(&s_io_lock);
    got = nx_weather_io_consume(&s_io, expect, out, now_us);
    portEXIT_CRITICAL(&s_io_lock);
    return got;
}

void nx_weather_io_worker_observe(uint64_t now_us, NxWeatherIoDiag *out)
{
    portENTER_CRITICAL(&s_io_lock);
    nx_weather_io_observe(&s_io, now_us, out);
    portEXIT_CRITICAL(&s_io_lock);
}

/*
 * The worker loop.
 *
 * IDLE BEHAVIOUR: blocked indefinitely in ulTaskNotifyTake. It consumes no CPU
 * while waiting, holds no lock, and cannot spin — there is no polling delay and
 * no timeout-driven wakeup, so a device that never has due weather runs this
 * task exactly zero times after boot.
 *
 * AT MOST ONE IN FLIGHT: the loop claims at most one request per wakeup and
 * runs it to completion before looking again. The pure core enforces the same
 * rule from the submitter side, so the bound holds even if this loop were
 * driven differently.
 *
 * NO RETRY HERE: a failure is published as a failure. The committed W3
 * weather_retry policy, evaluated by the submitter, decides whether another
 * attempt happens. Retrying here would be a second, unbounded policy.
 */
static void weather_io_worker_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "weather io worker started (prio=%d stack=%u)",
             NX_WX_IO_WORKER_PRIORITY,
             (unsigned)CONFIG_NX_WEATHER_IO_WORKER_STACK);

    for (;;) {
        NxWeatherIoRequest   req;
        WeatherObservation   obs;
        WeatherProviderResult r;

        /* Sleep until the submitter has something. Notifications coalesce, so
         * a burst of wakeups costs one pass, never a queue. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (io_claim(&req, (uint64_t)esp_timer_get_time())) {
            memset(&obs, 0, sizeof(obs));

            /*
             * THE committed Gate W3 sequence, through the single shared
             * implementation: ops->fetch -> weather_transport_validate ->
             * weather_open_meteo_parse. This worker supplies the scratch
             * buffer and its own identity; it passes NULL for the fetch
             * counter because the request generation IS its identity.
             *
             * This call is the blocking one, and it is the reason this task
             * exists. Nothing else in the loop can block.
             */
            r = weather_fetch_execute(s_ops, s_ctx, &req.request, &req.parse,
                                      NULL, &s_response, &obs);

            /* Never leave a response body resident between requests. */
            memset(&s_response, 0, sizeof(s_response));

            /*
             * Publish under the lock. If the submitter already timed this
             * attempt out, the generation no longer matches and the pure core
             * discards it — the worker neither knows nor needs to.
             */
            io_publish(req.generation, r, &obs, (uint64_t)esp_timer_get_time());

            /* The observation is bounded and scalar-only; clear it anyway so
             * nothing survives into the next iteration's frame. */
            memset(&obs, 0, sizeof(obs));
            memset(&req, 0, sizeof(req));
        }
    }
}

bool nx_weather_io_worker_start(const WeatherTransportOps *ops,
                                void *transport_ctx)
{
    TaskHandle_t h;

    if (ops == NULL || ops->fetch == NULL) {
        return false;   /* no transport => no worker; fail passive          */
    }

    portENTER_CRITICAL(&s_io_lock);
    if (s_worker_count != 0u) {
        portEXIT_CRITICAL(&s_io_lock);
        return false;   /* exactly one, module-wide, however often called   */
    }
    s_worker_count = 1u;
    nx_weather_io_init(&s_io);
    s_ops = ops;
    s_ctx = transport_ctx;
    portEXIT_CRITICAL(&s_io_lock);

    h = xTaskCreateStatic(weather_io_worker_task, NX_WX_IO_WORKER_NAME,
                          CONFIG_NX_WEATHER_IO_WORKER_STACK, NULL,
                          NX_WX_IO_WORKER_PRIORITY, s_worker_stack,
                          &s_worker_tcb);
    if (h == NULL) {
        /* Creation failed: release the claim so a later boot-stage retry is
         * possible, and leave the state machine untouched and usable. */
        portENTER_CRITICAL(&s_io_lock);
        s_worker_count = 0u;
        s_ops = NULL;
        s_ctx = NULL;
        portEXIT_CRITICAL(&s_io_lock);
        ESP_LOGE(TAG, "weather io worker creation failed");
        return false;
    }
    s_worker_handle = h;
    return true;
}

bool nx_weather_io_worker_running(void)
{
    return s_worker_handle != NULL;
}

uint32_t nx_weather_io_worker_count(void)
{
    uint32_t n;

    portENTER_CRITICAL(&s_io_lock);
    n = s_worker_count;
    portEXIT_CRITICAL(&s_io_lock);
    return n;
}

void nx_weather_io_worker_notify(void)
{
    TaskHandle_t h = s_worker_handle;

    if (h != NULL) {
        /* O(1), never blocks, no capacity to exhaust. Repeated notifications
         * coalesce into one pending count, which the loop drains. */
        xTaskNotifyGive(h);
    }
}

uint32_t nx_weather_io_worker_stack_high_water(void)
{
    TaskHandle_t h = s_worker_handle;

    if (h == NULL) {
        return 0u;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(h);
}

#endif /* CONFIG_NX_WEATHER_IO_WORKER */
