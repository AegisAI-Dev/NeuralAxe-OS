/*
 * NeuralAxe timed pool sessions — Gate B7 production glue
 * (Phase 2M.1B). Board 601 / BM1370 only.
 *
 * This file owns the SINGLE production executor instance and the two
 * production adapters:
 *
 *  - the CONFIGURATION adapter stages identity fields through the existing
 *    audited nvs_config writer (single writer, cache-coherent) and verifies
 *    them through an INDEPENDENT read-only NVS handle on the "main"
 *    namespace — flash truth, never the writer's RAM cache. It writes
 *    exactly the eleven identity keys (primary/fallback host, port, user,
 *    protocol, TLS + the fallback-role flag) and NEVER references either
 *    password key. TLS is compared and written at boolean granularity (the
 *    committed B1 identity carries a bool; an unchanged boolean leaves the
 *    stored tls_mode untouched).
 *  - the PROTOCOL adapter wraps the Gate B7 controlled hooks of
 *    protocol_coordinator.c (single protocol engine, drained events,
 *    fresh-generation counters, bounded stop handshake, one-shot handoff).
 *
 * Everything is compiled ONLY under CONFIG_NX_TIMED_SESSIONS_EXECUTION:
 * with the flag off no executor storage, adapter, hook or gate consultation
 * exists anywhere in the binary.
 */

#include "sdkconfig.h"
#include "nx_execution_glue.h"

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "global_state.h"
#include "nvs_config.h"
#include "system.h"
#include "protocol_coordinator.h"
#include "pool_session_execution.h"
#include "pool_session_runtime.h"
#include "pool_session_runtime_boot.h"

static const char *TAG = "nx_pool_glue";

static PoolSessionExecutor s_executor;
static GlobalState        *s_gs;
static bool                s_bound;

/* ------------------------------------------------------------------ */
/* Configuration adapter                                               */
/* ------------------------------------------------------------------ */

static bool glue_device_identity(void *ctx, char *board, size_t board_cap,
                                 char *asic, size_t asic_cap)
{
    (void)ctx;
    if (s_gs == NULL || board == NULL || asic == NULL ||
        s_gs->DEVICE_CONFIG.board_version == NULL ||
        s_gs->DEVICE_CONFIG.family.asic.name == NULL) {
        return false;
    }
    strncpy(board, s_gs->DEVICE_CONFIG.board_version, board_cap - 1u);
    board[board_cap - 1u] = '\0';
    strncpy(asic, s_gs->DEVICE_CONFIG.family.asic.name, asic_cap - 1u);
    asic[asic_cap - 1u] = '\0';
    return true;
}

static const char *proto_to_str(PoolSessionProtocol p)
{
    return (p == POOL_PROTO_STRATUM_V2) ? STRATUM_V2 : STRATUM_V1;
}

/*
 * Stage the canonical desired identity into the audited writer, in a
 * documented deterministic order: role first, then the fallback endpoint,
 * then the primary endpoint (the mining-critical fields last). The setters
 * dedupe against their cache, so unchanged fields enqueue nothing. TLS
 * keys are written only on a BOOLEAN change (see the file contract).
 * NO password key is referenced anywhere in this function.
 */
static bool glue_stage_apply(void *ctx, const PoolConfigIdentity *identity)
{
    PoolExecEffectiveConfig desired;

    (void)ctx;
    if (identity == NULL) {
        return false;
    }
    pool_exec_desired_from_identity(identity, &desired);

    nvs_config_set_bool(NVS_CONFIG_USE_FALLBACK_STRATUM, false);

    nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_URL, desired.fallback.host);
    nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, desired.fallback.port);
    nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, desired.fallback.user);
    nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL,
                          proto_to_str(desired.fallback.protocol));
    if ((nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS) != 0u) !=
        desired.fallback.tls) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS,
                           desired.fallback.tls ? 1u : 0u);
    }

    nvs_config_set_string(NVS_CONFIG_STRATUM_URL, desired.primary.host);
    nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, desired.primary.port);
    nvs_config_set_string(NVS_CONFIG_STRATUM_USER, desired.primary.user);
    nvs_config_set_string(NVS_CONFIG_STRATUM_PROTOCOL,
                          proto_to_str(desired.primary.protocol));
    if ((nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS) != 0u) != desired.primary.tls) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_TLS, desired.primary.tls ? 1u : 0u);
    }
    return true;
}

/*
 * Independent flash readback of one string key. A stored value longer than
 * the bounded field is replaced by a one-byte sentinel that can never equal
 * a valid identity value (truncation could falsely match). A missing key
 * reads as the audited default — exactly what a fresh boot would load.
 */
static void read_str_key(nvs_handle_t h, NvsConfigKey key, char *out, size_t cap)
{
    const Settings *setting = nvs_config_get_settings(key);
    size_t len = 0;
    esp_err_t err;

    out[0] = '\0';
    err = nvs_get_str(h, setting->nvs_key_name, NULL, &len);
    if (err == ESP_OK) {
        if (len > cap) {
            out[0] = '\x01'; /* overflow sentinel: never a valid identity */
            out[1] = '\0';
            return;
        }
        if (nvs_get_str(h, setting->nvs_key_name, out, &len) == ESP_OK) {
            return;
        }
        out[0] = '\x01';
        out[1] = '\0';
        return;
    }
    if (setting->default_value.str != NULL) {
        strncpy(out, setting->default_value.str, cap - 1u);
        out[cap - 1u] = '\0';
    }
}

static uint16_t read_u16_key(nvs_handle_t h, NvsConfigKey key)
{
    const Settings *setting = nvs_config_get_settings(key);
    uint16_t v = 0;

    if (nvs_get_u16(h, setting->nvs_key_name, &v) != ESP_OK) {
        v = setting->default_value.u16;
    }
    return v;
}

static PoolSessionProtocol proto_from_str(const char *s)
{
    return (s != NULL && strcmp(s, STRATUM_V2) == 0) ? POOL_PROTO_STRATUM_V2
                                                     : POOL_PROTO_STRATUM_V1;
}

static void glue_read_effective(void *ctx, PoolExecEffectiveConfig *out)
{
    nvs_handle_t h;
    char proto_buf[8];

    (void)ctx;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (nvs_open("main", NVS_READONLY, &h) != ESP_OK) {
        return; /* valid stays false: the readback itself failed */
    }

    read_str_key(h, NVS_CONFIG_STRATUM_URL, out->primary.host,
                 sizeof(out->primary.host));
    out->primary.port = read_u16_key(h, NVS_CONFIG_STRATUM_PORT);
    read_str_key(h, NVS_CONFIG_STRATUM_USER, out->primary.user,
                 sizeof(out->primary.user));
    read_str_key(h, NVS_CONFIG_STRATUM_PROTOCOL, proto_buf, sizeof(proto_buf));
    out->primary.protocol = proto_from_str(proto_buf);
    /* Carry the RAW stored mode, not just the lossy B1 boolean, so the
     * representability gate can reject a custom-certificate configuration
     * instead of silently downgrading it on restore. */
    {
        uint16_t raw = read_u16_key(h, NVS_CONFIG_STRATUM_TLS);
        out->primary_tls_mode = (raw > UINT8_MAX) ? UINT8_MAX : (uint8_t) raw;
        out->primary.tls = raw != 0u;
    }

    read_str_key(h, NVS_CONFIG_FALLBACK_STRATUM_URL, out->fallback.host,
                 sizeof(out->fallback.host));
    out->fallback.port = read_u16_key(h, NVS_CONFIG_FALLBACK_STRATUM_PORT);
    read_str_key(h, NVS_CONFIG_FALLBACK_STRATUM_USER, out->fallback.user,
                 sizeof(out->fallback.user));
    read_str_key(h, NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL, proto_buf,
                 sizeof(proto_buf));
    out->fallback.protocol = proto_from_str(proto_buf);
    {
        uint16_t raw = read_u16_key(h, NVS_CONFIG_FALLBACK_STRATUM_TLS);
        out->fallback_tls_mode = (raw > UINT8_MAX) ? UINT8_MAX : (uint8_t) raw;
        out->fallback.tls = raw != 0u;
    }

    /* Booleans are stored as u16 by the audited writer. */
    out->use_fallback = read_u16_key(h, NVS_CONFIG_USE_FALLBACK_STRATUM) != 0u;

    nvs_close(h);
    out->valid = true;
}

/*
 * EXECUTOR-OWNED live configuration storage.
 *
 * The protocol-facing SYSTEM_MODULE identity pointers must be re-pointed
 * whenever a controlled transaction changes the effective configuration.
 * Allocating fresh strings per refresh and retaining the old ones would
 * grow without bound; freeing them immediately would be a use-after-free
 * for readers that hold the raw pointer (HTTP/BAP status surfaces, the
 * share-submit path) and follow no lifetime protocol; and two rotating
 * buffers would still let a long-lived reader be overwritten in place.
 *
 * The storage therefore comes from the execution component's identity slot
 * pool, whose ownership model is proven rather than assumed (see
 * pool_session_execution.h): static and never freed, each slot written
 * exactly once BEFORE publication and immutable afterwards, never reused
 * within an execution epoch, reclaimed only at a quiescent point, bounded
 * by the B1 retry budgets, refcounted for controlled readers, and
 * FAIL-CLOSED on exhaustion.
 *
 * Passwords and certificates are NEVER copied here: the protocol tasks keep
 * reading the untouched boot-time pointers for those.
 */
static uint32_t s_live_refreshes; /* audit counter (never a leak) */

/* Bounded copy; false when the value cannot be represented exactly. */
static bool slot_copy(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (src == NULL) {
        return false;
    }
    n = strlen(src);
    if (n >= cap) {
        return false; /* never publish a truncated identity */
    }
    memcpy(dst, src, n + 1u);
    return true;
}

/*
 * Refresh the live protocol-facing SYSTEM_MODULE copies from the audited
 * cache (which the independent readback already proved equals flash).
 * Called ONLY while no session protocol task exists.
 */
static bool glue_refresh_live(void *ctx, const PoolConfigIdentity *identity)
{
    SystemModule            *m;
    PoolExecIdentityStrings *slot;
    uint32_t                 slot_id = 0u;
    char *url = NULL, *user = NULL, *fb_url = NULL, *fb_user = NULL, *fb_proto = NULL;
    bool  ok;

    (void)ctx;
    (void)identity; /* the audited cache is the verified source of truth */
    if (s_gs == NULL) {
        return false;
    }
    m = &s_gs->SYSTEM_MODULE;

    /* Acquire a slot that has NEVER been used in this epoch. Exhaustion is
     * a hard, honest failure: no publication, no protocol start. */
    slot = pool_session_execution_identity_acquire(&slot_id);
    if (slot == NULL) {
        ESP_LOGE(TAG, "live identity refresh rejected (slot pool exhausted)");
        return false;
    }

    url      = nvs_config_get_string(NVS_CONFIG_STRATUM_URL);
    user     = nvs_config_get_string(NVS_CONFIG_STRATUM_USER);
    fb_url   = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL);
    fb_user  = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER);
    fb_proto = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL);

    /* Write the slot ONCE, before it is published: after publication its
     * bytes are immutable for the rest of the epoch. */
    ok = slot_copy(slot->primary_host, sizeof(slot->primary_host), url) &&
         slot_copy(slot->primary_user, sizeof(slot->primary_user), user) &&
         slot_copy(slot->fallback_host, sizeof(slot->fallback_host), fb_url) &&
         slot_copy(slot->fallback_user, sizeof(slot->fallback_user), fb_user) &&
         fb_proto != NULL;

    /* The temporary reader copies are released immediately — they never
     * become the published pointers. */
    free(url);
    free(user);
    free(fb_url);
    free(fb_user);
    if (!ok) {
        free(fb_proto);
        ESP_LOGE(TAG, "live identity refresh rejected (bounds)");
        return false; /* fail closed: nothing was published */
    }

    if (!pool_session_execution_identity_publish(slot_id)) {
        free(fb_proto);
        ESP_LOGE(TAG, "live identity publication refused");
        return false;
    }

    /* Publish: scalars first, then the identity pointers into the slot. */
    m->pool_port          = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT);
    m->pool_tls           = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS);
    m->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT);
    m->fallback_pool_tls  = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS);
    m->fallback_pool_protocol = stratum_protocol_from_string(fb_proto);
    free(fb_proto);

    m->pool_url           = slot->primary_host;
    m->pool_user          = slot->primary_user;
    m->fallback_pool_url  = slot->fallback_host;
    m->fallback_pool_user = slot->fallback_user;

    m->use_fallback_stratum = nvs_config_get_bool(NVS_CONFIG_USE_FALLBACK_STRATUM);
    m->is_using_fallback    = false; /* sessions pin the primary endpoint */

    s_live_refreshes++;
    return true;
}

static const PoolExecConfigOps s_config_ops = {
    .device_identity = glue_device_identity,
    .stage_apply     = glue_stage_apply,
    .read_effective  = glue_read_effective,
    .refresh_live    = glue_refresh_live,
};

/* ------------------------------------------------------------------ */
/* Protocol adapter                                                    */
/* ------------------------------------------------------------------ */

static bool glue_proto_start(void *ctx, PoolSessionProtocol protocol)
{
    (void)ctx;
    return nx_protocol_ctrl_start((protocol == POOL_PROTO_STRATUM_V2)
                                      ? STRATUM_PROTOCOL_V2
                                      : STRATUM_PROTOCOL_V1);
}

static bool glue_proto_stop(void *ctx)
{
    (void)ctx;
    return nx_protocol_ctrl_stop();
}

static bool glue_proto_running(void *ctx)
{
    (void)ctx;
    return nx_protocol_ctrl_running();
}

static uint32_t glue_proto_poll_events(void *ctx)
{
    uint32_t nx = nx_protocol_ctrl_poll_events();
    uint32_t bits = 0u;

    (void)ctx;
    if (nx & NX_PROTOCOL_EVT_FAILED) {
        bits |= EXEC_PEVT_CONNECTION_FAILED;
    }
    if (nx & NX_PROTOCOL_EVT_SETUP_SUCCESS) {
        bits |= EXEC_PEVT_SETUP_SUCCESS;
    }
    if (nx & NX_PROTOCOL_EVT_TASK_EXITED) {
        bits |= EXEC_PEVT_TASK_EXITED;
    }
    return bits;
}

static void glue_proto_counters(void *ctx, PoolExecProtocolCounters *out)
{
    nx_protocol_counters_t c;

    (void)ctx;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    nx_protocol_ctrl_counters(&c);
    out->work_received   = c.work_received;
    out->shares_accepted = c.shares_accepted;
    out->shares_rejected = c.shares_rejected;
    out->queue_depth     = c.queue_depth;
}

static bool glue_proto_handoff(void *ctx)
{
    (void)ctx;
    return nx_protocol_ctrl_handoff_to_coordinator();
}

static const PoolExecProtocolOps s_proto_ops = {
    .start          = glue_proto_start,
    .stop           = glue_proto_stop,
    .running        = glue_proto_running,
    .poll_events    = glue_proto_poll_events,
    .counters       = glue_proto_counters,
    .handoff_source = glue_proto_handoff,
};

/* ------------------------------------------------------------------ */
/* Runtime hook and boot wiring                                        */
/* ------------------------------------------------------------------ */

/* Driven by the single B6 owner task; steps the executor and reports
 * whether it owns the session flow (the task then defers its own plan
 * persistence/reconcile machinery). */
static bool glue_runtime_hook(void *ctx)
{
    PoolSessionExecutor *ex = (PoolSessionExecutor *)ctx;

    (void)pool_session_executor_step(ex);
    return pool_session_executor_owns_flow(ex);
}

bool nx_pool_execution_boot_init(void *gs)
{
    PoolSessionRuntime *rt = pool_session_runtime_default_instance();
    PoolExecReason      r;

    if (s_bound) {
        return true; /* exactly one bind per boot */
    }
    if (gs == NULL || rt == NULL || !rt->initialized || !rt->booted) {
        ESP_LOGW(TAG, "executor not bound (runtime unavailable)");
        return false;
    }
    s_gs = (GlobalState *)gs;

    /* Boot-time gate initialization: DEFAULT_OPEN, zeroed counters, empty
     * delivered-work registry and a fully reclaimed identity slot pool.
     * This is a quiescent point by construction — no session protocol
     * instance exists before the executor is bound. */
    pool_session_execution_gate_reset();

    pool_session_executor_init(&s_executor);
    r = pool_session_executor_bind(&s_executor, rt, &s_config_ops, NULL,
                                   &s_proto_ops, NULL, NULL);
    if (r != EXEC_REASON_NONE) {
        ESP_LOGE(TAG, "executor bind refused (%s)", pool_exec_reason_str(r));
        return false;
    }
    pool_session_runtime_register_executor(glue_runtime_hook, &s_executor);
    s_bound = true;
    ESP_LOGI(TAG, "Gate B7 executor bound (execution flag enabled)");
    return true;
}

void nx_pool_execution_notify_system_ready(void)
{
    if (s_bound) {
        pool_session_executor_set_system_ready(&s_executor, true);
    }
}

#else /* !CONFIG_NX_TIMED_SESSIONS_EXECUTION — hold-only or fully disabled */

bool nx_pool_execution_boot_init(void *gs)
{
    (void)gs;
    return false; /* no executor storage, no adapters, no hook */
}

void nx_pool_execution_notify_system_ready(void)
{
}

#endif /* CONFIG_NX_TIMED_SESSIONS_EXECUTION */
