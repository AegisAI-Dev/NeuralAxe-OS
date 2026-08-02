/*
 * NeuralAxe timed pool sessions — one-shot read-only store preflight boot
 * integration (Phase 2M.1B, Gate B10.2).
 *
 * THE feature gate lives here. Under CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT
 * this file performs EXACTLY ONE read-only classification of the "nx_tps"
 * namespace and emits bounded machine tokens. Without the flag there is no
 * state, no read, no token and no symbol: the shipped default firmware is
 * unchanged.
 *
 * SCOPE OF THE READ-ONLY CLAIM: this inspector never writes the nx_tps
 * namespace. It does NOT make the image write-free — the pre-existing
 * nvs_config_init() runs first and can write the "main" namespace, and in
 * its recovery branch erases the whole nvs partition. That case is passed in
 * through nx_tps_preflight_set_partition_erased() and always blocks.
 *
 * It starts no task, registers no route, opens no socket, creates no
 * namespace, writes no nx_tps key, acquires no lease, evaluates no recovery
 * plan, touches no pool/protocol/ASIC/tuning setting and never restarts. It
 * does not require CONFIG_NX_TIMED_SESSIONS: the Gate B3 store and record
 * components compile unconditionally, so the preflight reads the committed
 * schema without bringing the timed-session runtime into existence at all.
 */

#include <string.h>

#include "sdkconfig.h"
#include "pool_session_preflight.h"

/*
 * Mutual exclusion, enforced at the C level as well as in Kconfig so a
 * hand-written sdkconfig cannot produce a posture that inspects the store
 * while something else is allowed to act on it.
 */
#ifdef CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT
/*
 * CONFIG_NX_TIMED_SESSIONS itself must be in this list, not just its children.
 * The four sub-flags all `depends on NX_TIMED_SESSIONS`, so negating them does
 * NOT negate the parent: STORE_PREFLIGHT=y + NX_TIMED_SESSIONS=y would compile,
 * and nx_timed_sessions_boot_init() would then run on the same boot and open
 * "nx_tps" with NVS_READWRITE — creating the very namespace this component
 * exists to inspect without creating.
 */
#if defined(CONFIG_NX_TIMED_SESSIONS) || \
    defined(CONFIG_NX_TIMED_SESSIONS_EXECUTION) || \
    defined(CONFIG_NX_TIMED_SESSIONS_API) || \
    defined(CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE) || \
    defined(CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS)
#error "the read-only store preflight is mutually exclusive with the timed-session \
runtime, controlled execution, the control API, trusted-time observation and pilot \
diagnostics — any of them would open nx_tps NVS_READWRITE on the same boot"
#endif
#endif

#ifdef CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "pool_session.h"
#include "pool_session_record.h"

static const char *TAG = "nx_tps_pre";

/*
 * RAM-only, and file-static rather than task locals: PoolSessionStore carries
 * two 1 KiB codec buffers and PoolSessionRecord is ~1 KiB more, which is far
 * too much for the app_main stack this runs on. Nothing here is persisted.
 */
static PoolSessionStore         s_store;
static PoolSessionRecord        s_record;
static NxTpsPreflightNvsBackend s_backend;
static NxTpsPreflightResult     s_result;
static char                     s_line[NX_TPS_PREFLIGHT_LINE_MAX];
static bool                     s_ran;
static uint32_t                 s_run_count;

bool nx_tps_preflight_enabled(void) { return true; }

uint32_t nx_tps_preflight_run_count(void) { return s_run_count; }

/* Set when nvs_flash_init() failed and we refused to erase to recover. */
static bool s_nvs_init_failed;

bool nx_tps_preflight_boot_gate(void)
{
    esp_err_t err;

    if (s_ran) {
        return !s_nvs_init_failed; /* one-shot; never re-initializes NVS */
    }

    /*
     * NON-DESTRUCTIVE NVS INITIALIZATION.
     *
     * The default firmware answers ESP_ERR_NVS_NO_FREE_PAGES /
     * ESP_ERR_NVS_NEW_VERSION_FOUND by calling nvs_flash_erase(), wiping the
     * WHOLE partition. An image whose only job is to INSPECT the timed-session
     * store must never do that: it would destroy the evidence it came to read,
     * plus the Wi-Fi credentials and the pool configuration.
     *
     * So there is exactly one nvs_flash_init() call and NO recovery path. Every
     * failure — out of pages, newer version, or anything else — fails closed.
     * nvs_flash_erase() is not called here and its call site in nvs_config_init()
     * is compiled out in this posture, so it cannot run at all.
     */
    err = nvs_flash_init();
    if (err != ESP_OK) {
        s_ran             = true;
        s_nvs_init_failed = true;
        if (s_run_count < UINT32_MAX) {
            s_run_count++;
        }
        nx_tps_preflight_result_init(&s_result);

        /* Sanitized: the token carries no raw ESP-IDF error text or number. */
        ESP_LOGI(TAG, "TPS_PREFLIGHT_BOOT");
        ESP_LOGE(TAG, "TPS_PREFLIGHT_BLOCKED_NVS_INIT");

        {
            NxTpsPreflightInput in;
            memset(&in, 0, sizeof(in));
            in.model_version   = NX_TPS_PREFLIGHT_MODEL_VERSION;
            in.nvs_init_failed = true;
            nx_tps_preflight_classify(&in, &s_result);
        }
        if (nx_tps_preflight_format(&s_result, 0u, s_line, sizeof(s_line)) > 0u) {
            ESP_LOGI(TAG, "%s", s_line);
        }
        ESP_LOGE(TAG, "TPS_PREFLIGHT_BLOCKED");
        return false; /* the caller MUST stop: no config init, no Wi-Fi, no mining */
    }

    nx_tps_preflight_run_once(NULL);
    return true;
}

void nx_tps_preflight_run_once(NxTpsPreflightResult *out)
{
    NxTpsPreflightInput in;
    PoolStoreResult     sr;
    PoolStoreResult     init_res;
    uint32_t            uptime_s;

    if (s_ran) {
        /* Idempotent: no re-read, no second token, no polling loop. */
        if (out != NULL) {
            *out = s_result;
        }
        return;
    }
    s_ran = true;
    if (s_run_count < UINT32_MAX) {
        s_run_count++;
    }

    nx_tps_preflight_result_init(&s_result);
    ESP_LOGI(TAG, "TPS_PREFLIGHT_BOOT");

    memset(&in, 0, sizeof(in));
    in.model_version = NX_TPS_PREFLIGHT_MODEL_VERSION;

    nx_tps_preflight_nvs_init(&s_backend);
    s_backend.namespace_present = true; /* until the read-only open says otherwise */

    /*
     * Bind the READ-ONLY ops to the committed Gate B3 loader. Reusing the
     * committed decoder is deliberate: duplicating the schema would create a
     * second implementation that could drift from the one that actually
     * writes records. The loader itself performs only read_blob calls — the
     * mutation ops below it are refusing stubs.
     */
    init_res = pool_session_store_init(&s_store, nx_tps_preflight_nvs_ops(), &s_backend);
    in.namespace_present     = s_backend.namespace_present;
    in.namespace_open_failed = s_backend.open_failed;

    if (s_backend.open_failed) {
        /*
         * The namespace could not be READ. Checked FIRST: an open that failed
         * for any reason other than "does not exist" leaves namespace_present
         * false too, and treating that as absence would classify EMPTY and
         * permit a pilot on a store nobody actually looked at.
         */
        in.store_loaded = true;
        in.store_result = (uint8_t)STORE_IO_ERROR;
    } else if (!s_backend.namespace_present) {
        /* Namespace genuinely absent: the loader is never entered at all. */
        in.store_loaded = false;
    } else if (init_res != STORE_OK) {
        in.store_loaded  = true;
        in.store_result  = (uint8_t)init_res;
    } else {
        memset(&s_record, 0, sizeof(s_record));
        sr = pool_session_store_load(&s_store, &s_record, NULL);
        in.store_loaded = true;
        in.store_result = (uint8_t)sr;
        if (sr == STORE_OK) {
            /* Extract ONLY the scalars the pure classifier needs, through the
             * committed predicates, then destroy the copy. No identity, no
             * generation, no epoch and no payload byte outlives this block. */
            in.record_kind      = s_record.kind;
            in.session_state    = (uint8_t)s_record.state;
            in.restore_required = s_record.restore_required;
            in.record_valid     = (pool_session_record_validate(&s_record) == RECORD_OK);
        }
        memset(&s_record, 0, sizeof(s_record));
    }

    (void)pool_session_store_deinit(&s_store); /* closes the handle on every path */
    memset(&s_record, 0, sizeof(s_record));

    in.write_attempts  = s_backend.write_attempts;
    in.erase_attempts  = s_backend.erase_attempts;
    in.commit_attempts = s_backend.commit_attempts;

    nx_tps_preflight_classify(&in, &s_result);
    s_result.read_attempts = s_backend.read_attempts;

    uptime_s = (uint32_t)((uint64_t)esp_timer_get_time() / 1000000ull);

    /* One outcome token, then one bounded summary. Nothing repeats. */
    ESP_LOGI(TAG, "%s", nx_tps_preflight_outcome_token(s_result.outcome));
    if (nx_tps_preflight_format(&s_result, uptime_s, s_line, sizeof(s_line)) > 0u) {
        ESP_LOGI(TAG, "%s", s_line);
    }
    if (!s_result.permits_pilot) {
        ESP_LOGW(TAG, "TPS_PREFLIGHT_BLOCKED");
    }

    if (out != NULL) {
        *out = s_result;
    }
}

#else /* !CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT — the shipped default */

bool nx_tps_preflight_enabled(void) { return false; }

uint32_t nx_tps_preflight_run_count(void) { return 0u; }

bool nx_tps_preflight_boot_gate(void) { return true; } /* normal boot, unchanged */

void nx_tps_preflight_run_once(NxTpsPreflightResult *out)
{
    /* No NVS access, no namespace open, no token, no state. The fail-closed
     * result is all a caller can ever observe in a default build. */
    nx_tps_preflight_result_init(out);
}

#endif /* CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT */
