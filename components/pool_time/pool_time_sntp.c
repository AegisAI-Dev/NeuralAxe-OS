/*
 * NeuralAxe trusted-time SNTP provider — ESP-IDF adapter (Gate B2).
 *
 * Owns the in-boot SNTP-to-monotonic anchor. All platform access flows
 * through the injectable ops table; the REAL ops at the bottom of this file
 * bind to esp_netif_sntp / esp_sntp / esp_timer (ESP-IDF v5.5.3) and are
 * compiled but NEVER invoked by any production runtime path in Gate B2.
 *
 * Concurrency model:
 *  - The published anchor and the real-stack binding slot are only touched
 *    inside the bounded critical section `s_pool_time_lock` (a spinlock,
 *    safe from the lwIP tcpip thread where the sync callback runs).
 *  - Readers always receive a consistent copy of the anchor — never torn.
 *  - No lock is ever held while calling a platform op or any external code.
 *  - Candidate validation is pure bounded arithmetic and runs inside the
 *    critical section so accept/publish is atomic with respect to readers.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "pool_time_sntp.h"

/* Module lock: guards the published anchors and the real-stack binding. */
static portMUX_TYPE s_pool_time_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * The single real-stack binding slot. The ESP-IDF sync callback carries no
 * context pointer, so at most one provider may own the real SNTP stack.
 * Fake-ops providers never bind here.
 */
static PoolTimeSntpProvider *s_bound_provider = NULL;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void pool_time_sntp_config_defaults(PoolTimeSntpConfig *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_count    = 0u; /* caller MUST set servers; empty is invalid */
    cfg->accept_dhcp_ntp = false;
    cfg->smooth_sync     = false; /* immediate synchronization */
    cfg->sync_wait_s     = POOL_TIME_SYNC_WAIT_DEFAULT_S;
}

PoolTimeError pool_time_sntp_validate_config(const PoolTimeSntpConfig *cfg)
{
    uint32_t i;

    if (cfg == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (cfg->server_count == 0u || cfg->server_count > POOL_TIME_SNTP_MAX_SERVERS) {
        return TIME_ERR_INVALID_SERVER_CONFIG;
    }
    for (i = 0; i < cfg->server_count; i++) {
        const char *s = cfg->servers[i];
        int len = -1;
        int j;
        for (j = 0; j < POOL_TIME_SNTP_SERVER_HOST_MAX; j++) {
            if (s[j] == '\0') {
                len = j;
                break;
            }
        }
        if (len <= 0) { /* unterminated within bound, or empty */
            return TIME_ERR_INVALID_SERVER_CONFIG;
        }
        for (j = 0; j < len; j++) {
            unsigned char c = (unsigned char)s[j];
            /* printable ASCII only, no whitespace or control characters */
            if (c <= 0x20u || c >= 0x7Fu) {
                return TIME_ERR_INVALID_SERVER_CONFIG;
            }
        }
    }
    if (cfg->sync_wait_s == 0u || cfg->sync_wait_s > POOL_TIME_SYNC_WAIT_MAX_S) {
        return TIME_ERR_INVALID_WAIT_WINDOW;
    }
    return TIME_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static bool ops_complete(const PoolTimeSntpPlatformOps *ops)
{
    return ops != NULL && ops->monotonic_us != NULL && ops->sntp_init != NULL &&
           ops->sntp_start != NULL && ops->sntp_stop != NULL && ops->sntp_deinit != NULL;
}

PoolTimeError pool_time_sntp_init(PoolTimeSntpProvider *p,
                                  const PoolTimeSntpPlatformOps *ops,
                                  const PoolTimeSntpConfig *cfg,
                                  const PoolTimeTrustPolicy *policy)
{
    PoolTimeError cfg_err;
    bool wants_real_stack;

    if (p == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    /* Re-initializing an in-use provider is a deterministic error; the
     * caller must deinit first (a fresh zeroed instance models a reboot). */
    if (p->lifecycle != POOL_TIME_SNTP_UNINITIALIZED) {
        return TIME_ERR_SNTP_INIT;
    }
    if (!ops_complete(ops) || !pool_time_trust_policy_valid(policy)) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    cfg_err = pool_time_sntp_validate_config(cfg);
    if (cfg_err != TIME_OK) {
        return cfg_err;
    }

    memset(p, 0, sizeof(*p));
    p->ops    = ops;
    p->config = *cfg;
    p->policy = *policy;
    p->anchor.sync_status = POOL_TIME_SYNC_STATUS_NONE;

    wants_real_stack = (ops == pool_time_sntp_real_ops());
    if (wants_real_stack) {
        bool claimed = false;
        portENTER_CRITICAL(&s_pool_time_lock);
        if (s_bound_provider == NULL) {
            s_bound_provider = p;
            claimed = true;
        }
        portEXIT_CRITICAL(&s_pool_time_lock);
        if (!claimed) {
            memset(p, 0, sizeof(*p));
            return TIME_ERR_SNTP_INIT; /* single real-stack owner */
        }
        p->bound_to_real_stack = true;
    }

    /* Configure only — the platform op must not start any networking. */
    if (p->ops->sntp_init(&p->config) != 0) {
        if (p->bound_to_real_stack) {
            portENTER_CRITICAL(&s_pool_time_lock);
            if (s_bound_provider == p) {
                s_bound_provider = NULL;
            }
            portEXIT_CRITICAL(&s_pool_time_lock);
            p->bound_to_real_stack = false;
        }
        p->lifecycle  = POOL_TIME_SNTP_ERROR;
        p->last_error = TIME_ERR_SNTP_INIT;
        return TIME_ERR_SNTP_INIT;
    }

    p->lifecycle  = POOL_TIME_SNTP_INITIALIZED;
    p->last_error = TIME_OK;
    return TIME_OK;
}

PoolTimeError pool_time_sntp_start(PoolTimeSntpProvider *p)
{
    if (p == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (p->lifecycle == POOL_TIME_SNTP_UNINITIALIZED) {
        return TIME_ERR_NOT_INITIALIZED;
    }
    if (p->lifecycle == POOL_TIME_SNTP_ERROR) {
        return TIME_ERR_SNTP_START; /* only deinit recovers from ERROR */
    }
    if (p->ops->sntp_start() != 0) {
        p->lifecycle  = POOL_TIME_SNTP_ERROR;
        p->last_error = TIME_ERR_SNTP_START;
        return TIME_ERR_SNTP_START;
    }
    portENTER_CRITICAL(&s_pool_time_lock);
    if (p->anchor.valid) {
        /* An accepted anchor survives a service restart; trust holds. */
        p->lifecycle = POOL_TIME_SNTP_TRUSTED;
    } else {
        p->anchor.sync_status = POOL_TIME_SYNC_STATUS_PENDING;
        p->lifecycle = POOL_TIME_SNTP_SYNC_PENDING;
    }
    portEXIT_CRITICAL(&s_pool_time_lock);
    p->last_error = TIME_OK;
    return TIME_OK;
}

PoolTimeError pool_time_sntp_stop(PoolTimeSntpProvider *p)
{
    if (p == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (p->lifecycle == POOL_TIME_SNTP_UNINITIALIZED) {
        return TIME_ERR_NOT_INITIALIZED;
    }
    if (p->lifecycle == POOL_TIME_SNTP_ERROR) {
        return TIME_ERR_SNTP_STOP; /* only deinit recovers from ERROR */
    }
    if (p->lifecycle == POOL_TIME_SNTP_STOPPED) {
        return TIME_OK; /* idempotent no-op */
    }
    if (p->ops->sntp_stop() != 0) {
        p->lifecycle  = POOL_TIME_SNTP_ERROR;
        p->last_error = TIME_ERR_SNTP_STOP;
        return TIME_ERR_SNTP_STOP;
    }
    portENTER_CRITICAL(&s_pool_time_lock);
    if (!p->anchor.valid) {
        p->anchor.sync_status = POOL_TIME_SYNC_STATUS_NONE;
    }
    portEXIT_CRITICAL(&s_pool_time_lock);
    /* The anchor — and therefore in-boot trust — persists until deinit. */
    p->lifecycle  = POOL_TIME_SNTP_STOPPED;
    p->last_error = TIME_OK;
    return TIME_OK;
}

PoolTimeError pool_time_sntp_deinit(PoolTimeSntpProvider *p)
{
    if (p == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (p->lifecycle == POOL_TIME_SNTP_UNINITIALIZED) {
        return TIME_OK; /* idempotent */
    }
    if (ops_complete(p->ops)) {
        (void)p->ops->sntp_deinit();
    }
    portENTER_CRITICAL(&s_pool_time_lock);
    if (s_bound_provider == p) {
        s_bound_provider = NULL;
    }
    memset(&p->anchor, 0, sizeof(p->anchor)); /* trust never survives deinit */
    portEXIT_CRITICAL(&s_pool_time_lock);
    memset(p, 0, sizeof(*p)); /* back to UNINITIALIZED */
    return TIME_OK;
}

/* ------------------------------------------------------------------ */
/* Synchronization ingestion (the single anchor-capture path)          */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_sntp_handle_sync(PoolTimeSntpProvider *p,
                                         uint64_t epoch_s,
                                         uint32_t epoch_us_frac)
{
    PoolTimeAnchor candidate;
    PoolTimeError verdict;
    uint64_t mono;

    if (p == NULL) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (p->lifecycle == POOL_TIME_SNTP_UNINITIALIZED) {
        /* Callback after deinit: ignored safely, no mutation. */
        return TIME_ERR_NOT_INITIALIZED;
    }
    if (p->lifecycle != POOL_TIME_SNTP_SYNC_PENDING &&
        p->lifecycle != POOL_TIME_SNTP_TRUSTED) {
        /* Service not running: ignored deterministically, no mutation. */
        return TIME_ERR_NOT_SYNCED;
    }
    if (!ops_complete(p->ops)) {
        return TIME_ERR_NOT_INITIALIZED;
    }
    if (epoch_us_frac >= (uint32_t)POOL_TIME_US_PER_S) {
        p->last_error = TIME_ERR_INVALID_ARGUMENT;
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (epoch_s > (UINT64_MAX - (uint64_t)epoch_us_frac) / POOL_TIME_US_PER_S) {
        p->last_error = TIME_ERR_OVERFLOW;
        return TIME_ERR_OVERFLOW;
    }

    /* Capture monotonic as close as practical to the sync instant. */
    mono = p->ops->monotonic_us();

    memset(&candidate, 0, sizeof(candidate));
    candidate.valid                    = true;
    candidate.sync_completed_this_boot = true;
    candidate.epoch_us_at_sync         = epoch_s * POOL_TIME_US_PER_S + (uint64_t)epoch_us_frac;
    candidate.monotonic_us_at_sync     = mono;
    candidate.sync_status              = POOL_TIME_SYNC_STATUS_COMPLETED;
    candidate.last_error               = TIME_OK;

    /* Validate against the currently published anchor and publish atomically.
     * The validation is pure bounded arithmetic — safe inside the critical
     * section, and it makes accept/publish indivisible for readers. */
    portENTER_CRITICAL(&s_pool_time_lock);
    verdict = pool_time_validate_reanchor(p->anchor.valid ? &p->anchor : NULL,
                                          &candidate, &p->policy);
    if (verdict == TIME_OK) {
        candidate.generation = (p->anchor.generation == UINT32_MAX)
                                   ? UINT32_MAX /* saturate, never wrap */
                                   : p->anchor.generation + 1u;
        p->anchor    = candidate;
        p->lifecycle = POOL_TIME_SNTP_TRUSTED;
    }
    portEXIT_CRITICAL(&s_pool_time_lock);

    /* A rejected candidate leaves the accepted anchor byte-identical. */
    p->last_error = verdict;
    return verdict;
}

/* ------------------------------------------------------------------ */
/* Abstract clock binding                                              */
/* ------------------------------------------------------------------ */

static uint64_t provider_clock_monotonic_us(void *ctx)
{
    PoolTimeSntpProvider *p = (PoolTimeSntpProvider *)ctx;
    if (p == NULL || p->ops == NULL || p->ops->monotonic_us == NULL) {
        return 0u;
    }
    return p->ops->monotonic_us();
}

static bool provider_clock_read_anchor(void *ctx, PoolTimeAnchor *out)
{
    PoolTimeSntpProvider *p = (PoolTimeSntpProvider *)ctx;
    if (p == NULL || out == NULL) {
        return false;
    }
    if (p->lifecycle == POOL_TIME_SNTP_UNINITIALIZED) {
        return false;
    }
    portENTER_CRITICAL(&s_pool_time_lock);
    *out = p->anchor; /* consistent copy, never torn */
    portEXIT_CRITICAL(&s_pool_time_lock);
    return true;
}

static const PoolTimeClockOps s_provider_clock_ops = {
    .monotonic_us = provider_clock_monotonic_us,
    .read_anchor  = provider_clock_read_anchor,
};

void pool_time_sntp_get_clock(PoolTimeSntpProvider *p, PoolTimeClock *out)
{
    if (out == NULL) {
        return;
    }
    out->ops = &s_provider_clock_ops;
    out->ctx = p;
}

PoolTimeSntpLifecycle pool_time_sntp_lifecycle(const PoolTimeSntpProvider *p)
{
    return (p == NULL) ? POOL_TIME_SNTP_UNINITIALIZED : p->lifecycle;
}

PoolTimeError pool_time_sntp_last_error(const PoolTimeSntpProvider *p)
{
    return (p == NULL) ? TIME_ERR_INVALID_ARGUMENT : p->last_error;
}

const char *pool_time_sntp_lifecycle_str(PoolTimeSntpLifecycle lc)
{
    switch (lc) {
    case POOL_TIME_SNTP_UNINITIALIZED: return "SNTP_UNINITIALIZED";
    case POOL_TIME_SNTP_INITIALIZED:   return "SNTP_INITIALIZED";
    case POOL_TIME_SNTP_SYNC_PENDING:  return "SNTP_SYNC_PENDING";
    case POOL_TIME_SNTP_TRUSTED:       return "SNTP_TRUSTED";
    case POOL_TIME_SNTP_STOPPED:       return "SNTP_STOPPED";
    case POOL_TIME_SNTP_ERROR:         return "SNTP_ERROR";
    default:                           return "SNTP_LIFECYCLE_UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* REAL ESP-IDF platform ops (compiled; never invoked in Gate B2)      */
/* ------------------------------------------------------------------ */

/*
 * Sync notification callback. Runs in the lwIP tcpip thread AFTER the SNTP
 * response is processed; `tv` is the server-supplied UTC instant. The raw
 * system clock is never read here — a concurrent Stratum settimeofday()
 * cannot leak into the anchor.
 */
static void pool_time_sntp_real_sync_cb(struct timeval *tv)
{
    PoolTimeSntpProvider *p;
    uint64_t epoch_s;
    uint32_t frac;

    if (tv == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_pool_time_lock);
    p = s_bound_provider;
    portEXIT_CRITICAL(&s_pool_time_lock);
    if (p == NULL) {
        return; /* callback after deinit: ignored safely */
    }
    epoch_s = (tv->tv_sec > 0) ? (uint64_t)tv->tv_sec : 0u;
    frac    = (tv->tv_usec >= 0 && tv->tv_usec < (suseconds_t)POOL_TIME_US_PER_S)
                  ? (uint32_t)tv->tv_usec
                  : 0u;
    (void)pool_time_sntp_handle_sync(p, epoch_s, frac);
}

static uint64_t pool_time_sntp_real_monotonic_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static int pool_time_sntp_real_init_op(const PoolTimeSntpConfig *cfg)
{
    esp_sntp_config_t c;
    uint32_t i;

    if (cfg == NULL || cfg->server_count == 0u) {
        return -1;
    }
    /* The compiled lwIP limit is authoritative; reject rather than truncate. */
    if (cfg->server_count > (uint32_t)CONFIG_LWIP_SNTP_MAX_SERVERS) {
        return -1;
    }
#ifndef CONFIG_LWIP_DHCP_GET_NTP_SRV
    if (cfg->accept_dhcp_ntp) {
        return -1; /* DHCP-provided NTP is not compiled in */
    }
#endif

    memset(&c, 0, sizeof(c));
    c.smooth_sync               = cfg->smooth_sync;
    c.server_from_dhcp          = cfg->accept_dhcp_ntp;
    c.wait_for_sync             = false; /* trust comes from the anchor, not a semaphore */
    c.start                     = false; /* NO networking at init */
    c.sync_cb                   = pool_time_sntp_real_sync_cb;
    c.renew_servers_after_new_IP = false;
    c.ip_event_to_renew         = IP_EVENT_STA_GOT_IP;
    c.index_of_first_server     = 0;
    c.num_of_servers            = cfg->server_count;
    for (i = 0; i < cfg->server_count && i < (uint32_t)CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        /* lwIP stores the pointer; cfg is the provider's stable storage. */
        c.servers[i] = cfg->servers[i];
    }
    return (esp_netif_sntp_init(&c) == ESP_OK) ? 0 : -1;
}

static int pool_time_sntp_real_start_op(void)
{
    return (esp_netif_sntp_start() == ESP_OK) ? 0 : -1;
}

static int pool_time_sntp_real_stop_op(void)
{
    esp_sntp_stop();
    return 0;
}

static int pool_time_sntp_real_deinit_op(void)
{
    esp_netif_sntp_deinit();
    return 0;
}

static const PoolTimeSntpPlatformOps s_real_ops = {
    .monotonic_us = pool_time_sntp_real_monotonic_us,
    .sntp_init    = pool_time_sntp_real_init_op,
    .sntp_start   = pool_time_sntp_real_start_op,
    .sntp_stop    = pool_time_sntp_real_stop_op,
    .sntp_deinit  = pool_time_sntp_real_deinit_op,
};

const PoolTimeSntpPlatformOps *pool_time_sntp_real_ops(void)
{
    return &s_real_ops;
}
