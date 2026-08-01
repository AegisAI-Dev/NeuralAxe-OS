#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "protocol_coordinator.h"
#include "stratum_v1_task.h"
#include "stratum_v2_task.h"
#include "connect.h"
#include "system.h"
#include "nvs_config.h"
#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
#include "pool_session_execution.h"
#endif

#include <string.h>

// Internal coordinator states
typedef enum {
    COORD_STATE_IDLE = 0,
    COORD_STATE_RUNNING_PRIMARY,
    COORD_STATE_RUNNING_FALLBACK,
    // Entered when consecutive pool failures hit the configured threshold.
    // No protocol task is running; the coordinator probes pools periodically
    // and resumes mining as soon as one is reachable. While in this state,
    // pools_unavailable is true so power management cuts ASIC power.
    COORD_STATE_PAUSED,
} coordinator_state_t;

// Internal event types
typedef enum {
    COORD_EVENT_PROTOCOL_FAILED = 0,
    COORD_EVENT_PROTOCOL_SUCCESS,
    COORD_EVENT_V1_TASK_EXITED,
    COORD_EVENT_V2_TASK_EXITED,
} coordinator_event_t;

#define TRANSPORT_TIMEOUT_MS 5000
#define HEARTBEAT_INTERVAL_MS 60000
#define INITIAL_HEARTBEAT_DELAY_MS 10000
// While paused (all pools unreachable), probe again on this cadence.
#define RECOVERY_PROBE_INTERVAL_MS 30000
#define BUFFER_SIZE 1024

static const char *TAG = "protocol_coordinator";

static GlobalState *s_global_state = NULL;
static coordinator_state_t s_state = COORD_STATE_IDLE;
static QueueHandle_t s_event_queue = NULL;
static volatile bool s_v1_should_shutdown = false;
static volatile bool s_v2_should_shutdown = false;

// Protocol tracking
static stratum_protocol_t s_primary_protocol;
static stratum_protocol_t s_fallback_protocol;
static stratum_protocol_t s_running_protocol;
static bool s_heartbeat_enabled = false;

// Primary pool info (saved at startup for heartbeat probing).
// NeuralAxe Gate B7 reader contract: this is a bounded CALLER-OWNED COPY,
// never a retained pointer into the published identity storage. It is used
// only for diagnostics; every probe re-reads the live configuration.
#define NX_PRIMARY_URL_MAX 80
static char s_primary_url[NX_PRIMARY_URL_MAX] = {0};
static uint16_t s_primary_port = 0;

// Number of consecutive pools (primary and/or fallback) that have exhausted
// their retry budget without a successful setup. When this reaches
// pool_failure_threshold(), we enter COORD_STATE_PAUSED and set
// pools_unavailable so power management cuts ASIC power.
// Reset on COORD_EVENT_PROTOCOL_SUCCESS.
static int s_consecutive_pool_failures = 0;

void protocol_coordinator_init(GlobalState *gs)
{
    s_global_state = gs;
    s_event_queue = xQueueCreate(8, sizeof(coordinator_event_t));
    s_v1_should_shutdown = false;
    s_v2_should_shutdown = false;
    s_heartbeat_enabled = false;
    s_consecutive_pool_failures = 0;
}

void protocol_coordinator_notify_failure(void)
{
    coordinator_event_t evt = COORD_EVENT_PROTOCOL_FAILED;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &evt, 0);
    }
}

void protocol_coordinator_notify_success(void)
{
    coordinator_event_t evt = COORD_EVENT_PROTOCOL_SUCCESS;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &evt, 0);
    }
}

bool protocol_coordinator_v1_should_shutdown(void)
{
    return s_v1_should_shutdown;
}

void protocol_coordinator_v1_exited(void)
{
    coordinator_event_t evt = COORD_EVENT_V1_TASK_EXITED;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &evt, 0);
    }
}

bool protocol_coordinator_v2_should_shutdown(void)
{
    return s_v2_should_shutdown;
}

void protocol_coordinator_v2_exited(void)
{
    coordinator_event_t evt = COORD_EVENT_V2_TASK_EXITED;
    if (s_event_queue) {
        xQueueSend(s_event_queue, &evt, 0);
    }
}

static void reset_share_stats(GlobalState *gs)
{
    for (int i = 0; i < gs->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
        gs->SYSTEM_MODULE.rejected_reason_stats[i].count = 0;
        gs->SYSTEM_MODULE.rejected_reason_stats[i].message[0] = '\0';
    }
    gs->SYSTEM_MODULE.rejected_reason_stats_count = 0;
    gs->SYSTEM_MODULE.shares_accepted = 0;
    gs->SYSTEM_MODULE.shares_rejected = 0;
    gs->SYSTEM_MODULE.work_received = 0;
}

static bool has_fallback_pool(GlobalState *gs)
{
    return (gs->SYSTEM_MODULE.fallback_pool_url != NULL &&
            gs->SYSTEM_MODULE.fallback_pool_url[0] != '\0');
}

// Start the V1 stratum task (for primary V1 or fallback)
static void start_v1_task(GlobalState *gs)
{
    s_v1_should_shutdown = false;
    if (xTaskCreate(stratum_v1_task, "stratum v1", 8192, (void *)gs, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create V1 stratum task");
    }
}

// Start the V2 stratum task
static void start_v2_task(GlobalState *gs)
{
    s_v2_should_shutdown = false;
    if (xTaskCreate(stratum_v2_task, "stratum v2", 12288, (void *)gs, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create V2 stratum task");
    }
}

// Start a task for the given protocol
static void start_protocol_task(GlobalState *gs, stratum_protocol_t protocol)
{
    if (protocol == STRATUM_PROTOCOL_V2) {
        start_v2_task(gs);
    } else {
        start_v1_task(gs);
    }
}

// Tell the V1 task to shut down and wait for it to exit.
// Only closes the transport socket to unblock V1's recv — does NOT destroy it.
// The V1 task handles its own full cleanup (destroy, queue clear) on exit.
// Returns true when the task provably exited within the bounded wait.
static bool stop_v1_task(GlobalState *gs)
{
    s_v1_should_shutdown = true;

    // Close transport to unblock V1's blocked recv()
    if (gs->transport) {
        esp_transport_close(gs->transport);
    }

    coordinator_event_t evt;
    for (int i = 0; i < 100; i++) {
        if (xQueueReceive(s_event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (evt == COORD_EVENT_V1_TASK_EXITED || evt == COORD_EVENT_PROTOCOL_FAILED) {
                ESP_LOGI(TAG, "V1 task exited cleanly");
                return true;
            }
        }
    }
    ESP_LOGW(TAG, "V1 task did not exit within timeout");
    return false;
}

// Tell the V2 task to shut down and wait for it to exit.
// Only closes the transport socket to unblock V2's recv — does NOT destroy it.
// The V2 task handles its own full cleanup (destroy, noise ctx, queue clear) on exit.
// Returns true when the task provably exited within the bounded wait.
static bool stop_v2_task(GlobalState *gs)
{
    s_v2_should_shutdown = true;

    // Close transport to unblock V2's blocked recv()
    if (gs->transport) {
        esp_transport_close(gs->transport);
    }

    coordinator_event_t evt;
    for (int i = 0; i < 100; i++) {
        if (xQueueReceive(s_event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (evt == COORD_EVENT_V2_TASK_EXITED || evt == COORD_EVENT_PROTOCOL_FAILED) {
                ESP_LOGI(TAG, "V2 task exited cleanly");
                return true;
            }
        }
    }
    ESP_LOGW(TAG, "V2 task did not exit within timeout");
    return false;
}

// Stop the currently running protocol task
static void stop_running_task(GlobalState *gs)
{
    if (s_running_protocol == STRATUM_PROTOCOL_V2) {
        stop_v2_task(gs);
    } else {
        stop_v1_task(gs);
    }
}

// TCP connect probe (used for SV2 — full noise handshake is too expensive)
static bool probe_pool_sv2(const char *url, uint16_t port)
{
    if (url == NULL || url[0] == '\0' || port == 0) return false;

    esp_transport_handle_t probe = esp_transport_tcp_init();
    if (!probe) return false;

    esp_err_t err = esp_transport_connect(probe, url, port, TRANSPORT_TIMEOUT_MS);
    esp_transport_close(probe);
    esp_transport_destroy(probe);

    return (err == ESP_OK);
}

// Subscribe/authorize probe for V1 — succeeds only if the pool responds with
// a mining.notify line, confirming it's actually serving work.
static bool probe_pool_v1(GlobalState *gs, const char *url, uint16_t port,
                          tls_mode tls, char *cert, const char *user, const char *pass)
{
    if (url == NULL || url[0] == '\0' || port == 0) return false;

    esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, cert);
    if (!transport) return false;

    esp_err_t err = esp_transport_connect(transport, url, port, TRANSPORT_TIMEOUT_MS);
    if (err != ESP_OK) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
        return false;
    }

    int send_uid = 1;
    STRATUM_V1_subscribe(transport, send_uid++, gs->DEVICE_CONFIG.family.asic.name);
    STRATUM_V1_authorize(transport, send_uid++, user, pass);

    char recv_buffer[BUFFER_SIZE];
    memset(recv_buffer, 0, BUFFER_SIZE);
    int bytes_received = esp_transport_read(transport, recv_buffer, BUFFER_SIZE - 1, TRANSPORT_TIMEOUT_MS);

    esp_transport_close(transport);
    esp_transport_destroy(transport);

    return (bytes_received > 0 && strstr(recv_buffer, "mining.notify") != NULL);
}

// Probe a pool using the appropriate protocol for it.
static bool probe_pool(GlobalState *gs, bool use_fallback)
{
    stratum_protocol_t protocol = use_fallback ? s_fallback_protocol : s_primary_protocol;
    const char *url   = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_url   : gs->SYSTEM_MODULE.pool_url;
    uint16_t    port  = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_port  : gs->SYSTEM_MODULE.pool_port;

    if (protocol == STRATUM_PROTOCOL_V2) {
        return probe_pool_sv2(url, port);
    }

    tls_mode tls       = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_tls  : gs->SYSTEM_MODULE.pool_tls;
    char     *cert     = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_cert : gs->SYSTEM_MODULE.pool_cert;
    const char *user   = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_user : gs->SYSTEM_MODULE.pool_user;
    const char *pass   = use_fallback ? gs->SYSTEM_MODULE.fallback_pool_pass : gs->SYSTEM_MODULE.pool_pass;
    return probe_pool_v1(gs, url, port, tls, cert, user, pass);
}

// Switch from primary to fallback pool.
// The failed task has already exited (it sent PROTOCOL_FAILED then deleted itself).
static void switch_to_fallback(GlobalState *gs)
{
    queue_clear(&gs->stratum_queue);
    reset_share_stats(gs);

    gs->SYSTEM_MODULE.is_using_fallback = true;
    gs->stratum_protocol = s_fallback_protocol;
    s_running_protocol = s_fallback_protocol;
    s_state = COORD_STATE_RUNNING_FALLBACK;

    ESP_LOGI(TAG, "Switching to fallback pool (%s)",
             s_fallback_protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1);

    start_protocol_task(gs, s_fallback_protocol);

    // Only enable heartbeat if this was an automatic failover (not user choice)
    s_heartbeat_enabled = !gs->SYSTEM_MODULE.use_fallback_stratum;
}

// Switch from fallback back to primary pool.
// Must stop the running fallback task first.
static void switch_to_primary(GlobalState *gs)
{
    ESP_LOGI(TAG, "Primary pool is back! Switching from fallback.");

    stop_running_task(gs);

    queue_clear(&gs->stratum_queue);
    reset_share_stats(gs);

    gs->SYSTEM_MODULE.is_using_fallback = false;
    gs->stratum_protocol = s_primary_protocol;
    s_running_protocol = s_primary_protocol;
    s_state = COORD_STATE_RUNNING_PRIMARY;

    start_protocol_task(gs, s_primary_protocol);

    s_heartbeat_enabled = false;
}

// Non-blocking heartbeat probe — called when the heartbeat timer expires
static void do_heartbeat_probe(GlobalState *gs)
{
    // Never auto-switch back if user explicitly chose fallback
    if (gs->SYSTEM_MODULE.use_fallback_stratum) {
        s_heartbeat_enabled = false;
        return;
    }

    if (!wifi_is_connected()) {
        return;
    }

    ESP_LOGD(TAG, "Heartbeat: probing primary pool %s:%d", s_primary_url, s_primary_port);

    if (probe_pool(gs, /*use_fallback=*/false)) {
        switch_to_primary(gs);
    } else {
        ESP_LOGD(TAG, "Primary pool still unreachable");
    }
}

// Number of consecutive pool failures that triggers entering the paused state.
// With both primary and fallback configured we tolerate one failure per pool;
// with only one pool configured we pause on its first exhaustion.
static int pool_failure_threshold(GlobalState *gs)
{
    return has_fallback_pool(gs) ? 2 : 1;
}

// All configured pools have exhausted retries. Set pools_unavailable so power
// management cuts ASIC power, and park the coordinator until a probe succeeds.
static void enter_paused_state(GlobalState *gs)
{
    s_state = COORD_STATE_PAUSED;
    gs->SYSTEM_MODULE.pools_unavailable = true;
    s_heartbeat_enabled = false;
    ESP_LOGW(TAG, "All configured pools unreachable, pausing mining to conserve power.");
}

// Resume mining on the given pool after a successful recovery probe.
// Used only from the paused state — caller must have already verified the
// pool is reachable.
static void resume_on_pool(GlobalState *gs, bool use_fallback)
{
    s_consecutive_pool_failures = 0;
    gs->SYSTEM_MODULE.pools_unavailable = false;
    gs->SYSTEM_MODULE.is_using_fallback = use_fallback;

    stratum_protocol_t proto = use_fallback ? s_fallback_protocol : s_primary_protocol;
    gs->stratum_protocol = proto;
    s_running_protocol = proto;
    s_state = use_fallback ? COORD_STATE_RUNNING_FALLBACK : COORD_STATE_RUNNING_PRIMARY;

    queue_clear(&gs->stratum_queue);
    reset_share_stats(gs);

    ESP_LOGI(TAG, "Pool recovery: %s pool reachable, resuming mining (%s)",
             use_fallback ? "fallback" : "primary",
             proto == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1);

    start_protocol_task(gs, proto);

    // Only run the auto-switch-back heartbeat for *automatic* failovers
    // (user did not explicitly choose the fallback pool).
    s_heartbeat_enabled = use_fallback && !gs->SYSTEM_MODULE.use_fallback_stratum;
}

// Probe pools while paused. Tries the user-preferred pool first, then the other.
// Resumes mining as soon as one is reachable.
static void try_resume_from_paused(GlobalState *gs)
{
    if (!wifi_is_connected()) {
        return;
    }

    bool prefer_fallback = gs->SYSTEM_MODULE.use_fallback_stratum && has_fallback_pool(gs);

    if (prefer_fallback) {
        if (probe_pool(gs, /*use_fallback=*/true)) {
            resume_on_pool(gs, /*use_fallback=*/true);
            return;
        }
        if (probe_pool(gs, /*use_fallback=*/false)) {
            resume_on_pool(gs, /*use_fallback=*/false);
            return;
        }
    } else {
        if (probe_pool(gs, /*use_fallback=*/false)) {
            resume_on_pool(gs, /*use_fallback=*/false);
            return;
        }
        if (has_fallback_pool(gs) && probe_pool(gs, /*use_fallback=*/true)) {
            resume_on_pool(gs, /*use_fallback=*/true);
            return;
        }
    }

    ESP_LOGD(TAG, "Recovery probe: no pool reachable, staying paused");
}

// Handle an event from the event queue
static void handle_event(GlobalState *gs, coordinator_event_t evt)
{
    switch (evt) {
        case COORD_EVENT_PROTOCOL_FAILED: {
            if (s_state == COORD_STATE_PAUSED) {
                // Stray failure from a task that exited after we already paused — ignore.
                break;
            }
            s_consecutive_pool_failures++;
            int threshold = pool_failure_threshold(gs);
            ESP_LOGW(TAG, "Protocol failure reported (state=%d, failures=%d/%d)",
                     s_state, s_consecutive_pool_failures, threshold);

            if (s_consecutive_pool_failures >= threshold) {
                enter_paused_state(gs);
                break;
            }

            // Below threshold — try the other pool. This only fires when a
            // fallback exists (otherwise threshold=1 and we paused above).
            if (s_state == COORD_STATE_RUNNING_PRIMARY) {
                switch_to_fallback(gs);
            } else if (s_state == COORD_STATE_RUNNING_FALLBACK) {
                ESP_LOGI(TAG, "Fallback failed, trying primary");
                queue_clear(&gs->stratum_queue);
                reset_share_stats(gs);
                gs->SYSTEM_MODULE.is_using_fallback = false;
                gs->stratum_protocol = s_primary_protocol;
                s_running_protocol = s_primary_protocol;
                s_state = COORD_STATE_RUNNING_PRIMARY;
                start_protocol_task(gs, s_primary_protocol);
                s_heartbeat_enabled = false;
            }
            break;
        }

        case COORD_EVENT_PROTOCOL_SUCCESS:
            if (s_consecutive_pool_failures > 0 || gs->SYSTEM_MODULE.pools_unavailable) {
                ESP_LOGI(TAG, "Pool connection succeeded — clearing failure state");
            }
            s_consecutive_pool_failures = 0;
            gs->SYSTEM_MODULE.pools_unavailable = false;
            break;

        case COORD_EVENT_V1_TASK_EXITED:
        case COORD_EVENT_V2_TASK_EXITED:
            // These come from clean coordinator-requested shutdowns (via stop functions).
            // They're consumed by stop_v1_task/stop_v2_task during switch_to_primary.
            // If we receive one here unexpectedly, just log it.
            ESP_LOGI(TAG, "Task exited event received (evt=%d, state=%d)", evt, s_state);
            break;
    }
}

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
// Set once the coordinator task entered; the controlled B7 hooks refuse to
// run while the production coordinator owns the protocol.
static volatile bool s_coordinator_task_started = false;
#endif

void protocol_coordinator_task(void *pvParameters)
{
    GlobalState *gs = (GlobalState *)pvParameters;

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    s_coordinator_task_started = true;
#endif

    if (gs->SYSTEM_MODULE.pool_url != NULL) {
        strncpy(s_primary_url, gs->SYSTEM_MODULE.pool_url, sizeof(s_primary_url) - 1);
        s_primary_url[sizeof(s_primary_url) - 1] = '\0';
    } else {
        s_primary_url[0] = '\0';
    }
    s_primary_port = gs->SYSTEM_MODULE.pool_port;
    s_primary_protocol = gs->stratum_protocol;
    s_fallback_protocol = gs->SYSTEM_MODULE.fallback_pool_protocol;

    // Start initial protocol task
    if (gs->SYSTEM_MODULE.is_using_fallback) {
        // User explicitly selected fallback — use fallback protocol
        gs->stratum_protocol = s_fallback_protocol;
        s_running_protocol = s_fallback_protocol;
        s_state = COORD_STATE_RUNNING_FALLBACK;
        start_protocol_task(gs, s_fallback_protocol);
        // User chose fallback, no heartbeat
        s_heartbeat_enabled = false;
    } else {
        s_running_protocol = s_primary_protocol;
        s_state = COORD_STATE_RUNNING_PRIMARY;
        start_protocol_task(gs, s_primary_protocol);
    }

    ESP_LOGI(TAG, "Protocol coordinator started (primary: %s, fallback: %s, state: %d)",
             s_primary_protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1,
             s_fallback_protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1,
             s_state);

    // Heartbeat initial delay state — give fallback connection time to establish
    // before probing primary pool
    bool heartbeat_initial_delay = false;
    int64_t heartbeat_delay_start = 0;

    // Main non-blocking event loop
    while (1) {
        coordinator_event_t evt;
        TickType_t wait;
        if (s_state == COORD_STATE_PAUSED) {
            wait = pdMS_TO_TICKS(RECOVERY_PROBE_INTERVAL_MS);
        } else if (s_heartbeat_enabled) {
            wait = pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
        } else {
            wait = portMAX_DELAY;
        }

        bool was_heartbeat_enabled = s_heartbeat_enabled;

        if (xQueueReceive(s_event_queue, &evt, wait) == pdTRUE) {
            handle_event(gs, evt);

            // Detect heartbeat disabled→enabled transition, reset initial delay
            if (s_heartbeat_enabled && !was_heartbeat_enabled) {
                heartbeat_initial_delay = true;
                heartbeat_delay_start = esp_timer_get_time();
            }
        } else if (s_state == COORD_STATE_PAUSED) {
            // Recovery probe — try to bring a pool back online.
            try_resume_from_paused(gs);
        } else if (s_heartbeat_enabled) {
            // Timeout expired — time for a heartbeat probe
            if (heartbeat_initial_delay) {
                int64_t elapsed_ms = (esp_timer_get_time() - heartbeat_delay_start) / 1000;
                if (elapsed_ms < INITIAL_HEARTBEAT_DELAY_MS) {
                    continue;
                }
                heartbeat_initial_delay = false;
            }
            do_heartbeat_probe(gs);
        }
    }

    vTaskDelete(NULL);
}

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
/*
 * NeuralAxe Gate B7 controlled protocol hooks.
 *
 * Single caller: the timed-session executor on the B6 runtime owner task,
 * and ONLY while the coordinator task is not running (the boot barrier
 * withheld it in every posture where the executor acts, and every hook
 * re-checks). The hooks reuse the coordinator's internal start/stop/event
 * machinery so exactly one protocol engine exists, and they log machine
 * facts only — never a pool identity.
 */

static bool               s_ctrl_running  = false;
static stratum_protocol_t s_ctrl_protocol = STRATUM_PROTOCOL_V1;
/*
 * Gate B7 reader contract (slot borrow/release): a controlled protocol
 * instance reads the published identity pointers for its WHOLE lifetime, so
 * it holds a reference on the exact slot from start to proven exit. A
 * referenced slot is never reclaimed, so the instance's identity is stable
 * and valid for as long as the instance exists. UINT32_MAX = no borrow.
 */
static uint32_t s_ctrl_identity_slot = UINT32_MAX;

static void ctrl_release_identity(void)
{
    if (s_ctrl_identity_slot != UINT32_MAX) {
        pool_session_execution_identity_release(s_ctrl_identity_slot);
        s_ctrl_identity_slot = UINT32_MAX;
    }
}

bool nx_protocol_ctrl_start(stratum_protocol_t protocol)
{
    GlobalState *gs = s_global_state;

    if (gs == NULL || s_event_queue == NULL || s_coordinator_task_started ||
        s_ctrl_running) {
        return false;
    }
    if (protocol != STRATUM_PROTOCOL_V1 && protocol != STRATUM_PROTOCOL_V2) {
        return false;
    }

    // Drain every stale event so anything polled afterwards belongs to the
    // new controlled connection generation.
    coordinator_event_t evt;
    while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
    }

    // Fresh-generation baselines: empty job queue, zeroed share/work stats.
    queue_clear(&gs->stratum_queue);
    reset_share_stats(gs);

    // Controlled sessions pin the PRIMARY endpoint as the verified identity.
    gs->SYSTEM_MODULE.is_using_fallback = false;
    gs->stratum_protocol = protocol;
    s_ctrl_protocol      = protocol;
    s_running_protocol   = protocol;

    // Gate B7: no autonomous restart may originate inside the protocol
    // instance this session owns. Set BEFORE the task starts.
    STRATUM_V1_set_restart_inhibited(true);

    // Gate B7 reader contract: pin the identity slot this instance will read
    // for its whole lifetime. Taken BEFORE the task starts so the reference
    // exists before the first dereference.
    ctrl_release_identity();
    (void)pool_session_execution_identity_borrow(&s_ctrl_identity_slot);

    start_protocol_task(gs, protocol);
    s_ctrl_running = true;
    ESP_LOGI(TAG, "Controlled session protocol start (%s)",
             protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1);
    return true;
}

bool nx_protocol_ctrl_stop(void)
{
    GlobalState *gs = s_global_state;
    bool ok;

    if (gs == NULL || !s_ctrl_running) {
        return true; // nothing controlled is running
    }
    ok = (s_ctrl_protocol == STRATUM_PROTOCOL_V2) ? stop_v2_task(gs)
                                                  : stop_v1_task(gs);
    if (ok) {
        s_ctrl_running = false;
        queue_clear(&gs->stratum_queue);
        // The controlled instance provably exited: it can no longer
        // dereference the identity, so release its slot reference.
        ctrl_release_identity();
        // Restore the stock autonomous recovery behavior for any later
        // production-owned instance.
        STRATUM_V1_set_restart_inhibited(false);
    }
    return ok;
}

bool nx_protocol_ctrl_running(void)
{
    return s_ctrl_running;
}

uint32_t nx_protocol_ctrl_poll_events(void)
{
    uint32_t bits = 0u;
    coordinator_event_t evt;

    if (s_event_queue == NULL) {
        return 0u;
    }
    while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
        switch (evt) {
            case COORD_EVENT_PROTOCOL_FAILED:
                // The stratum task deletes itself after reporting failure.
                bits |= NX_PROTOCOL_EVT_FAILED | NX_PROTOCOL_EVT_TASK_EXITED;
                s_ctrl_running = false;
                ctrl_release_identity();
                STRATUM_V1_set_restart_inhibited(false);
                break;
            case COORD_EVENT_PROTOCOL_SUCCESS:
                bits |= NX_PROTOCOL_EVT_SETUP_SUCCESS;
                break;
            case COORD_EVENT_V1_TASK_EXITED:
            case COORD_EVENT_V2_TASK_EXITED:
                bits |= NX_PROTOCOL_EVT_TASK_EXITED;
                s_ctrl_running = false;
                break;
        }
    }
    return bits;
}

void nx_protocol_ctrl_counters(nx_protocol_counters_t *out)
{
    GlobalState *gs = s_global_state;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (gs == NULL) {
        return;
    }
    out->work_received   = gs->SYSTEM_MODULE.work_received;
    out->shares_accepted = gs->SYSTEM_MODULE.shares_accepted;
    out->shares_rejected = gs->SYSTEM_MODULE.shares_rejected;
    out->queue_depth     = (uint32_t)gs->stratum_queue.count;
}

bool nx_protocol_ctrl_handoff_to_coordinator(void)
{
    GlobalState *gs = s_global_state;

    if (gs == NULL || s_coordinator_task_started || s_ctrl_running) {
        return false;
    }
    if (xTaskCreate(protocol_coordinator_task, "protocol coord", 8192, (void *)gs,
                    5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create protocol coordinator task (handoff)");
        return false;
    }
    ESP_LOGI(TAG, "Protocol handed off to the production coordinator");
    return true;
}
#endif // CONFIG_NX_TIMED_SESSIONS_EXECUTION
