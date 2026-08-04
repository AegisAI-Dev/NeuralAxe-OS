/*
 * NeuralAxe Gate W6 / W6.1 — AUTHORITATIVE FACT ADAPTER.
 *
 * The pure checker in nx_weather_pilot_diag.c decides; this file is the only
 * place that GATHERS. Every field is stamped with where it came from, and a
 * field this firmware cannot obtain from its owning subsystem is stamped
 * UNAVAILABLE rather than defaulted to a convenient zero.
 *
 * SOURCE TABLE — audited against the tree at Gate W6.1.
 *
 *  feature_posture   STRUCTURAL. Decided by CONFIG_NX_WEATHER_AWARE_TUNING /
 *                    _SOURCE_POLICY / this TU existing at all, plus the W5
 *                    validator's own status. Update ownership: the build.
 *                    Lifetime: the image. Unknown: impossible.
 *
 *  execution         STRUCTURAL when CONFIG_NX_TIMED_SESSIONS_EXECUTION is
 *                    absent — the Gate B7 symbols are not linked, which is
 *                    stronger evidence than any runtime read. When the flag
 *                    IS set the pilot posture is already violated, and the
 *                    adapter reports that rather than inspecting further.
 *
 *  command_api       STRUCTURAL, same argument for CONFIG_NX_TIMED_SESSIONS_API:
 *                    with the flag absent no command route is registered
 *                    anywhere in the image.
 *
 *  ownership         W6.1. OBSERVED from the committed Gate B6 published
 *  session_counters  runtime snapshot (pool_session_runtime_snapshot), which
 *  mining_posture    carries the Gate B5 lease owner, the committed proposal
 *                    count and the boot mining policy. Update ownership: the
 *                    single runtime owner task. Lifetime: the boot.
 *                    STRUCTURAL when CONFIG_NX_TIMED_SESSIONS is absent: no
 *                    instance storage exists, so no owner and no session
 *                    mutation can exist, and nothing can inhibit source
 *                    mining. UNAVAILABLE when the instance exists but did not
 *                    publish a snapshot.
 *
 *  hardware_counters W6.1. OBSERVED from the mutation counters incremented at
 *  pool_counters     the OWNING mutation boundaries — the nvs_config writer
 *  restart_counters  (frequency, voltage, fan, thermal, pool, protocol), the
 *  tuning_snapshot   restart request sites and the OTA acceptance points —
 *                    compared against the pilot baseline. The weather
 *                    component never increments them; it reads a coherent
 *                    snapshot. ABSENT until the baseline exists (the window
 *                    has not opened yet, which is not a claim of health), and
 *                    UNAVAILABLE when CONFIG_NX_MUTATION_OBSERVABILITY is
 *                    absent, when the baseline was refused, or when an
 *                    authority stopped answering.
 *
 *  resources         OBSERVED from ESP-IDF: heap_caps_get_free_size /
 *                    _minimum_free_size (MALLOC_CAP_INTERNAL),
 *                    uxTaskGetStackHighWaterMark(NULL), esp_timer_get_time().
 *                    Update ownership: the heap allocator, the scheduler and
 *                    the monotonic timer. Lifetime: instantaneous.
 */

#include "sdkconfig.h"

#ifdef CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS

#include <string.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nx_weather_pilot_facts.h"
#include "nx_mutation_baseline.h"
#include "pool_session_runtime.h"

/* ------------------------------------------------------------------ */
/* Timed-session authorities (Gate B5 / B6)                            */
/* ------------------------------------------------------------------ */

static void gather_session_facts(NxWeatherPilotPosture *out)
{
#ifdef CONFIG_NX_TIMED_SESSIONS
    PoolSessionRuntime *rt = pool_session_runtime_default_instance();
    PoolRuntimeSnapshot snap;

    if (rt != NULL && pool_session_runtime_snapshot(rt, &snap) == RUNTIME_OK &&
        pool_runtime_snapshot_valid(&snap)) {
        out->facts.ownership        = NX_WX_FACT_OBSERVED;
        out->facts.session_counters = NX_WX_FACT_OBSERVED;
        out->facts.mining_posture   = NX_WX_FACT_OBSERVED;

        out->b5_owner_present = (snap.lease_owner != OP_OWNER_NONE);
        /*
         * Committed timed-session proposals are the durable mutations this
         * subsystem performs. They are counted for the WHOLE boot rather than
         * against a baseline: a recommendation-only pilot must see none at
         * any point, which is the stricter claim.
         */
        out->session_mutation_count = snap.proposal_commits;
        /*
         * Source mining is preserved only when the boot policy still allows
         * it AND the runtime is not holding the protocol. Anything else means
         * the pilot changed the device's operating posture.
         */
        out->source_mining_allowed =
            (snap.mining_policy == POOL_BOOT_MINING_ALLOW_SOURCE) &&
            (snap.protocol == POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE);
        return;
    }
    /* The instance exists but published nothing usable: say so. */
    out->facts.ownership        = NX_WX_FACT_UNAVAILABLE;
    out->facts.session_counters = NX_WX_FACT_UNAVAILABLE;
    out->facts.mining_posture   = NX_WX_FACT_UNAVAILABLE;
    out->b5_owner_present       = false;
    out->session_mutation_count = 0u;
    out->source_mining_allowed  = false;
#else
    /*
     * No runtime instance storage, no coordinator, no session record and no
     * protocol hold exist in this image, so no owner and no session mutation
     * are possible and nothing can inhibit source mining. That is a property
     * of the link, not a reading.
     */
    out->facts.ownership        = NX_WX_FACT_STRUCTURAL;
    out->facts.session_counters = NX_WX_FACT_STRUCTURAL;
    out->facts.mining_posture   = NX_WX_FACT_STRUCTURAL;
    out->b5_owner_present       = false;
    out->session_mutation_count = 0u;
    out->source_mining_allowed  = true;
#endif
}

/* ------------------------------------------------------------------ */
/* Baseline readiness prerequisites (Gate W6.1)                        */
/* ------------------------------------------------------------------ */

void nx_weather_pilot_prereq_gather(NxBaselinePrereq *out, bool host_task_valid)
{
    if (out == NULL) {
        return;
    }
    /* Start entirely UNREADY: a field this function cannot source blocks the
     * baseline rather than passing it. */
    memset(out, 0, sizeof(*out));

    /*
     * The provider is registered by main immediately after nvs_config_init()
     * returned ESP_OK, so its presence IS the evidence that the boot
     * configuration was loaded. Without observability compiled in no provider
     * can be registered, and the baseline is correctly unreachable.
     */
    out->config_loaded  = nx_tuning_snapshot_provider_present();
    out->host_task_valid = host_task_valid;

#ifdef CONFIG_NX_TIMED_SESSIONS
    {
        PoolSessionRuntime *rt = pool_session_runtime_default_instance();
        PoolRuntimeSnapshot snap;

        if (rt != NULL &&
            pool_session_runtime_snapshot(rt, &snap) == RUNTIME_OK &&
            pool_runtime_snapshot_valid(&snap)) {
            out->runtime_snapshot_valid = true;
            out->owner_none             = (snap.lease_owner == OP_OWNER_NONE);
            out->protocol_normal_source =
                (snap.protocol == POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE) &&
                (snap.mining_policy == POOL_BOOT_MINING_ALLOW_SOURCE);
            /* No session may be present, no restore obligation outstanding,
             * no persistence in flight and no recovery posture entered. */
            out->session_posture_clean =
                !snap.session_present && !snap.restore_required &&
                !snap.persistence_pending &&
                (snap.recovery_error == RECOVERY_OK);
            /*
             * The published runtime snapshot has no terminal_pending flag —
             * that belongs to the B5 coordinator state. Its authoritative
             * signal is the runtime STATE itself: RUNTIME_TERMINAL_PENDING is
             * precisely "a safe retained terminal result awaiting ack".
             */
            out->no_terminal_pending = (snap.state != RUNTIME_TERMINAL_PENDING);
        }
        /* Anything else leaves every field false: unavailable, not agreeable. */
    }
#else
    /*
     * No runtime instance storage, no coordinator, no session record and no
     * protocol hold exist in this image. Every timed-session prerequisite is
     * satisfied by the LINK, which is stronger than a runtime read.
     */
    out->runtime_snapshot_valid = true;
    out->owner_none             = true;
    out->protocol_normal_source = true;
    out->session_posture_clean  = true;
    out->no_terminal_pending    = true;
#endif
}

/* ------------------------------------------------------------------ */
/* Mutation authorities (Gate W6.1)                                    */
/* ------------------------------------------------------------------ */

static void stamp_mutation(NxWeatherPilotPosture *out, NxWeatherFactState s)
{
    out->facts.hardware_counters = s;
    out->facts.pool_counters     = s;
    out->facts.restart_counters  = s;
}

static void gather_mutation_facts(NxWeatherPilotPosture *out)
{
#ifdef CONFIG_NX_MUTATION_OBSERVABILITY
    NxMutationComparison cmp;

    (void)nx_mutation_baseline_compare(&cmp);

    if (!cmp.baseline_ready) {
        /*
         * The observation window has not opened. Nobody has supplied these
         * facts yet, which is ABSENT — explicitly not a claim that nothing
         * happened. The pilot is unhealthy until the baseline exists, and
         * that is the correct answer during the settle period.
         */
        stamp_mutation(out, NX_WX_FACT_ABSENT);
        out->facts.tuning_snapshot = NX_WX_FACT_ABSENT;
        return;
    }
    if (!cmp.counters_readable) {
        stamp_mutation(out, NX_WX_FACT_UNAVAILABLE);
    } else {
        stamp_mutation(out, NX_WX_FACT_OBSERVED);
        out->counter_history_lost = cmp.history_lost;
        if (!cmp.history_lost) {
            out->hardware_write_count  = cmp.delta.hardware_total;
            out->pool_write_count      = cmp.delta.pool_total;
            out->protocol_write_count  = cmp.delta.protocol_total;
            out->restart_request_count = cmp.delta.restart_total;
            out->ota_request_count     = cmp.delta.ota_total;
        }
    }

    /*
     * An unreadable configuration is UNAVAILABLE, never "changed": reporting
     * a mutation that was not observed is as wrong as hiding one that was.
     */
    if (!cmp.tuning_readable) {
        out->facts.tuning_snapshot = NX_WX_FACT_UNAVAILABLE;
        out->tuning_unchanged      = false;
    } else {
        out->facts.tuning_snapshot = NX_WX_FACT_OBSERVED;
        out->tuning_unchanged      = cmp.tuning_unchanged;
    }
#else
    /*
     * No counter exists anywhere in this image, so there is nothing to read.
     * The pilot fails closed rather than reporting five reassuring zeroes it
     * has no authority for.
     */
    (void)out;
    stamp_mutation(out, NX_WX_FACT_UNAVAILABLE);
    out->facts.tuning_snapshot = NX_WX_FACT_UNAVAILABLE;
    out->tuning_unchanged      = false;
#endif
}

/* ------------------------------------------------------------------ */

void nx_weather_pilot_facts_gather(const NxWeatherSourceConfig *cfg,
                                   NxWeatherSourceStatus source,
                                   NxWeatherPilotPosture *out,
                                   NxWeatherPilotResources *res)
{
    if (out == NULL) {
        return;
    }
    /* Start fully ABSENT. Anything this function does not explicitly stamp
     * stays unusable, so a future field added to the posture cannot silently
     * inherit a passing default. */
    memset(out, 0, sizeof(*out));

    /* --- feature posture: STRUCTURAL + the W5 validator's own verdict --- */
    out->facts.feature_posture = NX_WX_FACT_STRUCTURAL;
    out->weather_enabled       = true;   /* this TU only exists with the flags */
    out->source_policy_valid   = nx_weather_source_ready(source);
    out->recommendation_only   = (cfg != NULL) ? cfg->recommendation_only : false;

    /* --- Gate B7 execution: STRUCTURAL from the linked flag set --- */
    out->facts.execution = NX_WX_FACT_STRUCTURAL;
#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    out->execution_available = true;    /* already a violation for this pilot */
    out->b7_action_observed  = true;    /* cannot prove otherwise: fail closed */
#else
    out->execution_available = false;   /* the symbols are not in the image   */
    out->b7_action_observed  = false;
#endif

    /* --- Gate B8 command API: STRUCTURAL --- */
    out->facts.command_api = NX_WX_FACT_STRUCTURAL;
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    out->command_api_available = true;  /* already a violation for this pilot */
#else
    out->command_api_available = false;
#endif

    /* --- Gate B5/B6 ownership, session mutations and mining posture --- */
    gather_session_facts(out);

    /* --- Gate W6.1 mutation counters and tuning fingerprint --- */
    gather_mutation_facts(out);

    /* --- resources: OBSERVED from ESP-IDF --- */
    if (res != NULL) {
        res->free_heap        = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        res->min_free_heap    = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        res->stack_high_water = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        res->uptime_us        = (uint64_t)esp_timer_get_time();
        out->facts.resources  = NX_WX_FACT_OBSERVED;
    } else {
        out->facts.resources = NX_WX_FACT_ABSENT;
    }
}

#endif /* CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS */
