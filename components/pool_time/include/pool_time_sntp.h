#ifndef POOL_TIME_SNTP_H_
#define POOL_TIME_SNTP_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_time.h"

/*
 * NeuralAxe trusted-time SNTP provider (Phase 2M.1B, Gate B2).
 *
 * "Trusted" here means: accepted by the NeuralAxe scheduler trust policy
 * after an SNTP callback in the current boot — an OPERATIONAL verdict.
 * Standard SNTP/DNS in this design is NOT cryptographically authenticated;
 * the anchor prevents later Stratum settimeofday() manipulation and the
 * sanity band + anti-regression checks detect classes of error, but they do
 * not authenticate the remote NTP server or network path. See the
 * TRUST-BOUNDARY PRECISION and NETWORK-TRUST RESIDUAL sections in
 * pool_time.h.
 *
 * The ESP-IDF-backed adapter that owns the in-boot SNTP-to-monotonic anchor.
 * This header is deliberately free of ESP-IDF types so tests and the pure
 * layer can consume it without the platform. All platform access flows
 * through an injectable ops table; the REAL ops (pool_time_sntp_real_ops)
 * bind to esp_netif_sntp / esp_sntp / esp_timer inside pool_time_sntp.c.
 *
 * GATE B2 CONTRACT: this provider compiles as a production component but is
 * NEVER instantiated, started or wired by any production runtime path. No
 * networking happens unless a future gate explicitly initializes AND starts
 * a provider with the real ops. B2 tests use fake ops exclusively.
 *
 * ANCHOR DESIGN (validated against ESP-IDF v5.5.3 sources):
 *  - esp_netif_sntp_init() with .start=false configures the service without
 *    any network traffic; esp_netif_sntp_start() begins polling.
 *  - The sync notification callback runs in the lwIP tcpip thread and
 *    receives the SERVER-SUPPLIED `struct timeval` directly. The anchor is
 *    captured from that value plus esp_timer_get_time() — the raw system
 *    clock (settimeofday-writable, Stratum-influenced) is NEVER read.
 *  - sntp_get_sync_status() is a destructive read (COMPLETED resets to
 *    RESET), so SNTP status is never used as a trust signal; only the
 *    callback-captured anchor is.
 *  - lwIP stores the server-name POINTER (no copy): the provider's own
 *    bounded config storage must outlive the running service, which the
 *    provider struct guarantees.
 *  - The ESP-IDF callback carries no context pointer, so at most ONE
 *    provider may be bound to the real SNTP stack at a time (a static
 *    binding slot inside pool_time_sntp.c, set only when the provider is
 *    initialized with the real ops). Fake-ops providers never bind, so
 *    tests can freely create fresh instances to simulate reboots.
 *
 * PRIVACY: NTP server hostnames live only in the bounded config. They are
 * never copied into anchors, snapshots, errors or tokens. No secret, pool,
 * wallet or address field exists anywhere in this module.
 */

/* Bounded server list. The model bound is 4; the REAL ops additionally
 * enforce the compiled lwIP limit (CONFIG_LWIP_SNTP_MAX_SERVERS, currently 1
 * in the tracked sdkconfig) and reject configs exceeding it — no silent
 * truncation. Raising the lwIP limit is a future wiring-gate decision. */
#define POOL_TIME_SNTP_MAX_SERVERS     4u
#define POOL_TIME_SNTP_SERVER_HOST_MAX 64  /* buffer size incl. NUL */

/*
 * Bounded provider configuration. No default server list is baked into B2 —
 * server selection is an explicit future wiring-gate decision, so a
 * defaults-initialized config is intentionally INVALID until servers are set.
 */
typedef struct {
    uint32_t server_count;                       /* 1..POOL_TIME_SNTP_MAX_SERVERS */
    char     servers[POOL_TIME_SNTP_MAX_SERVERS][POOL_TIME_SNTP_SERVER_HOST_MAX];
    bool     accept_dhcp_ntp; /* requires CONFIG_LWIP_DHCP_GET_NTP_SRV (off today) */
    bool     smooth_sync;     /* false = immediate sync (recommended)              */
    uint32_t sync_wait_s;     /* bounded sync window, 1..POOL_TIME_SYNC_WAIT_MAX_S */
} PoolTimeSntpConfig;

/* Provider lifecycle (Phase 2M.1B B2 Stage 12). */
typedef enum {
    POOL_TIME_SNTP_UNINITIALIZED = 0,
    POOL_TIME_SNTP_INITIALIZED,   /* configured; no networking started        */
    POOL_TIME_SNTP_SYNC_PENDING,  /* started; no accepted sync yet            */
    POOL_TIME_SNTP_TRUSTED,       /* an anchor was accepted this boot         */
    POOL_TIME_SNTP_STOPPED,       /* service stopped; an anchor may persist   */
    POOL_TIME_SNTP_ERROR,         /* a platform op failed deterministically   */
    POOL_TIME_SNTP_LIFECYCLE__COUNT
} PoolTimeSntpLifecycle;

/*
 * Injectable platform operations. Return 0 on success, non-zero on failure.
 * The real implementations perform no network traffic at init (start=false);
 * only sntp_start begins polling. None of them may block indefinitely.
 */
typedef struct {
    uint64_t (*monotonic_us)(void);
    int (*sntp_init)(const PoolTimeSntpConfig *cfg); /* configure only; no traffic */
    int (*sntp_start)(void);                         /* begin/restart polling      */
    int (*sntp_stop)(void);                          /* stop polling               */
    int (*sntp_deinit)(void);                        /* tear down; clears callback */
} PoolTimeSntpPlatformOps;

/*
 * Bounded post-synchronization observer (Gate B10).
 *
 * Invoked from pool_time_sntp_handle_sync() — i.e. from the ESP-IDF sync
 * notification callback running in the lwIP tcpip thread — AFTER the anchor
 * decision has been published and OUTSIDE the module critical section, so no
 * provider lock is ever held while it runs. It fires for BOTH acceptance and
 * rejection, because either is a trust-relevant fact the owner of the
 * session runtime must re-evaluate.
 *
 * CONTRACT (enforced by review and by the Gate B10 test suite): an observer
 * may perform ONLY a bounded task notification. It must not touch NVS, the
 * operation coordinator, the pool configuration, the protocol, HTTP or the
 * ASIC; it must not restart the device; it must not block or allocate; and
 * it must never log the configured server name — which it is deliberately
 * not given, along with the epoch and the sync generation.
 */
typedef void (*PoolTimeSntpSyncObserver)(void *ctx, PoolTimeError verdict);

/*
 * Provider instance. Transparent for tests (like PoolSession in Gate B1);
 * production callers treat it as opaque. The published anchor is only ever
 * read/written under the module's bounded critical section, and readers
 * always receive a consistent copy — never a torn one.
 */
typedef struct {
    const PoolTimeSntpPlatformOps *ops;
    PoolTimeSntpConfig             config;   /* stable storage for server names */
    PoolTimeTrustPolicy            policy;
    PoolTimeSntpLifecycle          lifecycle;
    PoolTimeAnchor                 anchor;   /* published under the module lock */
    PoolTimeError                  last_error;
    bool                           bound_to_real_stack;
    PoolTimeSntpSyncObserver       observer;     /* set under the module lock */
    void                          *observer_ctx;
} PoolTimeSntpProvider;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Fill non-server defaults (immediate sync, no DHCP, default wait window).
 * server_count stays 0: the caller MUST provide servers explicitly. */
void pool_time_sntp_config_defaults(PoolTimeSntpConfig *cfg);

/* Validate a config: server count in bounds, every used server name
 * non-empty / NUL-terminated / printable-ASCII without whitespace, wait
 * window in 1..POOL_TIME_SYNC_WAIT_MAX_S. Stable machine codes only. */
PoolTimeError pool_time_sntp_validate_config(const PoolTimeSntpConfig *cfg);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Initialize a provider (UNINITIALIZED -> INITIALIZED). Validates ops,
 * config and policy, then calls ops->sntp_init (which must not start any
 * networking). Re-initializing an already-initialized provider fails
 * deterministically with TIME_ERR_SNTP_INIT and changes nothing. A provider
 * initialized with the REAL ops additionally claims the single real-stack
 * binding slot (TIME_ERR_SNTP_INIT when already claimed).
 */
PoolTimeError pool_time_sntp_init(PoolTimeSntpProvider *p,
                                  const PoolTimeSntpPlatformOps *ops,
                                  const PoolTimeSntpConfig *cfg,
                                  const PoolTimeTrustPolicy *policy);

/* Start (or deterministically restart) synchronization. Valid from
 * INITIALIZED / SYNC_PENDING / TRUSTED / STOPPED. An accepted anchor
 * survives a restart; trust is never dropped by starting. */
PoolTimeError pool_time_sntp_start(PoolTimeSntpProvider *p);

/* Stop synchronization. The anchor (and therefore in-boot trust) persists
 * until deinit. Stopping an already-stopped provider is a TIME_OK no-op. */
PoolTimeError pool_time_sntp_stop(PoolTimeSntpProvider *p);

/* Tear down: clears the anchor, releases the real-stack binding, zeroes the
 * instance back to UNINITIALIZED. Idempotent (TIME_OK on repeat). A fresh
 * init after deinit models a reboot: the provider is untrusted until a new
 * synchronization completes. */
PoolTimeError pool_time_sntp_deinit(PoolTimeSntpProvider *p);

/*
 * Synchronization ingestion — the single anchor-capture path. Called by the
 * real ESP-IDF callback (server-supplied timeval, tcpip thread) or directly
 * by tests as the fake SNTP completion. Validates the candidate (sanity
 * band, no monotonic/trust regression, overflow) and atomically publishes
 * it with a saturating generation bump. A rejected candidate leaves the
 * accepted anchor byte-identical. Ignored deterministically (no mutation)
 * unless the provider is SYNC_PENDING or TRUSTED.
 */
PoolTimeError pool_time_sntp_handle_sync(PoolTimeSntpProvider *p,
                                         uint64_t epoch_s,
                                         uint32_t epoch_us_frac);

/*
 * Register (or clear, with observer == NULL) the bounded post-synchronization
 * observer. Valid only on an initialized provider; UNINITIALIZED is refused
 * with TIME_ERR_NOT_INITIALIZED and changes nothing.
 *
 * ORDERING: register BEFORE pool_time_sntp_start(). Initialization performs
 * no networking (the ESP-IDF service is configured with .start = false), so
 * no synchronization callback can fire in the window between init and
 * registration. Deinit clears the observer along with the rest of the
 * instance, so no callback can reach a torn-down owner.
 */
PoolTimeError pool_time_sntp_set_observer(PoolTimeSntpProvider *p,
                                          PoolTimeSntpSyncObserver observer,
                                          void *ctx);

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

/* Bind this provider into the abstract clock contract (pool_time.h). */
void pool_time_sntp_get_clock(PoolTimeSntpProvider *p, PoolTimeClock *out);

PoolTimeSntpLifecycle pool_time_sntp_lifecycle(const PoolTimeSntpProvider *p);
PoolTimeError pool_time_sntp_last_error(const PoolTimeSntpProvider *p);

/* The actual ESP-IDF-backed ops. Compiled always; performs networking ONLY
 * if a future caller passes it to init AND calls start. Never used in B2. */
const PoolTimeSntpPlatformOps *pool_time_sntp_real_ops(void);

/* Stable machine token for a lifecycle state (diagnostics; never a secret). */
const char *pool_time_sntp_lifecycle_str(PoolTimeSntpLifecycle lc);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_TIME_SNTP_MAX_SERVERS >= 1u && POOL_TIME_SNTP_MAX_SERVERS <= 8u,
               "server list bound out of range");
_Static_assert(POOL_TIME_SNTP_SERVER_HOST_MAX >= 16,
               "server hostname buffer too small");
_Static_assert(POOL_TIME_SNTP_LIFECYCLE__COUNT == 6,
               "lifecycle count changed — review adapter/tests");

#endif /* POOL_TIME_SNTP_H_ */
