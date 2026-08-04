#ifndef NX_MUTATION_COUNTERS_H_
#define NX_MUTATION_COUNTERS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe — AUTHORITATIVE MUTATION OBSERVABILITY (Phase 2W, Gate W6.1).
 *
 * A supervised recommendation-only pilot has to prove a negative: that weather
 * changed nothing. Gate W6 could not prove it, because the owning subsystems
 * exposed no observable at their mutation boundaries — and a counter the
 * OBSERVER increments would be self-reported evidence, not authority.
 *
 * W6.1 puts saturating counters where the mutation actually happens, inside
 * the subsystem that owns it, behind CONFIG_NX_MUTATION_OBSERVABILITY
 * (default n). The weather component never increments them; it only reads the
 * snapshot.
 *
 * CONFIGURATION MUTATION IS NOT RUNTIME ACTIVITY. This is the whole semantic
 * point. The following are NORMAL and are deliberately NOT counted:
 *
 *   - fan PWM the thermal controller adjusts every cycle;
 *   - the ASIC frequency ramp during boot;
 *   - the initial voltage application during boot;
 *   - a pool reconnect that reuses unchanged configuration;
 *   - protocol state transitions on unchanged configuration;
 *   - share submission, Wi-Fi reconnects, trusted-time synchronisation;
 *   - loading the configuration cache at boot, which reads NVS directly and
 *     never passes through the instrumented writer.
 *
 * What IS counted is an owner-authorised change to STORED CONFIGURATION, plus
 * two explicit control-boundary events (restart request, OTA acceptance).
 * Because the boundary is the persistent-configuration writer, runtime
 * activity cannot reach it: the thermal controller drives PWM without writing
 * a setting, and a reconnect re-reads configuration without writing it.
 *
 * EFFECTIVE CHANGES ONLY. A write that stores the value already present is a
 * no-op and does NOT increment: the pilot asks "did the configuration
 * change?", not "did someone call the setter?". A rejected or failed write
 * does not increment either, because nothing changed — but a failure is also
 * not silently benign, which is why the snapshot carries a validity flag and
 * the pilot treats an unreadable counter as fail-closed rather than zero.
 *
 * A COUNTER PROVES THAT, NEVER WHO. It records that a class of configuration
 * changed; it attributes nothing. The invariant a pilot proves with it is
 * "nothing changed at all", which is the stronger claim and the only one
 * these counters can support.
 */

/* ------------------------------------------------------------------ */
/* Counter identity                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    NX_MUT_FREQUENCY_CONFIG = 0, /* stored ASIC frequency setting           */
    NX_MUT_VOLTAGE_CONFIG,       /* stored core-voltage setting             */
    NX_MUT_FAN_CONFIG,           /* fan mode / percent / min / curve / hyst */
    NX_MUT_THERMAL_CONFIG,       /* temp target, overheat, thermal envelope */
    NX_MUT_POOL_CONFIG,          /* pool identity, port, account, fallback  */
    NX_MUT_PROTOCOL_CONFIG,      /* protocol selection, TLS, extranonce, v2 */
    NX_MUT_RESTART_REQUEST,      /* an explicit software restart requested  */
    NX_MUT_OTA_FIRMWARE,         /* a firmware OTA accepted                 */
    NX_MUT_OTA_WEB,              /* a web-image OTA accepted                */
    NX_MUT__COUNT
} NxMutationCounterId;

/* Stable machine token; never a value, identity or path. Total. */
const char *nx_mutation_counter_str(NxMutationCounterId id);

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

#define NX_MUTATION_SNAPSHOT_VERSION 1u

/*
 * A bounded, coherent, read-only view. Fixed-width scalars only: no string,
 * no pointer, no identity, no private value can be represented here.
 *
 * `saturated_mask` has bit i set when counter i reached UINT32_MAX. A
 * saturated counter has lost its exact history, so the pilot must treat it as
 * a failure rather than as a large number — saturation is observable
 * precisely so it can fail closed instead of wrapping to a value that would
 * compare equal to a baseline.
 */
typedef struct {
    uint32_t version;                     /* NX_MUTATION_SNAPSHOT_VERSION   */
    uint32_t size;                        /* sizeof(NxMutationSnapshot)     */
    bool     valid;                       /* false => treat as unavailable  */
    uint32_t counters[NX_MUT__COUNT];
    uint32_t saturated_mask;
} NxMutationSnapshot;

/*
 * Read every counter under one lock so the set is mutually coherent: a pilot
 * comparing nine counters against a baseline must not see a mixture of
 * before- and after-values. Zeroes `out` and reports valid=false on any
 * failure (NULL, wrong size, observability not compiled in). Never allocates,
 * never blocks on network or storage.
 */
bool nx_mutation_counters_snapshot(NxMutationSnapshot *out, size_t out_size);

/* True when any counter in the snapshot has saturated. */
bool nx_mutation_snapshot_saturated(const NxMutationSnapshot *s);

/*
 * Compare two coherent snapshots. Returns true only when every counter is
 * EQUAL, neither snapshot is saturated, and no counter regressed. `*first`
 * receives the first differing counter id when the result is false.
 */
bool nx_mutation_snapshot_unchanged(const NxMutationSnapshot *baseline,
                                    const NxMutationSnapshot *now,
                                    NxMutationCounterId *first);

/* ------------------------------------------------------------------ */
/* Deltas against a pilot baseline                                     */
/* ------------------------------------------------------------------ */

/*
 * The per-class arithmetic a pilot needs, computed once and pure so it can be
 * tested without hardware. Totals saturate rather than wrap, for the same
 * reason the counters do.
 *
 * `regressed` means a counter is BELOW its baseline: the counters were reset,
 * or the two snapshots came from different authorities. Either way the pilot
 * can no longer prove what happened in between, so it is a failure and not a
 * conveniently small delta.
 */
typedef struct {
    bool     valid;      /* false => the deltas below mean nothing          */
    bool     regressed;
    bool     saturated;  /* either snapshot had a saturated counter         */
    uint32_t counters[NX_MUT__COUNT];

    uint32_t hardware_total; /* frequency + voltage + fan + thermal         */
    uint32_t pool_total;
    uint32_t protocol_total;
    uint32_t restart_total;
    uint32_t ota_total;      /* firmware + web                              */
} NxMutationDelta;

/*
 * Pure, total and NULL-safe. Returns false (with `out` zeroed and invalid)
 * when either snapshot is unusable, the versions or sizes disagree, either is
 * saturated, or any counter regressed — every one of which is a state in
 * which "nothing changed" is unprovable rather than true.
 */
bool nx_mutation_delta_compute(const NxMutationSnapshot *baseline,
                               const NxMutationSnapshot *now,
                               NxMutationDelta *out);

/* True only for a valid delta whose every class total is zero. */
bool nx_mutation_delta_clean(const NxMutationDelta *d);

/* ------------------------------------------------------------------ */
/* Increment API — owning mutation boundaries ONLY                     */
/* ------------------------------------------------------------------ */

/*
 * Call ONLY from the subsystem that owns the mutation, and only once the
 * change is known to be effective. Saturating: increments to UINT32_MAX and
 * stays there; it never wraps to zero, because a wrapped counter could
 * silently compare equal to a pilot baseline.
 *
 * With CONFIG_NX_MUTATION_OBSERVABILITY unset this is a no-op the compiler
 * removes entirely, so instrumented call sites keep their exact behaviour and
 * the default image contains no observability symbol or RAM object.
 */
#ifdef CONFIG_NX_MUTATION_OBSERVABILITY
void nx_mutation_counter_note(NxMutationCounterId id);
#else
static inline void nx_mutation_counter_note(NxMutationCounterId id)
{
    (void)id;
}
#endif

/* Test-only reset. Never called from production code. */
void nx_mutation_counters_reset_for_test(void);

/*
 * Test-only preset. Exists so the SATURATING behaviour can be proven rather
 * than asserted: reaching UINT32_MAX by counting is not feasible, and a
 * saturation claim backed only by a hand-built struct would prove nothing
 * about the increment itself. Never called from production code; a no-op when
 * observability is not compiled in.
 */
void nx_mutation_counters_preset_for_test(NxMutationCounterId id, uint32_t v);

_Static_assert(NX_MUT__COUNT == 9, "W6.1 counter set changed — review tokens, "
                                   "boundaries, snapshot mask and tests");
_Static_assert(NX_MUT__COUNT <= 32, "saturated_mask must hold one bit per counter");

#endif /* NX_MUTATION_COUNTERS_H_ */
