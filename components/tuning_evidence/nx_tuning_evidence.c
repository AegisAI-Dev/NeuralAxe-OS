/*
 * NeuralAxe — Gamma 601 tuning-profile validation evidence (Gate W6.5A).
 *
 * Pure and total. No allocation, no global state, no I/O, no task, no clock.
 * See the header for what this module deliberately CANNOT do.
 */

#include <string.h>

#include "nx_tuning_evidence.h"
#include "tuning_policy.h"   /* TUNING_*_TEMP_PLAUSIBLE_MAX_DC */

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_tev_run_status_str(NxTevRunStatus s)
{
    switch (s) {
    case NX_TEV_RUN_INCOMPLETE: return "RUN_INCOMPLETE";
    case NX_TEV_RUN_COMPLETED:  return "RUN_COMPLETED";
    case NX_TEV_RUN_ABORTED:    return "RUN_ABORTED";
    default:                    return "RUN_INCOMPLETE";
    }
}

const char *nx_tev_verdict_str(NxTevVerdict v)
{
    switch (v) {
    case NX_TEV_UNVALIDATED:                return "TEV_UNVALIDATED";
    case NX_TEV_EVIDENCE_INCOMPLETE:        return "TEV_EVIDENCE_INCOMPLETE";
    case NX_TEV_EVIDENCE_REJECTED:          return "TEV_EVIDENCE_REJECTED";
    case NX_TEV_REQUIREMENT_MISSING:        return "TEV_REQUIREMENT_MISSING";
    case NX_TEV_QUALIFIED_FOR_OWNER_REVIEW: return "TEV_QUALIFIED_FOR_OWNER_REVIEW";
    default:                                return "TEV_UNVALIDATED";
    }
}

const char *nx_tev_reason_str(NxTevReason r)
{
    switch (r) {
    case NX_TEV_REASON_NOT_EVALUATED:             return "REASON_NOT_EVALUATED";
    case NX_TEV_REASON_OK:                        return "REASON_OK";
    case NX_TEV_REASON_NULL_INPUT:                return "REASON_NULL_INPUT";
    case NX_TEV_REASON_SCHEMA_UNKNOWN:            return "REASON_SCHEMA_UNKNOWN";
    case NX_TEV_REASON_SCHEMA_FUTURE:             return "REASON_SCHEMA_FUTURE";
    case NX_TEV_REASON_PROFILE_ID_INVALID:        return "REASON_PROFILE_ID_INVALID";
    case NX_TEV_REASON_PROFILE_UNKNOWN:           return "REASON_PROFILE_UNKNOWN";
    case NX_TEV_REASON_PROFILE_REVISION_MISMATCH: return "REASON_PROFILE_REVISION_MISMATCH";
    case NX_TEV_REASON_BOARD_MISMATCH:            return "REASON_BOARD_MISMATCH";
    case NX_TEV_REASON_ASIC_MISMATCH:             return "REASON_ASIC_MISMATCH";
    case NX_TEV_REASON_FIRMWARE_REVISION_MISSING: return "REASON_FIRMWARE_REVISION_MISSING";
    case NX_TEV_REASON_RUN_GENERATION_MISSING:    return "REASON_RUN_GENERATION_MISSING";
    case NX_TEV_REASON_INSTALLATION_UNDECLARED:   return "REASON_INSTALLATION_UNDECLARED";
    case NX_TEV_REASON_COOLING_INSUFFICIENT:      return "REASON_COOLING_INSUFFICIENT";
    case NX_TEV_REASON_PSU_INSUFFICIENT:          return "REASON_PSU_INSUFFICIENT";
    case NX_TEV_REASON_RUN_INCOMPLETE:            return "REASON_RUN_INCOMPLETE";
    case NX_TEV_REASON_RUN_ABORTED:               return "REASON_RUN_ABORTED";
    case NX_TEV_REASON_TELEMETRY_ABSENT:          return "REASON_TELEMETRY_ABSENT";
    case NX_TEV_REASON_ASIC_TELEMETRY_INVALID:    return "REASON_ASIC_TELEMETRY_INVALID";
    case NX_TEV_REASON_VRM_TELEMETRY_INVALID:     return "REASON_VRM_TELEMETRY_INVALID";
    case NX_TEV_REASON_FAN_TELEMETRY_INVALID:     return "REASON_FAN_TELEMETRY_INVALID";
    case NX_TEV_REASON_FAN_CONTROL_FAULT:         return "REASON_FAN_CONTROL_FAULT";
    case NX_TEV_REASON_EMERGENCY_THERMAL:         return "REASON_EMERGENCY_THERMAL";
    case NX_TEV_REASON_RESTART_DURING_RUN:        return "REASON_RESTART_DURING_RUN";
    case NX_TEV_REASON_SAMPLE_ACCOUNTING_INVALID: return "REASON_SAMPLE_ACCOUNTING_INVALID";
    case NX_TEV_REASON_TEMPERATURE_IMPLAUSIBLE:   return "REASON_TEMPERATURE_IMPLAUSIBLE";
    case NX_TEV_REASON_BINDING_MISMATCH:          return "REASON_BINDING_MISMATCH";
    case NX_TEV_REASON_DIGEST_UNAVAILABLE:        return "REASON_DIGEST_UNAVAILABLE";
    case NX_TEV_REASON_PROFILE_PAYLOAD_ABSENT:    return "REASON_PROFILE_PAYLOAD_ABSENT";
    case NX_TEV_REASON_THRESHOLD_UNGOVERNED:      return "REASON_THRESHOLD_UNGOVERNED";
    default:                                      return "REASON_NOT_EVALUATED";
    }
}

bool nx_tev_verdict_is_reviewable(NxTevVerdict v)
{
    return v == NX_TEV_QUALIFIED_FOR_OWNER_REVIEW;
}

/* ------------------------------------------------------------------ */
/* Canonical serialization                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   off;
    bool     ok;
} CanWriter;

static void w_u8(CanWriter *w, uint8_t v)
{
    if (!w->ok || w->off + 1u > w->cap) { w->ok = false; return; }
    w->buf[w->off++] = v;
}

static void w_u16(CanWriter *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v & 0xFFu));
    w_u8(w, (uint8_t)((v >> 8) & 0xFFu));
}

static void w_u32(CanWriter *w, uint32_t v)
{
    w_u16(w, (uint16_t)(v & 0xFFFFu));
    w_u16(w, (uint16_t)((v >> 16) & 0xFFFFu));
}

/* Signed values are encoded as two's-complement u32 so the byte pattern is
 * fixed by this function and not by the host's representation choices. */
static void w_i32(CanWriter *w, int32_t v)
{
    w_u32(w, (uint32_t)v);
}

static void w_bool(CanWriter *w, bool v)
{
    w_u8(w, v ? 1u : 0u);
}

/* Fixed-width, zero-padded. A shorter id can never alias a longer one, and
 * nothing after the terminator is allowed to carry residue. */
static void w_fixed_str(CanWriter *w, const char *s, size_t width)
{
    size_t i;
    size_t n = 0;

    if (s != NULL) {
        while (n < width && s[n] != '\0') { n++; }
    }
    for (i = 0; i < width; i++) {
        w_u8(w, (i < n) ? (uint8_t)s[i] : 0u);
    }
}

bool nx_tuning_evidence_canonical_encode(const NxTuningEvidence *ev,
                                         const TuningProfile *profile,
                                         uint8_t *out, size_t cap,
                                         size_t *out_len)
{
    CanWriter w;

    if (ev == NULL || profile == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    if (ev->schema_version < NX_TEV_SCHEMA_VERSION_MIN ||
        ev->schema_version > NX_TEV_SCHEMA_VERSION) {
        return false;   /* an unknown schema has no canonical form here */
    }

    w.buf = out; w.cap = cap; w.off = 0u; w.ok = true;

    /* --- section 0: schema (always first) --- */
    w_u16(&w, ev->schema_version);

    /* --- section 1: what is being validated --- */
    w_fixed_str(&w, ev->profile_id, NX_TEV_PROFILE_ID_MAX);
    w_u16(&w, ev->profile_revision);

    /* --- section 2: hardware identity --- */
    w_u8(&w, (uint8_t)ev->board);
    w_u8(&w, (uint8_t)ev->asic);

    /* --- section 3: provenance --- */
    w_fixed_str(&w, ev->firmware_revision, NX_TEV_FW_REVISION_MAX);
    w_u32(&w, ev->run_generation);

    /* --- section 4: declared installation --- */
    w_bool(&w, ev->installation.declared);
    w_u8(&w, (uint8_t)ev->installation.cooling_installed);
    w_u8(&w, (uint8_t)ev->installation.psu_installed);

    /* --- section 5: the run --- */
    w_u32(&w, ev->run_duration_s);
    w_u8(&w, ev->run_status);

    /* --- section 6: observed telemetry --- */
    w_bool(&w, ev->telemetry.present);
    w_u32(&w, ev->telemetry.samples_total);
    w_u32(&w, ev->telemetry.samples_valid);
    w_i32(&w, ev->telemetry.asic_temp_max_dc);
    w_i32(&w, ev->telemetry.asic_temp_min_dc);
    w_bool(&w, ev->telemetry.asic_temp_valid_all);
    w_i32(&w, ev->telemetry.vrm_temp_max_dc);
    w_bool(&w, ev->telemetry.vrm_expected);
    w_bool(&w, ev->telemetry.vrm_read_ok_all);
    w_u16(&w, ev->telemetry.fan_rpm_min);
    w_bool(&w, ev->telemetry.fan_expected);
    w_bool(&w, ev->telemetry.fan_control_fault_seen);
    w_bool(&w, ev->telemetry.emergency_thermal_seen);
    w_u32(&w, ev->telemetry.restart_count);

    /*
     * --- section 7: the GOVERNED PROFILE PARAMETERS ---
     *
     * This is what binds evidence to the exact configuration it claims to
     * validate. Change any of these after evidence generation and the
     * canonical bytes change, so the digest no longer matches and the binding
     * is refused. Presentation-only fields (labels) are deliberately excluded;
     * they cannot change what the hardware was asked to do.
     */
    w_fixed_str(&w, profile->id, TUNING_PROFILE_ID_MAX);
    w_u16(&w, profile->revision);
    w_u16(&w, profile->model_version);
    w_u8(&w, (uint8_t)profile->board);
    w_u8(&w, (uint8_t)profile->asic);
    w_u8(&w, (uint8_t)profile->required_cooling);
    w_u8(&w, (uint8_t)profile->required_psu);
    w_u8(&w, profile->rank);
    w_bool(&w, profile->payload_present);
    w_u16(&w, profile->frequency_mhz);
    w_u16(&w, profile->core_voltage_mv);
    w_u8(&w, profile->fan_curve_count);
    {
        size_t i;
        for (i = 0; i < TUNING_FAN_CURVE_MAX_POINTS; i++) {
            w_u8(&w, profile->fan_curve[i].temp_c);
            w_u8(&w, profile->fan_curve[i].fan_percent);
        }
    }
    w_i32(&w, (int32_t)profile->asic_warn_dc);
    w_i32(&w, (int32_t)profile->asic_crit_dc);
    w_i32(&w, (int32_t)profile->vrm_warn_dc);
    w_i32(&w, (int32_t)profile->vrm_crit_dc);
    w_fixed_str(&w, profile->rollback_profile_id, TUNING_PROFILE_ID_MAX);
    w_bool(&w, profile->disabled);

    /*
     * DELIBERATELY EXCLUDED:
     *   ev->binding_digest  — cannot be an input to itself.
     *   ev->owner_approved  — approval is a later, separate authority and must
     *                         not change the identity of the physical evidence.
     *   profile->validation / evidence_fingerprint — the registry's promotion
     *                         state is the OUTPUT of this process, never an
     *                         input to it; including them would make the
     *                         binding change the moment a profile is promoted.
     */

    if (!w.ok) {
        return false;
    }
    *out_len = w.off;
    return true;
}

/* ------------------------------------------------------------------ */
/* Criteria matrix                                                     */
/* ------------------------------------------------------------------ */

/*
 * Every criterion a physical Gamma 601 validation would have to satisfy, with
 * an HONEST statement of whether committed source actually governs it.
 *
 * The SOURCE_DEFINED rows are rules this module genuinely applies. The
 * UNGOVERNED rows are the ones nobody has decided yet: they are counted,
 * surfaced and turned into REQUIREMENT_MISSING rather than defaulted. Inventing
 * a number for any of them here would be the single most dangerous thing this
 * gate could do, because it would look exactly like governance.
 */
static const NxTevCriterion S_CRITERIA[] = {
    /* ---- structural / identity: fully governed by committed source ---- */
    { "evidence schema version", "nx_tuning_evidence.h NX_TEV_SCHEMA_VERSION",
      "schema_version", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "profile identity", "tuning_profile.h registry ids",
      "profile_id", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "profile revision match", "TuningProfile::revision",
      "profile_revision", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "board identity", "TuningBoardClass / tuning_profile_compatible",
      "board", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "asic identity", "TuningAsicClass / tuning_profile_compatible",
      "asic", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "cooling sufficiency", "tuning_profile_compatible (ordered tiers)",
      "installation.cooling_installed", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "psu sufficiency", "tuning_profile_compatible (ordered tiers)",
      "installation.psu_installed", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "binding integrity", "nx_tuning_evidence canonical encoding + digest",
      "binding_digest", NX_TEV_CRIT_SOURCE_DEFINED, false, false },
    { "run completion", "NxTevRunStatus",
      "run_status", NX_TEV_CRIT_SOURCE_DEFINED, true, false },

    /* ---- telemetry validity: governed by the committed snapshot contract ---- */
    { "asic temperature validity", "NxTelemetrySafetySnapshot::asic_temp_valid",
      "telemetry.asic_temp_valid_all", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "vrm read validity", "NxTelemetrySafetySnapshot::vrm_read_ok/vrm_expected",
      "telemetry.vrm_read_ok_all", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "fan tach validity", "NxTelemetrySafetySnapshot::fan_rpm/fan_expected",
      "telemetry.fan_rpm_min", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "fan control fault absent", "NxTelemetrySafetySnapshot::fan_control_fault",
      "telemetry.fan_control_fault_seen", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "emergency thermal absent", "NxTelemetrySafetySnapshot::emergency_thermal_active",
      "telemetry.emergency_thermal_seen", NX_TEV_CRIT_SOURCE_DEFINED, true, false },
    { "temperature plausibility", "tuning_policy.h TUNING_*_TEMP_PLAUSIBLE_MAX_DC",
      "telemetry.asic_temp_max_dc / vrm_temp_max_dc", NX_TEV_CRIT_SOURCE_DEFINED, true, false },

    /* ---- physical acceptance thresholds: NOT governed anywhere ---- */
    { "maximum acceptable asic temperature",
      "NOT DEFINED — power_management_task THROTTLE_TEMP 75.0 is an operational "
      "throttle trip, not a validation acceptance criterion",
      "telemetry.asic_temp_max_dc", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "maximum acceptable vrm temperature",
      "NOT DEFINED — TPS546_THROTTLE_TEMP 105.0 is an operational throttle trip, "
      "not a validation acceptance criterion",
      "telemetry.vrm_temp_max_dc", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "minimum required fan rpm", "NOT DEFINED",
      "telemetry.fan_rpm_min", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "minimum stability duration", "NOT DEFINED",
      "run_duration_s", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "minimum valid sample count", "NOT DEFINED",
      "telemetry.samples_valid", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "allowable telemetry invalidity ratio", "NOT DEFINED",
      "telemetry.samples_valid / samples_total", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "acceptable rejected-share / error rate",
      "NOT DEFINED — and no authoritative coherent source exists: share counts "
      "are absent from NxTelemetrySafetySnapshot",
      "NOT AVAILABLE", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "required hashrate range",
      "NOT DEFINED — and no authoritative coherent source exists: hashrate is "
      "absent from NxTelemetrySafetySnapshot",
      "NOT AVAILABLE", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "psu electrical margin",
      "NOT DEFINED — and no authoritative coherent source exists: power/current/"
      "input voltage are absent from NxTelemetrySafetySnapshot",
      "NOT AVAILABLE", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "ambient temperature envelope",
      "NOT DEFINED — no ambient sensor exists on this board",
      "NOT AVAILABLE", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "restart / fault tolerance during run", "NOT DEFINED",
      "telemetry.restart_count", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "profile tuning payload under test",
      "NOT DEFINED — every production profile is payload_present == false, so "
      "there is no commanded frequency or voltage to validate",
      "profile.frequency_mhz / core_voltage_mv", NX_TEV_CRIT_UNGOVERNED, true, true },
    { "owner promotion approval",
      "NOT DEFINED — no committed owner-governance mechanism exists",
      "owner_approved", NX_TEV_CRIT_UNGOVERNED, false, true },
};

const NxTevCriterion *nx_tuning_evidence_criteria(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(S_CRITERIA) / sizeof(S_CRITERIA[0]);
    }
    return S_CRITERIA;
}

size_t nx_tuning_evidence_ungoverned_count(void)
{
    size_t i;
    size_t n = sizeof(S_CRITERIA) / sizeof(S_CRITERIA[0]);
    size_t c = 0;

    for (i = 0; i < n; i++) {
        if (S_CRITERIA[i].governance == NX_TEV_CRIT_UNGOVERNED) {
            c++;
        }
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Qualification                                                       */
/* ------------------------------------------------------------------ */

static NxTevVerdict finish(NxTevQualification *out, NxTevVerdict v, NxTevReason r)
{
    if (out != NULL) {
        out->verdict = (uint8_t)v;
        out->reason  = (uint8_t)r;
    }
    return v;
}

static bool bounded_len(const char *s, size_t width, size_t *out_len)
{
    size_t n = 0;

    while (n < width && s[n] != '\0') { n++; }
    if (n == width) {
        return false;      /* unterminated within its field */
    }
    *out_len = n;
    return true;
}

NxTevVerdict nx_tuning_evidence_qualify(const NxTuningEvidence *ev,
                                        const TuningProfile *profile,
                                        const NxTevDigestOps *digest_ops,
                                        void *digest_ctx,
                                        NxTevQualification *out)
{
    uint8_t  canonical[NX_TEV_CANONICAL_MAX_BYTES];
    uint8_t  computed[NX_TEV_DIGEST_BYTES];
    size_t   canonical_len = 0;
    size_t   idlen = 0;
    TuningHardwareContext hw;
    TuningCompatibilityResult compat;

    /* Fail closed FIRST: an ignored return value must never leave a passing
     * qualification behind. */
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->verdict = (uint8_t)NX_TEV_UNVALIDATED;
        out->reason  = (uint8_t)NX_TEV_REASON_NOT_EVALUATED;
        out->ungoverned_criteria =
            (uint8_t)nx_tuning_evidence_ungoverned_count();
    }
    if (ev == NULL || profile == NULL) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE, NX_TEV_REASON_NULL_INPUT);
    }

    /* ---- 1. schema ---- */
    if (ev->schema_version == 0u ||
        ev->schema_version < NX_TEV_SCHEMA_VERSION_MIN) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_SCHEMA_UNKNOWN);
    }
    if (ev->schema_version > NX_TEV_SCHEMA_VERSION) {
        /* A future schema may mean anything at all. Refuse; never guess. */
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_SCHEMA_FUTURE);
    }

    /* ---- 2. identity ---- */
    if (!bounded_len(ev->profile_id, NX_TEV_PROFILE_ID_MAX, &idlen) ||
        idlen == 0u) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_PROFILE_ID_INVALID);
    }
    if (strncmp(ev->profile_id, profile->id, NX_TEV_PROFILE_ID_MAX) != 0) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_PROFILE_UNKNOWN);
    }
    if (ev->profile_revision != profile->revision) {
        /* The profile changed after the evidence was produced. */
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_PROFILE_REVISION_MISMATCH);
    }
    if (ev->board != profile->board) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_BOARD_MISMATCH);
    }
    if (ev->asic != profile->asic) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_ASIC_MISMATCH);
    }

    /* ---- 3. provenance ---- */
    if (!bounded_len(ev->firmware_revision, NX_TEV_FW_REVISION_MAX, &idlen) ||
        idlen == 0u) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_FIRMWARE_REVISION_MISSING);
    }
    if (ev->run_generation == 0u) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_RUN_GENERATION_MISSING);
    }

    /* ---- 4. declared installation, judged by the COMMITTED compat rules ---- */
    if (!ev->installation.declared) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_INSTALLATION_UNDECLARED);
    }
    memset(&hw, 0, sizeof(hw));
    hw.board             = ev->board;
    hw.asic              = ev->asic;
    hw.cooling_installed = ev->installation.cooling_installed;
    hw.psu_installed     = ev->installation.psu_installed;
    compat = tuning_profile_compatible(profile, &hw);
    if (compat == TUNING_COMPAT_ERR_COOLING_INSUFFICIENT) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_COOLING_INSUFFICIENT);
    }
    if (compat == TUNING_COMPAT_ERR_PSU_INSUFFICIENT) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_PSU_INSUFFICIENT);
    }
    if (compat != TUNING_COMPAT_OK) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_BOARD_MISMATCH);
    }

    /* ---- 5. run completion ---- */
    if (ev->run_status == (uint8_t)NX_TEV_RUN_ABORTED) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED, NX_TEV_REASON_RUN_ABORTED);
    }
    if (ev->run_status != (uint8_t)NX_TEV_RUN_COMPLETED) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_RUN_INCOMPLETE);
    }

    /* ---- 6. telemetry: validity only. NOT acceptance. ---- */
    if (!ev->telemetry.present) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_TELEMETRY_ABSENT);
    }
    if (ev->telemetry.samples_total == 0u ||
        ev->telemetry.samples_valid > ev->telemetry.samples_total) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_SAMPLE_ACCOUNTING_INVALID);
    }
    if (!ev->telemetry.asic_temp_valid_all) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_ASIC_TELEMETRY_INVALID);
    }
    if (ev->telemetry.vrm_expected && !ev->telemetry.vrm_read_ok_all) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_VRM_TELEMETRY_INVALID);
    }
    if (ev->telemetry.fan_expected && ev->telemetry.fan_rpm_min == 0u) {
        /* 0 rpm on a board that declares a fan controller is the committed
         * "no tach signal" answer, not a legitimate reading. */
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_FAN_TELEMETRY_INVALID);
    }
    if (ev->telemetry.fan_control_fault_seen) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_FAN_CONTROL_FAULT);
    }
    if (ev->telemetry.emergency_thermal_seen) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_EMERGENCY_THERMAL);
    }
    if (ev->telemetry.restart_count > 0u) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_RESTART_DURING_RUN);
    }
    /* Plausibility bands ARE committed (tuning_policy.h). They are not
     * acceptance thresholds — they only reject readings that cannot be real. */
    if (ev->telemetry.asic_temp_max_dc > TUNING_ASIC_TEMP_PLAUSIBLE_MAX_DC ||
        ev->telemetry.asic_temp_min_dc > ev->telemetry.asic_temp_max_dc ||
        (ev->telemetry.vrm_expected &&
         ev->telemetry.vrm_temp_max_dc > TUNING_VRM_TEMP_PLAUSIBLE_MAX_DC)) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_TEMPERATURE_IMPLAUSIBLE);
    }

    /* ---- 7. binding ---- */
    if (digest_ops == NULL || digest_ops->digest == NULL) {
        /* An unverifiable binding is exactly the case that must fail closed. */
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_DIGEST_UNAVAILABLE);
    }
    if (!nx_tuning_evidence_canonical_encode(ev, profile, canonical,
                                             sizeof(canonical), &canonical_len)) {
        return finish(out, NX_TEV_EVIDENCE_INCOMPLETE,
                      NX_TEV_REASON_SCHEMA_UNKNOWN);
    }
    if (out != NULL) {
        out->canonical_len = (uint16_t)canonical_len;
    }
    memset(computed, 0, sizeof(computed));
    if (!digest_ops->digest(digest_ctx, canonical, canonical_len, computed)) {
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_DIGEST_UNAVAILABLE);
    }
    if (memcmp(computed, ev->binding_digest, NX_TEV_DIGEST_BYTES) != 0) {
        /* Either the evidence was tampered with, or the profile changed after
         * the evidence was produced. Both are refusals. */
        return finish(out, NX_TEV_EVIDENCE_REJECTED,
                      NX_TEV_REASON_BINDING_MISMATCH);
    }
    if (out != NULL) {
        out->binding_verified       = true;
        out->owner_approval_present = ev->owner_approved;
    }

    /*
     * ---- 8. THE GOVERNANCE GATE ----
     *
     * Everything above is structure and integrity, and it all passed. What has
     * NOT been checked is whether the physical numbers are ACCEPTABLE, because
     * no committed source defines what acceptable means for this board. There
     * is no maximum validated ASIC temperature, no required stability duration,
     * no minimum sample count and no fan-rpm floor anywhere in the tree.
     *
     * Returning QUALIFIED here would mean silently deciding those thresholds.
     * REQUIREMENT_MISSING is the honest answer, and it is not a pass.
     *
     * A further, independent reason this can never be a pass today: every
     * production profile is payload_present == false, so there is no commanded
     * frequency or voltage for a physical run to have exercised at all.
     */
    if (!profile->payload_present) {
        return finish(out, NX_TEV_REQUIREMENT_MISSING,
                      NX_TEV_REASON_PROFILE_PAYLOAD_ABSENT);
    }
    if (nx_tuning_evidence_ungoverned_count() > 0u) {
        return finish(out, NX_TEV_REQUIREMENT_MISSING,
                      NX_TEV_REASON_THRESHOLD_UNGOVERNED);
    }

    /*
     * Reachable only once every criterion above is governed AND the profile
     * carries a payload. Even then this is REVIEW READINESS, not validation:
     * production promotion remains a separate owner authority that this module
     * cannot express, return or perform.
     */
    return finish(out, NX_TEV_QUALIFIED_FOR_OWNER_REVIEW, NX_TEV_REASON_OK);
}
