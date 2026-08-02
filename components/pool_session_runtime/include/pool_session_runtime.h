#ifndef POOL_SESSION_RUNTIME_H_
#define POOL_SESSION_RUNTIME_H_

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "pool_session_runtime_core.h"
#include "pool_session_store.h"
#include "pool_session_recovery.h"
#include "pool_operation_coordinator.h"
#include "pool_time.h"
#include "pool_time_sntp.h"
#include "pool_time_source.h"

/*
 * NeuralAxe timed pool sessions — ESP-IDF runtime adapter (Gate B6).
 * Board 601 / BM1370 only.
 *
 * This adapter owns the ONE bounded, feature-flagged runtime that ties the
 * committed B1-B5 foundations together at boot:
 *
 *   NVS already initialized (existing nvs_config_init)
 *     -> read esp_reset_reason() EXACTLY ONCE -> B4 reset classification
 *     -> open the real B3 "nx_tps" store and load the committed state
 *     -> build the B4 time policy from the record and take an initial
 *        UNTRUSTED B2 snapshot under that same floor
 *     -> build the B4 boot context and evaluate the recovery plan
 *     -> bootstrap EXACTLY ONE B5 operation coordinator
 *     -> commit + independently read-back-verify any mandatory B4 proposal
 *     -> classify the runtime state (pure core)
 *     -> publish the sanitized snapshot
 *     -> answer protocol-start ALLOW or HOLD (synchronously)
 *     -> create the single runtime owner task only once ownership is coherent
 *
 * WHAT THIS GATE DOES NOT DO — enforced by construction, asserted by tests:
 *  - no pool configuration change, no source restoration;
 *  - no Stratum stop/start/reconnect for a timed session;
 *  - no target or source identity verification, no mining authorization;
 *  - no session creation / Restore Now / acknowledgement from HTTP;
 *  - no pool PATCH integration, no OTA, no device restart;
 *  - no frontend, weather or hardware interaction.
 * Gate B7 owns controlled apply, restore and live verification.
 *
 * OWNERSHIP: the runtime owner task is the ONLY production owner of
 * session-store mutation, B4 re-evaluation, B5 mutating transitions and the
 * B2 timed-session time lifecycle. Boot-time work runs synchronously in
 * app_main BEFORE that task exists, so the two never overlap.
 *
 * LOCKING: the B5 coordinator serializes its own state internally and its
 * API takes NO callbacks. This adapter additionally guarantees that NO NVS
 * operation, NO SNTP operation and NO external callback runs while a
 * coordinator call is in progress — a debug depth counter makes that
 * testable (pool_session_runtime_coordinator_depth()).
 *
 * INSTANCE MODEL: every instance is CALLER-OWNED (the B3/B5 convention), so
 * the test suite exercises the full adapter — including the real ESP-IDF
 * NVS backend — without the production singleton. The single production
 * instance exists ONLY when CONFIG_NX_TIMED_SESSIONS is enabled; with the
 * flag off pool_session_runtime_default_instance() returns NULL, no task is
 * created, "nx_tps" is never opened and SNTP is never initialized.
 */

/* ------------------------------------------------------------------ */
/* Injected platform dependencies                                      */
/* ------------------------------------------------------------------ */

/*
 * The complete platform seam. Production fills this from
 * pool_session_runtime_production_deps(); tests inject deterministic fakes
 * (fake store backend, fake SNTP ops, fake monotonic clock, fake reset
 * reason). No real NTP, DNS, pool or hardware access exists in any test.
 */
typedef struct {
    /* B3 persistence backend (real NVS ops in production). */
    const PoolStoreBackendOps *store_ops;
    void                      *store_ctx;

    /* B2 SNTP platform ops. NULL means "no time provider is available":
     * the runtime then reports an unconfigured time source and HOLDS. */
    const PoolTimeSntpPlatformOps *sntp_ops;

    /* Read the raw esp_reset_reason() value. Called EXACTLY ONCE per
     * feature-enabled boot initialization; the raw value is classified
     * immediately and never stored, published or logged. */
    int32_t (*read_reset_reason_raw)(void);

    /* Monotonic microseconds since boot (esp_timer_get_time in production). */
    uint64_t (*monotonic_us)(void);

    /*
     * Product-configured trusted-time source. NULL or "" means NO source is
     * configured — the runtime does NOT invent one, does NOT contact any
     * public NTP server, and keeps protocol startup HELD when the recovery
     * plan requires trusted time.
     */
    const char *ntp_server;

    /* Bounded post-reboot synchronization window (seconds). 0 selects the
     * committed B2 default; values above the B2 hard maximum are clamped. */
    uint32_t sync_wait_limit_s;

    /*
     * Gate B10 observation-only mode (CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
     * in production; injected here so BOTH values are deterministically
     * testable). It authorizes NOTHING: it only permits the trusted-time
     * provider to start after network readiness on a device with no session
     * owner, so a supervised pilot can validate the time foundation. It
     * never creates a session, writes the store, takes a lease, commits a
     * heartbeat, touches the pool/protocol/ASIC or restarts the device.
     */
    bool observe_enabled;

    /*
     * Gate B10.1 observation-pilot link fact. OPTIONAL and diagnostic ONLY:
     * NULL means "no link fact is available", which is the shipped binding and
     * the binding of every build without
     * CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS. It is read
     * only by the bounded pilot diagnostics on the single owner task, it must
     * never block, and it feeds NO decision anywhere in B1-B10: it cannot
     * start or stop the time provider, change a plan, take a lease, write the
     * store, touch the pool/protocol/ASIC or restart the device. Production
     * binds a bounded Wi-Fi association read; tests inject a deterministic
     * fake. The callee returns association state only — no SSID, BSSID, RSSI,
     * address or any other identity ever leaves it.
     */
    bool (*link_up)(void);
} PoolSessionRuntimeDeps;

/* ------------------------------------------------------------------ */
/* Runtime instance                                                    */
/* ------------------------------------------------------------------ */

/*
 * Caller-owned instance. Every large working struct lives HERE, never on
 * the runtime task stack. Transparent for tests; treat as opaque otherwise.
 */
typedef struct {
    PoolSessionRuntimeDeps deps;
    bool                   initialized;
    bool                   booted;

    /* B3 */
    PoolSessionStore   store;
    PoolStoreLoadInfo  load_info;
    PoolStoreResult    store_result;
    bool               store_opened;
    bool               store_loaded;
    bool               record_present;
    PoolSessionRecord  record;      /* the committed record (STORE_OK only) */
    PoolSessionRecord  work_record; /* proposal staging + read-back copy    */
    uint32_t           committed_generation;

    /* Reset classification — the raw value is read once and never retained. */
    bool                  reset_reason_read;
    uint32_t              reset_reason_read_count; /* audit: must equal 1 */
    PoolSessionResetClass reset_class;

    /* B2 */
    PoolTimeTrustPolicy  time_policy;
    PoolTimeSntpProvider time_provider;
    PoolTimeClock        clock;
    PoolTimeSnapshot     time_snapshot;
    bool                 time_source_configured;
    bool                 time_provider_initialized;
    bool                 time_provider_started;

    /* B10 — trusted-time source policy, bounded start budget and the
     * sanitized diagnostics view. None of these fields is a string, so no
     * hostname can be retained outside deps.ntp_server itself. */
    PoolTimeSourceValidation  time_source_validation;
    PoolTimeSourceRetryPolicy time_retry_policy;
    PoolTimeSourceDiagnostics time_diag;
    uint32_t                  time_start_attempts;
    uint32_t                  time_observe_elapsed_s; /* observation window only */
    uint64_t                  time_last_attempt_us;
    bool                      time_attempt_made;
    bool                      time_attempts_exhausted;
    bool                      time_observation_active;
    PoolTimeError             time_last_sync_result;
    uint32_t                  time_sync_callbacks; /* audit; written in the cb */

    /* B4 */
    PoolSessionBootContext  boot_ctx;
    PoolSessionRecoveryPlan plan;

    /* B5 */
    PoolOperationCoordinator coord;
    PoolOperationLeaseToken  token;
    PoolOperationState       lease;
    PoolOperationStatus      bootstrap_status;

    /* Persistence-before-action evidence for the CURRENT evaluation. */
    bool            persist_attempted;
    bool            persist_verified; /* committed AND read-back verified */
    PoolStoreResult persist_result;

    /* B6 — generation-aware persistence-proposal tracking (RAM-only; reset
     * naturally by RAM loss on a real reboot). `proposal` is the normalized
     * semantic proposal of the CURRENT evaluation. */
    PoolRuntimeProposalTracker tracker;
    PoolRuntimeProposal        proposal;

    /* B6 */
    PoolRuntimeControl  control;
    PoolRuntimeDecision decision;
    PoolRuntimeSnapshot snapshot;

    /* Task */
    void *task_handle; /* TaskHandle_t; opaque here to keep the header thin */
    bool  task_running;
} PoolSessionRuntime;

/* ------------------------------------------------------------------ */
/* Lifecycle (synchronous; safe to call only from a task context)      */
/* ------------------------------------------------------------------ */

/* Fill the production dependency set: real NVS ops bound to `backend`, real
 * SNTP ops, esp_reset_reason(), esp_timer_get_time(), and the configured
 * (possibly empty) NTP server. */
void pool_session_runtime_production_deps(PoolSessionRuntimeDeps *out,
                                          PoolStoreNvsBackend *backend);

/* Bind dependencies. Does NOT open the store, touch NVS or start anything. */
PoolRuntimeStatus pool_session_runtime_init(PoolSessionRuntime *rt,
                                            const PoolSessionRuntimeDeps *deps);

/* Close the store, stop the task, deinit the time provider and the
 * coordinator, and zero the instance. Idempotent. */
PoolRuntimeStatus pool_session_runtime_deinit(PoolSessionRuntime *rt);

/*
 * THE synchronous boot sequence (steps 2-12 of the gate order). Must be
 * called AFTER the existing NVS subsystem is initialized and BEFORE the
 * protocol-start barrier. Idempotent: a second call returns
 * RUNTIME_ERR_ALREADY_INITIALIZED and re-reads nothing (in particular it
 * never reads esp_reset_reason() twice).
 */
PoolRuntimeStatus pool_session_runtime_boot(PoolSessionRuntime *rt);

/*
 * Step 13 — the protocol-start answer. Total and fail-closed: an instance
 * that is NULL, uninitialized, un-booted or in any unresolved posture
 * returns HOLD. Nothing in Gate B6 ever turns a HOLD back into an ALLOW.
 */
PoolRuntimeProtocolPermission pool_session_runtime_protocol_permission(
    const PoolSessionRuntime *rt);

/*
 * Step 14 — create the single bounded runtime owner task. Refused with
 * RUNTIME_ERR_TASK_ALREADY_RUNNING when ANY runtime task already exists
 * (a module-wide single-task guard), and with RUNTIME_ERR_NOT_INITIALIZED
 * before a successful boot.
 */
PoolRuntimeStatus pool_session_runtime_start_task(PoolSessionRuntime *rt);

/* Ask the task to stop and wait (bounded) for it to exit. Stopping never
 * releases a protocol hold and never frees ownership. Idempotent. */
PoolRuntimeStatus pool_session_runtime_stop_task(PoolSessionRuntime *rt);

/* Post a bounded event bit mask to the runtime task. Unknown bits are
 * dropped. Safe to call before the task exists (the bits are latched). */
PoolRuntimeStatus pool_session_runtime_notify(PoolSessionRuntime *rt,
                                              uint32_t event_bits);

/*
 * Gate B10 — the ONLY notification path allowed from the B2 SNTP sync
 * callback (lwIP tcpip thread). It performs a bounded FreeRTOS task
 * notification and nothing else: no NVS, no B5 call, no protocol or pool
 * operation, no restart, no HTTP, no blocking, no allocation and no logging
 * of the configured server name.
 *
 * Unlike pool_session_runtime_notify() it NEVER latches into the control
 * block when no task exists — that would be an unsynchronized
 * read-modify-write from a foreign thread. It returns
 * RUNTIME_ERR_NOT_INITIALIZED instead, and the bounded tick re-evaluates.
 */
PoolRuntimeStatus pool_session_runtime_notify_from_callback(PoolSessionRuntime *rt,
                                                            uint32_t event_bits);

/* Copy the published sanitized snapshot. Never a torn read. */
PoolRuntimeStatus pool_session_runtime_snapshot(const PoolSessionRuntime *rt,
                                                PoolRuntimeSnapshot *out);

/* Number of times the raw reset reason was read on this instance. The gate
 * contract requires exactly 1 after a feature-enabled boot. */
uint32_t pool_session_runtime_reset_reason_reads(const PoolSessionRuntime *rt);

/* Number of runtime tasks that currently exist module-wide (0 or 1). */
uint32_t pool_session_runtime_task_count(void);

/* Nesting depth of in-progress B5 coordinator calls. Every NVS and SNTP op
 * asserts this is 0, proving contract 11 deterministically. */
uint32_t pool_session_runtime_coordinator_depth(void);

/* ------------------------------------------------------------------ */
/* Gate B10 — sanitized trusted-time diagnostics                       */
/* ------------------------------------------------------------------ */

/*
 * Copy the published sanitized trusted-time diagnostics. Never a torn read.
 * The model has NO string field, so it cannot carry the server hostname, a
 * resolved address, DNS error text, a server index, a raw SNTP status, a raw
 * epoch, the sync generation, a pool identity, a session id or a lease token.
 *
 * This is DIAGNOSTIC ONLY. Nothing in it authorizes target mining, releases
 * a protocol hold, or substitutes for the B4/B6 re-evaluation that must
 * follow a trust change.
 */
PoolRuntimeStatus pool_session_runtime_time_diagnostics(const PoolSessionRuntime *rt,
                                                        PoolTimeSourceDiagnostics *out);

/* Audit: bounded provider start attempts consumed this boot. */
uint32_t pool_session_runtime_time_start_attempts(const PoolSessionRuntime *rt);

/* Audit: synchronization callbacks observed this boot (accepted + refused). */
uint32_t pool_session_runtime_time_sync_callbacks(const PoolSessionRuntime *rt);

/* True while the provider runs for OBSERVATION ONLY — i.e. no B4 plan
 * required the trust. Observation never authorizes anything. */
bool pool_session_runtime_time_observation_active(const PoolSessionRuntime *rt);

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
/*
 * Gate B7 executor hook (exists ONLY under the execution flag; with it off
 * the B6 runtime is byte-for-byte the committed hold-only runtime).
 *
 * The single runtime owner task calls the hook once per loop iteration —
 * AFTER any event-driven re-evaluation, so the executor always consumes a
 * fresh B4 plan. The hook returns true while the executor OWNS the session
 * flow; the task then defers its own plan-persistence and lease-reconcile
 * machinery to the executor so exactly ONE mutating session owner exists.
 * Registration is one-shot at boot, before the owner task starts stepping.
 */
typedef bool (*PoolRuntimeExecutorHook)(void *ctx);
void pool_session_runtime_register_executor(PoolRuntimeExecutorHook hook, void *ctx);
#endif /* CONFIG_NX_TIMED_SESSIONS_EXECUTION */

#ifdef CONFIG_NX_TIMED_SESSIONS_API
/*
 * Gate B8 API command hook (exists ONLY under the API flag; with it off the
 * runtime is byte-for-byte the committed B6/B7 runtime).
 *
 * The single runtime owner task calls the hook once per loop iteration —
 * FIRST, so the flow-ownership answer is fresh on every path — and it is
 * the ONLY consumer of the bounded API command mailbox. No second mutating
 * task is created. The hook returns true while the API owns the flow of a
 * session it CREATED this boot; the task then withholds its BOOT-time B4
 * re-planning and B6 re-classification from that in-boot creation (see
 * runtime_reevaluate). Registration is one-shot at boot.
 */
typedef bool (*PoolRuntimeApiCommandHook)(void *ctx);
void pool_session_runtime_register_api_commands(PoolRuntimeApiCommandHook hook, void *ctx);
#endif /* CONFIG_NX_TIMED_SESSIONS_API */

/*
 * Commit ONE bounded TARGET_ACTIVE liveness heartbeat: raise the persisted
 * latest-accepted trusted-epoch floor to `epoch_s` through the COMMITTED
 * generation-aware Gate B6 persistence engine (normalized proposal ->
 * dedupe -> commit -> independent reload -> exact readback verification ->
 * B5 proof).
 *
 * It builds an EPOCH-FLOOR-ONLY proposal from a neutral plan, so it cannot
 * change the persisted state, failure code, counters, duration or ANY
 * deadline field — extending a deadline is structurally impossible.
 *
 * Returns RUNTIME_OK when a commit was performed AND durably proven,
 * RUNTIME_ERR_PERSIST_DUPLICATE when nothing needed writing (already
 * durable — not a write), RUNTIME_ERR_STORE_UNCERTAIN when the outcome is
 * unknowable (the B5 recovery guard is entered), and a bounded failure
 * status otherwise. Callable ONLY from the single owner task.
 */
PoolRuntimeStatus pool_session_runtime_commit_epoch_heartbeat(PoolSessionRuntime *rt,
                                                              uint64_t epoch_s);

/* ------------------------------------------------------------------ */
/* Production singleton (feature-gated)                                */
/* ------------------------------------------------------------------ */

/* The compile-time value of CONFIG_NX_TIMED_SESSIONS. */
bool pool_session_runtime_feature_enabled(void);

/*
 * The single production instance, or NULL when the feature is disabled.
 * With the feature disabled NO instance storage exists, so there is nothing
 * to bootstrap, no task to create, no "nx_tps" namespace to open and no
 * SNTP service to initialize.
 */
PoolSessionRuntime *pool_session_runtime_default_instance(void);

#endif /* POOL_SESSION_RUNTIME_H_ */
