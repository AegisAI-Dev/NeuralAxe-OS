/*
 * NeuralAxe timed pool sessions — production singleton and boot integration
 * (Phase 2M.1B, Gate B6).
 *
 * THE feature gate lives here. Under CONFIG_NX_TIMED_SESSIONS this file
 * owns the single production runtime instance and drives the synchronous
 * boot order; without it there is no instance storage at all, so nothing
 * can be bootstrapped, no task can be created, "nx_tps" is never opened
 * and SNTP is never initialized.
 *
 * The pure rule that both branches obey is
 * pool_runtime_boot_action_for_feature(), so the disabled contract is a
 * deterministically TESTED property, not just a preprocessor claim.
 */

#include "sdkconfig.h"
#include "pool_session_runtime_boot.h"
#include "pool_session_runtime.h"

#ifdef CONFIG_NX_TIMED_SESSIONS
#include "esp_log.h"

static const char *TAG = "nx_pool_boot";

/* The ONE production instance and its NVS backend context. */
static PoolSessionRuntime  s_runtime;
static PoolStoreNvsBackend s_nvs_backend;
static bool                s_boot_attempted;
static bool                s_boot_ok;

bool nx_timed_sessions_enabled(void) { return true; }

PoolSessionRuntime *pool_session_runtime_default_instance(void) { return &s_runtime; }

bool nx_timed_sessions_boot_init(void)
{
    PoolSessionRuntimeDeps deps;
    PoolRuntimeStatus      st;

    if (s_boot_attempted) {
        return s_boot_ok; /* exactly one bootstrap per boot */
    }
    s_boot_attempted = true;

    pool_session_runtime_production_deps(&deps, &s_nvs_backend);

    st = pool_session_runtime_init(&s_runtime, &deps);
    if (st != RUNTIME_OK) {
        ESP_LOGE(TAG, "runtime init failed (%s) — protocol start HELD",
                 pool_runtime_status_str(st));
        return false; /* no proven safe posture: the barrier holds */
    }

    (void)pool_session_runtime_boot(&s_runtime);

    /* Step 14: the owner task is created only once coherent ownership
     * exists. A task failure never releases the hold. */
    st = pool_session_runtime_start_task(&s_runtime);
    if (st != RUNTIME_OK) {
        ESP_LOGE(TAG, "runtime task not started (%s)", pool_runtime_status_str(st));
    }

    s_boot_ok = true;
    return true;
}

bool nx_timed_sessions_protocol_start_allowed(void)
{
    PoolRuntimeProtocolPermission p;

    if (!s_boot_attempted || !s_boot_ok) {
        return false; /* fail closed: no proven safe posture */
    }
    p = pool_session_runtime_protocol_permission(&s_runtime);
    return p == POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;
}

void nx_timed_sessions_notify_network_ready(void)
{
    if (!s_boot_ok) {
        return;
    }
    (void)pool_session_runtime_notify(&s_runtime, RUNTIME_EVENT_NETWORK_READY);
}

#else /* !CONFIG_NX_TIMED_SESSIONS — the default build */

bool nx_timed_sessions_enabled(void) { return false; }

/* No instance storage exists when the feature is disabled. */
PoolSessionRuntime *pool_session_runtime_default_instance(void) { return NULL; }

bool nx_timed_sessions_boot_init(void)
{
    /* No bootstrap, no store open, no SNTP, no task, no NVS access. */
    return pool_runtime_boot_action_for_feature(false).bootstrap_required;
}

bool nx_timed_sessions_protocol_start_allowed(void)
{
    /* Protocol startup is unchanged when the feature is off. */
    return pool_runtime_boot_action_for_feature(false).protocol_allowed;
}

void nx_timed_sessions_notify_network_ready(void)
{
    /* Nothing exists to notify. */
}

#endif /* CONFIG_NX_TIMED_SESSIONS */

bool pool_session_runtime_feature_enabled(void)
{
    return nx_timed_sessions_enabled();
}
