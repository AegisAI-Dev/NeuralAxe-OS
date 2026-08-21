#ifndef NX_TUNING_INPUT_H_
#define NX_TUNING_INPUT_H_

#include <stdint.h>
#include <stdbool.h>

#include "nx_telemetry_safety.h"   /* Gate W6.3T-B coherent snapshot        */
#include "tuning_policy.h"         /* W1 environment/input/classifiers      */
#include "tuning_profile.h"        /* registry + hardware context           */

/*
 * NeuralAxe — PRODUCTION W1 POLICY INPUT PROJECTION (Phase 2W, Gate W6.3.1).
 *
 * WHY THIS EXISTS. Until now the production weather pilot stepped the W4
 * runtime with `env = NULL`, so it stopped at NO_PROFILE_SELECTED and
 * tuning_policy_evaluate() could not run on a device however healthy it was.
 * Gate W6.3.1 removes that obstacle, and the only honest way to remove it was
 * to first build the authorities it needs: W6.3T-A gave fan-tach and VRM
 * read-validity classifiers, W6.3T-B gave one coherent telemetry snapshot.
 * This module is the last, purely mechanical step: it turns those authorities
 * into the two W1 structures.
 *
 * WHAT THIS DOES **NOT** ACHIEVE, STATED PLAINLY. Supplying `env` is NECESSARY
 * for the evaluator to run; it is not SUFFICIENT, and this gate does not make
 * tuning_policy_evaluate() reachable on a shipped device on its own. The step
 * refuses earlier, at `observation == NULL`, whenever no weather result has
 * arrived — and in every posture this tree can currently build, none ever can:
 * the committed W3 local schedule ships `enabled = false`, no Kconfig symbol
 * and no production assignment anywhere sets it true, so the schedule verdict
 * is permanently DISABLED, the W6.3 worker is never asked to fetch, and the
 * observation stays NULL forever. Opening that gate is a separate, owner-facing
 * configuration decision — it is what finally permits outbound requests — and
 * it deliberately is not taken here. Until it is taken, this projection is
 * correct, exercised by its tests, and dormant in production.
 *
 * IT FABRICATES NOTHING. Every field below is one of:
 *   A. an OBSERVED fact, classified by a committed W1 classifier;
 *   B. a STRUCTURAL fact declared by the build target;
 *   C. an honest fail-closed SENTINEL for a mechanism that does not exist in
 *      this posture — and in every such case the sentinel is the one the W1
 *      contract itself documents, not a convenient default.
 *
 * There is no (D): if a load-bearing fact had no source, this gate would have
 * stopped rather than invent one. The two that previously had none — fan tach
 * and VRM freshness — are exactly why W6.3T-A and W6.3T-B exist.
 *
 * PURE. No task, no lock, no hardware read, no NVS, no network, no weather
 * dependency and no clock. It takes a snapshot the caller already read and
 * returns two value structures. It CANNOT apply a profile: it produces inputs
 * to a decision, never the decision and never an action.
 *
 * WHAT IT DELIBERATELY DOES NOT SET. `climate_request`, `trusted_time_valid`
 * and `now_epoch_s` are left at their fail-safe values, because
 * weather_runtime_step() overwrites all three from the committed B2/B10
 * trusted-time view and the W1 climate chain. Setting them here would create a
 * second, competing authority for facts weather already owns.
 */

/* ------------------------------------------------------------------ */
/* Structural production facts                                         */
/* ------------------------------------------------------------------ */

/*
 * Facts that come from the BUILD TARGET or from an owner declaration, never
 * from runtime inference.
 *
 * `cooling_installed` / `psu_installed` are owner-declared installation facts.
 * No authoritative production source exists in this tree, so the projection
 * leaves them UNSPECIFIED — which the W1 compatibility rule treats as
 * fail-closed. The firmware must never guess the owner's physical setup: a
 * SuperSink, a fan count or a PSU model inferred from development history
 * would be a safety claim about hardware nobody verified.
 */
typedef struct {
    TuningBoardClass   board;              /* from the declared target       */
    TuningAsicClass    asic;               /* from the declared target       */
    TuningCoolingClass cooling_installed;  /* UNSPECIFIED unless declared    */
    TuningPsuClass     psu_installed;      /* UNSPECIFIED unless declared    */
    uint32_t           policy_generation;  /* integrator-scoped counter      */
} NxTuningStructuralFacts;

/*
 * Fill `out` with the committed Gamma 601 / BM1370 declared identity and the
 * fail-closed installation sentinels. This is the ONLY place the board and
 * ASIC classes are chosen, so no caller duplicates board constants.
 */
void nx_tuning_structural_facts_gamma601(NxTuningStructuralFacts *out);

/* ------------------------------------------------------------------ */
/* Bounded projection outcome                                          */
/* ------------------------------------------------------------------ */

/*
 * Why the projection did or did not produce a usable W1 input. Every value is
 * a bounded token; the zero is the fail-closed one.
 */
typedef enum {
    NX_W1_INPUT_UNAVAILABLE = 0,      /* fail-closed zero                   */
    NX_W1_INPUT_OK,                   /* both producers published           */
    NX_W1_INPUT_TELEMETRY_PARTIAL,    /* one producer has never published   */
    NX_W1_INPUT_TELEMETRY_ABSENT,     /* neither producer has published     */
    NX_W1_INPUT_NO_REGISTRY,          /* no candidate registry supplied     */
    NX_W1_INPUT__COUNT
} NxTuningInputFact;

const char *nx_tuning_input_fact_str(NxTuningInputFact f);

/* True only for a projection W1 may be evaluated against. */
bool nx_tuning_input_usable(NxTuningInputFact f);

/*
 * Bounded, privacy-safe evidence about the projection. Scalars and token ids
 * only: there is no pointer and no character array, so a coordinate, host,
 * URL, credential or installation detail cannot travel here.
 */
typedef struct {
    uint8_t  fact;                    /* NxTuningInputFact token id         */
    bool     telemetry_power_published;
    bool     telemetry_fan_published;
    uint32_t telemetry_power_generation;
    uint32_t telemetry_fan_generation;
    uint8_t  asic_temp_status;        /* TuningSensorStatus token id        */
    uint8_t  vrm_temp_status;
    uint8_t  fan_tach_status;
    bool     fan_control_uncertain;
    bool     emergency_thermal_active;
    bool     sensors_integrity_failed;
    bool     sensors_upgrade_ok;
    uint8_t  mining_health;           /* TuningMiningHealth token id        */
    bool     current_profile_known;
    uint32_t profile_count;
} NxTuningInputDiag;

/* ------------------------------------------------------------------ */
/* THE projection                                                      */
/* ------------------------------------------------------------------ */

/*
 * Project ONE telemetry snapshot plus the structural facts into the two W1
 * structures. Pure, total and NULL-safe.
 *
 * `profiles` / `profile_count` are the candidate registry — production passes
 * tuning_registry_gamma601(). Supplying a registry is SELECTION OF CANDIDATE
 * OBJECTS, never authorization: tuning_profile_auto_eligible() independently
 * requires VALIDATED, and all three production profiles ship UNVALIDATED, so
 * nothing here can make one automatically actionable.
 *
 * Returns the bounded fact. `out_env` and `out_in` are always written when
 * non-NULL, even for an unusable projection, so a caller cannot accidentally
 * evaluate against uninitialised memory — but the caller MUST check
 * nx_tuning_input_usable() before evaluating, and the fail-closed content
 * would refuse an upgrade anyway.
 */
NxTuningInputFact nx_tuning_input_project(const NxTelemetrySafetySnapshot *snap,
                                          const NxTuningStructuralFacts *facts,
                                          const TuningProfile *profiles,
                                          size_t profile_count,
                                          TuningPolicyEnvironment *out_env,
                                          TuningPolicyInput *out_in,
                                          NxTuningInputDiag *out_diag);

_Static_assert(NX_W1_INPUT_UNAVAILABLE == 0,
               "a zeroed projection fact must be the fail-closed token");
_Static_assert(NX_W1_INPUT__COUNT == 5,
               "W6.3.1 projection facts changed — review tokens and tests");

#endif /* NX_TUNING_INPUT_H_ */
