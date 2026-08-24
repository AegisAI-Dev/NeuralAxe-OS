/*
 * Gate W6.5A — tuning-profile validation evidence framework tests.
 *
 * WHAT THESE PROVE, AND WHAT THEY DELIBERATELY DO NOT.
 *
 * They prove that a candidate evidence bundle can be checked for structure,
 * identity, integrity and internal consistency, and that every unknown,
 * absent, tampered or ungoverned case FAILS CLOSED.
 *
 * They prove nothing whatsoever about physical hardware. No Gamma 601 is
 * touched, no measurement is taken, and no synthetic bundle here is or may
 * become production evidence. The central assertion of the whole file is that
 * a perfectly-formed bundle still cannot promote a production profile.
 *
 * Everything constructed here is SYNTHETIC and stays inside these tests.
 */

#include <string.h>

#include "unity.h"

#include "nx_tuning_evidence.h"
#include "tuning_profile.h"
#include "tuning_policy.h"

/* ================================================================== */
/* A deterministic stand-in digest                                     */
/* ================================================================== */

/*
 * NOT a cryptographic hash, and never used as one. The property under test in
 * this file is the BINDING LOGIC — that the digest is taken over the canonical
 * bytes, that a change anywhere in those bytes changes it, and that a mismatch
 * is refused. Hash strength is supplied by the offline verifier, which computes
 * SHA-256 over the identical canonical bytes; the shared golden vector proves
 * the two implementations agree on those bytes.
 */
static bool fake_digest(void *ctx, const uint8_t *data, size_t len,
                        uint8_t out[NX_TEV_DIGEST_BYTES])
{
    size_t   i;
    uint32_t h = 2166136261u;   /* FNV-1a, purely as a mixing function */

    (void)ctx;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    memset(out, 0, NX_TEV_DIGEST_BYTES);
    for (i = 0; i < NX_TEV_DIGEST_BYTES; i++) {
        h ^= (uint32_t)i;
        h *= 16777619u;
        out[i] = (uint8_t)(h >> 24);
    }
    return true;
}

static const NxTevDigestOps FAKE_OPS = { .digest = fake_digest };

static bool refusing_digest(void *ctx, const uint8_t *data, size_t len,
                            uint8_t out[NX_TEV_DIGEST_BYTES])
{
    (void)ctx; (void)data; (void)len; (void)out;
    return false;
}

static const NxTevDigestOps REFUSING_OPS = { .digest = refusing_digest };

/* ================================================================== */
/* Fixtures                                                            */
/* ================================================================== */

/*
 * A SYNTHETIC profile that carries a payload. The production registry cannot
 * be used for the "everything structural passes" path, because every
 * production profile is payload-free — which is itself asserted below.
 */
static TuningProfile synthetic_profile(void)
{
    TuningProfile p;

    memset(&p, 0, sizeof(p));
    p.model_version    = TUNING_PROFILE_MODEL_VERSION;
    strcpy(p.id, "synthetic-test");
    p.revision         = 1;
    p.board            = TUNING_BOARD_GAMMA_601;
    p.asic             = TUNING_ASIC_BM1370;
    p.required_cooling = TUNING_COOLING_STOCK;
    p.required_psu     = TUNING_PSU_STANDARD;
    p.validation       = TUNING_VALIDATION_UNVALIDATED;
    p.rank             = 1;
    p.payload_present  = true;
    p.frequency_mhz    = 500;
    p.core_voltage_mv  = 0;      /* Gamma 601 has no validated voltage window */
    return p;
}

/* A structurally complete bundle for `p`, with a correct binding. */
static NxTuningEvidence good_evidence(const TuningProfile *p)
{
    NxTuningEvidence ev;
    uint8_t          canon[NX_TEV_CANONICAL_MAX_BYTES];
    size_t           n = 0;

    memset(&ev, 0, sizeof(ev));
    ev.schema_version = NX_TEV_SCHEMA_VERSION;
    strncpy(ev.profile_id, p->id, NX_TEV_PROFILE_ID_MAX - 1);
    ev.profile_revision = p->revision;
    ev.board            = p->board;
    ev.asic             = p->asic;
    strcpy(ev.firmware_revision, "v2.14.2-92-gfe4cd50");
    ev.installation.declared          = true;
    ev.installation.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    ev.installation.psu_installed     = TUNING_PSU_STANDARD;
    ev.run_duration_s   = 3600;
    ev.run_generation   = 1;
    ev.run_status       = (uint8_t)NX_TEV_RUN_COMPLETED;

    ev.telemetry.present                = true;
    ev.telemetry.samples_total          = 3600;
    ev.telemetry.samples_valid          = 3600;
    ev.telemetry.asic_temp_max_dc       = 620;
    ev.telemetry.asic_temp_min_dc       = 410;
    ev.telemetry.asic_temp_valid_all    = true;
    ev.telemetry.vrm_temp_max_dc        = 700;
    ev.telemetry.vrm_expected           = true;
    ev.telemetry.vrm_read_ok_all        = true;
    ev.telemetry.fan_rpm_min            = 3100;
    ev.telemetry.fan_expected           = true;
    ev.telemetry.fan_control_fault_seen = false;
    ev.telemetry.emergency_thermal_seen = false;
    ev.telemetry.restart_count          = 0;

    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, p, canon,
                                                         sizeof(canon), &n));
    TEST_ASSERT_TRUE(fake_digest(NULL, canon, n, ev.binding_digest));
    return ev;
}

static NxTevVerdict qualify(const NxTuningEvidence *ev, const TuningProfile *p,
                            NxTevQualification *q)
{
    return nx_tuning_evidence_qualify(ev, p, &FAKE_OPS, NULL, q);
}

/* Re-seal a bundle after mutating it, so the binding stays correct and the
 * test isolates the field under test rather than tripping the digest. */
static void reseal(NxTuningEvidence *ev, const TuningProfile *p)
{
    uint8_t canon[NX_TEV_CANONICAL_MAX_BYTES];
    size_t  n = 0;

    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(ev, p, canon,
                                                         sizeof(canon), &n));
    TEST_ASSERT_TRUE(fake_digest(NULL, canon, n, ev->binding_digest));
}

/* ================================================================== */
/* 1-4 — schema handling                                               */
/* ================================================================== */

TEST_CASE("w65a: an empty bundle is INCOMPLETE, never a pass", "[tuning_evidence]")
{
    NxTuningEvidence   ev;
    TuningProfile      p = synthetic_profile();
    NxTevQualification q;

    memset(&ev, 0, sizeof(ev));
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_SCHEMA_UNKNOWN, (NxTevReason)q.reason);
    TEST_ASSERT_FALSE(q.binding_verified);
}

TEST_CASE("w65a: NULL inputs fail closed", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE,
                      nx_tuning_evidence_qualify(NULL, &p, &FAKE_OPS, NULL, &q));
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE,
                      nx_tuning_evidence_qualify(&ev, NULL, &FAKE_OPS, NULL, &q));
    /* And a NULL out-param must not crash. */
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE,
                      nx_tuning_evidence_qualify(NULL, NULL, &FAKE_OPS, NULL, NULL));
}

TEST_CASE("w65a: valid schema with missing required fields is INCOMPLETE",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev;
    NxTevQualification q;

    memset(&ev, 0, sizeof(ev));
    ev.schema_version = NX_TEV_SCHEMA_VERSION;   /* schema fine, rest absent */
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_PROFILE_ID_INVALID, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an unknown (zero) schema is INCOMPLETE", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.schema_version = 0u;
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_SCHEMA_UNKNOWN, (NxTevReason)q.reason);
}

TEST_CASE("w65a: a FUTURE schema is REJECTED, never interpreted",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.schema_version = (uint16_t)(NX_TEV_SCHEMA_VERSION + 1u);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_SCHEMA_FUTURE, (NxTevReason)q.reason);
    /* A future schema also has no canonical form. */
    {
        uint8_t canon[NX_TEV_CANONICAL_MAX_BYTES];
        size_t  n = 0;
        TEST_ASSERT_FALSE(nx_tuning_evidence_canonical_encode(&ev, &p, canon,
                                                              sizeof(canon), &n));
    }
}

/* ================================================================== */
/* 5-8 — identity mismatches                                           */
/* ================================================================== */

TEST_CASE("w65a: wrong board is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.board = TUNING_BOARD_UNKNOWN;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_BOARD_MISMATCH, (NxTevReason)q.reason);
}

TEST_CASE("w65a: wrong ASIC is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.asic = TUNING_ASIC_UNKNOWN;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_ASIC_MISMATCH, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an unknown profile id is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    memset(ev.profile_id, 0, sizeof(ev.profile_id));
    strcpy(ev.profile_id, "no-such-profile");
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_PROFILE_UNKNOWN, (NxTevReason)q.reason);
}

TEST_CASE("w65a: a profile revision mismatch is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.profile_revision = (uint16_t)(p.revision + 1u);
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_PROFILE_REVISION_MISMATCH,
                      (NxTevReason)q.reason);
}

/* ================================================================== */
/* 9-10 — binding and tampering                                        */
/* ================================================================== */

TEST_CASE("w65a: a binding mismatch is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.binding_digest[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_BINDING_MISMATCH, (NxTevReason)q.reason);
    TEST_ASSERT_FALSE(q.binding_verified);
}

TEST_CASE("w65a: tampering with ANY sealed field breaks the binding",
          "[tuning_evidence]")
{
    TuningProfile p = synthetic_profile();
    size_t        i;

    /*
     * Field-by-field tamper sweep. Each mutation is applied WITHOUT resealing,
     * so a field that the canonical encoding forgot to cover would silently
     * still verify — and this test would catch that omission.
     */
    for (i = 0; i < 9u; i++) {
        NxTuningEvidence   ev = good_evidence(&p);
        NxTevQualification q;

        switch (i) {
        case 0: ev.run_duration_s++;                       break;
        case 1: ev.run_generation++;                       break;
        case 2: ev.telemetry.samples_total++;              break;
        case 3: ev.telemetry.samples_valid--;              break;
        case 4: ev.telemetry.asic_temp_max_dc += 1;        break;
        case 5: ev.telemetry.vrm_temp_max_dc += 1;         break;
        case 6: ev.telemetry.fan_rpm_min += 1;             break;
        case 7: ev.installation.cooling_installed = TUNING_COOLING_STOCK; break;
        case 8: ev.firmware_revision[0] = 'X';             break;
        default: break;
        }
        TEST_ASSERT_EQUAL_MESSAGE(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q),
                                  "a tampered field still qualified");
        TEST_ASSERT_EQUAL(NX_TEV_REASON_BINDING_MISMATCH, (NxTevReason)q.reason);
    }
}

TEST_CASE("w65a: changing PROFILE parameters invalidates existing evidence",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    /*
     * THE PROPERTY THE WHOLE BINDING EXISTS FOR. The evidence was sealed
     * against a profile commanding 500 MHz. Retuning the profile must not
     * silently inherit the old physical evidence.
     */
    p.frequency_mhz = 550;
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_BINDING_MISMATCH, (NxTevReason)q.reason);
}

/* ================================================================== */
/* 11-16 — telemetry validity                                          */
/* ================================================================== */

TEST_CASE("w65a: absent telemetry is INCOMPLETE", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    memset(&ev.telemetry, 0, sizeof(ev.telemetry));
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_TELEMETRY_ABSENT, (NxTevReason)q.reason);
}

TEST_CASE("w65a: invalid ASIC telemetry is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.asic_temp_valid_all = false;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_ASIC_TELEMETRY_INVALID, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an invalid VRM read is REJECTED when a VRM is expected",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.vrm_read_ok_all = false;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_VRM_TELEMETRY_INVALID, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an invalid fan tach is REJECTED when a fan is expected",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.fan_rpm_min = 0u;   /* the committed "no tach signal" answer */
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_FAN_TELEMETRY_INVALID, (NxTevReason)q.reason);
}

TEST_CASE("w65a: a fan-control fault is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.fan_control_fault_seen = true;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_FAN_CONTROL_FAULT, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an emergency thermal event is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.emergency_thermal_seen = true;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_EMERGENCY_THERMAL, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an implausible temperature is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.asic_temp_max_dc = TUNING_ASIC_TEMP_PLAUSIBLE_MAX_DC + 1;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_TEMPERATURE_IMPLAUSIBLE, (NxTevReason)q.reason);
}

TEST_CASE("w65a: incoherent sample accounting is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.samples_valid = ev.telemetry.samples_total + 1u;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_SAMPLE_ACCOUNTING_INVALID,
                      (NxTevReason)q.reason);
}

/* ================================================================== */
/* 17-20 — installation, completion, restart                           */
/* ================================================================== */

TEST_CASE("w65a: incompatible cooling is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev;
    NxTevQualification q;

    p.required_cooling = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    ev = good_evidence(&p);
    ev.installation.cooling_installed = TUNING_COOLING_STOCK;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_COOLING_INSUFFICIENT, (NxTevReason)q.reason);
}

TEST_CASE("w65a: incompatible PSU is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev;
    NxTevQualification q;

    p.required_psu = TUNING_PSU_STANDARD;
    ev = good_evidence(&p);
    ev.installation.psu_installed = TUNING_PSU_UNSPECIFIED;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_PSU_INSUFFICIENT, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an undeclared installation is INCOMPLETE — never assumed",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.installation.declared = false;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_INSTALLATION_UNDECLARED, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an incomplete or aborted run never qualifies",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.run_status = (uint8_t)NX_TEV_RUN_INCOMPLETE;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_RUN_INCOMPLETE, (NxTevReason)q.reason);

    ev.run_status = (uint8_t)NX_TEV_RUN_ABORTED;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_RUN_ABORTED, (NxTevReason)q.reason);
}

TEST_CASE("w65a: a restart during the run is REJECTED", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    ev.telemetry.restart_count = 1u;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_RESTART_DURING_RUN, (NxTevReason)q.reason);
}

TEST_CASE("w65a: missing provenance is INCOMPLETE", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    memset(ev.firmware_revision, 0, sizeof(ev.firmware_revision));
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_FIRMWARE_REVISION_MISSING,
                      (NxTevReason)q.reason);

    ev = good_evidence(&p);
    ev.run_generation = 0u;
    reseal(&ev, &p);
    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_INCOMPLETE, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_RUN_GENERATION_MISSING, (NxTevReason)q.reason);
}

TEST_CASE("w65a: an unavailable digest is REJECTED, never trusted",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED,
                      nx_tuning_evidence_qualify(&ev, &p, NULL, NULL, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_DIGEST_UNAVAILABLE, (NxTevReason)q.reason);

    TEST_ASSERT_EQUAL(NX_TEV_EVIDENCE_REJECTED,
                      nx_tuning_evidence_qualify(&ev, &p, &REFUSING_OPS, NULL, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_DIGEST_UNAVAILABLE, (NxTevReason)q.reason);
}

/* ================================================================== */
/* 21-24 — THE GOVERNANCE BOUNDARY                                     */
/* ================================================================== */

TEST_CASE("w65a: structurally perfect evidence still returns "
          "REQUIREMENT_MISSING, not a pass", "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    /*
     * THE CENTRAL ASSERTION OF THIS GATE.
     *
     * Every structural, identity, integrity and telemetry-validity check
     * passes. The bundle is complete, correctly bound and internally
     * consistent. And it STILL does not qualify — because no committed source
     * defines what an acceptable ASIC temperature, stability duration, sample
     * count or fan floor actually is.
     *
     * Returning QUALIFIED here would mean this framework had quietly decided
     * those thresholds itself. It refuses to.
     */
    TEST_ASSERT_EQUAL(NX_TEV_REQUIREMENT_MISSING, qualify(&ev, &p, &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_THRESHOLD_UNGOVERNED, (NxTevReason)q.reason);

    /* The binding DID verify — the refusal is governance, not integrity. */
    TEST_ASSERT_TRUE(q.binding_verified);
    TEST_ASSERT_TRUE(q.ungoverned_criteria > 0u);
    TEST_ASSERT_FALSE(nx_tev_verdict_is_reviewable((NxTevVerdict)q.verdict));
}

TEST_CASE("w65a: owner approval is reported but never required or granted",
          "[tuning_evidence]")
{
    TuningProfile      p = synthetic_profile();
    NxTuningEvidence   ev = good_evidence(&p);
    NxTevQualification q;

    /* Absent approval does not change the automated verdict... */
    TEST_ASSERT_EQUAL(NX_TEV_REQUIREMENT_MISSING, qualify(&ev, &p, &q));
    TEST_ASSERT_FALSE(q.owner_approval_present);

    /* ...and neither does PRESENT approval. Approval is a separate authority;
     * it cannot substitute for missing governance, and qualification cannot
     * consume it to reach a better verdict. */
    ev.owner_approved = true;
    TEST_ASSERT_EQUAL(NX_TEV_REQUIREMENT_MISSING, qualify(&ev, &p, &q));
    TEST_ASSERT_TRUE(q.owner_approval_present);
}

TEST_CASE("w65a: the verdict type CANNOT express production validation",
          "[tuning_evidence]")
{
    size_t i;

    /*
     * QUALIFIED_FOR_OWNER_REVIEW != VALIDATED, structurally. The two live in
     * different enums with different underlying types, so no assignment,
     * cast-free comparison or accidental collapse is possible.
     */
    for (i = 0; i < (size_t)NX_TEV__COUNT; i++) {
        const char *s = nx_tev_verdict_str((NxTevVerdict)i);
        TEST_ASSERT_NOT_NULL(s);
        /* No verdict token may claim validation. */
        TEST_ASSERT_NULL(strstr(s, "TEV_VALIDATED"));
    }
    TEST_ASSERT_TRUE(nx_tev_verdict_is_reviewable(NX_TEV_QUALIFIED_FOR_OWNER_REVIEW));
    TEST_ASSERT_FALSE(nx_tev_verdict_is_reviewable(NX_TEV_UNVALIDATED));
    TEST_ASSERT_FALSE(nx_tev_verdict_is_reviewable(NX_TEV_REQUIREMENT_MISSING));
    TEST_ASSERT_FALSE(nx_tev_verdict_is_reviewable(NX_TEV_EVIDENCE_REJECTED));
    TEST_ASSERT_FALSE(nx_tev_verdict_is_reviewable(NX_TEV_EVIDENCE_INCOMPLETE));
}

TEST_CASE("w65a: a production profile has no payload to validate",
          "[tuning_evidence]")
{
    const TuningProfile *reg;
    size_t               n = 0;
    NxTuningEvidence     ev;
    NxTevQualification   q;
    uint8_t              canon[NX_TEV_CANONICAL_MAX_BYTES];
    size_t               len = 0;

    /*
     * An INDEPENDENT reason production evidence cannot qualify today, on top of
     * the ungoverned thresholds: every production profile is payload-free, so
     * there is no commanded frequency or voltage a physical run could have
     * exercised. There is literally nothing to validate yet.
     */
    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_NOT_NULL(reg);
    TEST_ASSERT_EQUAL_UINT32(TUNING_REGISTRY_GAMMA601_COUNT, (uint32_t)n);

    memset(&ev, 0, sizeof(ev));
    ev.schema_version = NX_TEV_SCHEMA_VERSION;
    strncpy(ev.profile_id, reg[0].id, NX_TEV_PROFILE_ID_MAX - 1);
    ev.profile_revision = reg[0].revision;
    ev.board = reg[0].board;
    ev.asic  = reg[0].asic;
    strcpy(ev.firmware_revision, "v0.0.0-0-gsynth");
    ev.installation.declared          = true;
    ev.installation.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    ev.installation.psu_installed     = TUNING_PSU_STANDARD;
    ev.run_duration_s = 3600; ev.run_generation = 1;
    ev.run_status = (uint8_t)NX_TEV_RUN_COMPLETED;
    ev.telemetry.present = true;
    ev.telemetry.samples_total = 10; ev.telemetry.samples_valid = 10;
    ev.telemetry.asic_temp_max_dc = 600; ev.telemetry.asic_temp_min_dc = 400;
    ev.telemetry.asic_temp_valid_all = true;
    ev.telemetry.vrm_temp_max_dc = 700; ev.telemetry.vrm_expected = true;
    ev.telemetry.vrm_read_ok_all = true;
    ev.telemetry.fan_rpm_min = 3000; ev.telemetry.fan_expected = true;
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &reg[0], canon,
                                                         sizeof(canon), &len));
    TEST_ASSERT_TRUE(fake_digest(NULL, canon, len, ev.binding_digest));

    TEST_ASSERT_EQUAL(NX_TEV_REQUIREMENT_MISSING, qualify(&ev, &reg[0], &q));
    TEST_ASSERT_EQUAL(NX_TEV_REASON_PROFILE_PAYLOAD_ABSENT, (NxTevReason)q.reason);
}

/* ================================================================== */
/* 25-29 — the production registry is untouched                        */
/* ================================================================== */

TEST_CASE("w65a: EVERY production Gamma 601 profile is still UNVALIDATED",
          "[tuning_evidence]")
{
    const TuningProfile *reg;
    size_t               n = 0;
    size_t               i;

    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_EQUAL_UINT32(TUNING_REGISTRY_GAMMA601_COUNT, (uint32_t)n);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(TUNING_VALIDATION_UNVALIDATED,
                                  reg[i].validation,
                                  "a production profile is no longer UNVALIDATED");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, reg[i].evidence_fingerprint,
                                         "a production profile gained evidence");
        TEST_ASSERT_FALSE_MESSAGE(reg[i].payload_present,
                                  "a production profile gained a payload");
        TEST_ASSERT_FALSE(reg[i].disabled);
    }
}

TEST_CASE("w65a: no production profile is auto-eligible", "[tuning_evidence]")
{
    const TuningProfile  *reg;
    size_t                n = 0;
    size_t                i;
    TuningHardwareContext hw;

    /* Even given a fully capable declared installation. */
    memset(&hw, 0, sizeof(hw));
    hw.board             = TUNING_BOARD_GAMMA_601;
    hw.asic              = TUNING_ASIC_BM1370;
    hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    hw.psu_installed     = TUNING_PSU_STANDARD;

    reg = tuning_registry_gamma601(&n);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                                  tuning_profile_auto_eligible(&reg[i], &hw),
                                  "a production profile became auto-eligible");
    }
}

TEST_CASE("w65a: synthetic evidence cannot reach the production registry",
          "[tuning_evidence]")
{
    TuningProfile        p = synthetic_profile();
    NxTuningEvidence     ev = good_evidence(&p);
    const TuningProfile *reg;
    size_t               n = 0;
    size_t               i;
    size_t               j;

    /*
     * The synthetic bundle above is as good as a bundle can be. Prove it left
     * no trace anywhere in production state: no fingerprint was copied, no
     * validation state moved, and the framework exposes no function that could
     * have done either.
     */
    reg = tuning_registry_gamma601(&n);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, reg[i].validation);
        TEST_ASSERT_EQUAL_UINT32(0u, reg[i].evidence_fingerprint);
        /* The synthetic digest must not appear in the registry's tag. */
        for (j = 0; j < NX_TEV_DIGEST_BYTES - 3u; j++) {
            uint32_t word = (uint32_t)ev.binding_digest[j] |
                            ((uint32_t)ev.binding_digest[j + 1] << 8) |
                            ((uint32_t)ev.binding_digest[j + 2] << 16) |
                            ((uint32_t)ev.binding_digest[j + 3] << 24);
            if (word != 0u) {
                TEST_ASSERT_NOT_EQUAL(word, reg[i].evidence_fingerprint);
            }
        }
    }
    /* And the synthetic id is not a registry id. */
    TEST_ASSERT_NULL(tuning_registry_find(reg, n, "synthetic-test"));
}

/* ================================================================== */
/* Canonical encoding — determinism and the golden vector              */
/* ================================================================== */

TEST_CASE("w65a: canonical encoding is deterministic and padding-free",
          "[tuning_evidence]")
{
    TuningProfile p = synthetic_profile();
    NxTuningEvidence ev = good_evidence(&p);
    uint8_t a[NX_TEV_CANONICAL_MAX_BYTES];
    uint8_t b[NX_TEV_CANONICAL_MAX_BYTES];
    size_t  na = 0, nb = 0;

    memset(a, 0xAA, sizeof(a));
    memset(b, 0x55, sizeof(b));
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, a, sizeof(a), &na));
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, b, sizeof(b), &nb));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)na, (uint32_t)nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);

    /* A short buffer must fail rather than truncate. */
    TEST_ASSERT_FALSE(nx_tuning_evidence_canonical_encode(&ev, &p, a, 4u, &na));
}

TEST_CASE("w65a: GOLDEN VECTOR — canonical bytes are pinned",
          "[tuning_evidence]")
{
    /*
     * A fully-specified fixture whose canonical encoding is pinned byte for
     * byte. The OFFLINE verifier asserts the identical vector, which is what
     * makes the two independent implementations provably agree — and what would
     * catch a field reorder, a width change or an endianness slip in either.
     */
    TuningProfile    p;
    NxTuningEvidence ev;
    uint8_t          buf[NX_TEV_CANONICAL_MAX_BYTES];
    size_t           n = 0;
    size_t           i;
    uint32_t         sum = 0;

    memset(&p, 0, sizeof(p));
    p.model_version = TUNING_PROFILE_MODEL_VERSION;
    strcpy(p.id, "golden");
    p.revision = 1;
    p.board = TUNING_BOARD_GAMMA_601;
    p.asic  = TUNING_ASIC_BM1370;
    p.required_cooling = TUNING_COOLING_STOCK;
    p.required_psu     = TUNING_PSU_STANDARD;
    p.rank = 1;
    p.payload_present = true;
    p.frequency_mhz = 500;

    memset(&ev, 0, sizeof(ev));
    ev.schema_version = 1u;
    strcpy(ev.profile_id, "golden");
    ev.profile_revision = 1;
    ev.board = TUNING_BOARD_GAMMA_601;
    ev.asic  = TUNING_ASIC_BM1370;
    strcpy(ev.firmware_revision, "v1.0.0-0-gabcdef1");
    ev.installation.declared = true;
    ev.installation.cooling_installed = TUNING_COOLING_STOCK;
    ev.installation.psu_installed     = TUNING_PSU_STANDARD;
    ev.run_duration_s = 60;
    ev.run_generation = 1;
    ev.run_status = (uint8_t)NX_TEV_RUN_COMPLETED;
    ev.telemetry.present = true;
    ev.telemetry.samples_total = 60;
    ev.telemetry.samples_valid = 60;
    ev.telemetry.asic_temp_max_dc = 600;
    ev.telemetry.asic_temp_min_dc = 400;
    ev.telemetry.asic_temp_valid_all = true;
    ev.telemetry.vrm_temp_max_dc = 700;
    ev.telemetry.vrm_expected = true;
    ev.telemetry.vrm_read_ok_all = true;
    ev.telemetry.fan_rpm_min = 3000;
    ev.telemetry.fan_expected = true;

    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, buf,
                                                         sizeof(buf), &n));

    /* Pinned length. A field added, removed or resized changes this. */
    TEST_ASSERT_EQUAL_UINT32(195u, (uint32_t)n);

    /* Pinned leading bytes: schema, then the fixed-width profile id. */
    TEST_ASSERT_EQUAL_UINT8(0x01u, buf[0]);   /* schema lo */
    TEST_ASSERT_EQUAL_UINT8(0x00u, buf[1]);   /* schema hi */
    TEST_ASSERT_EQUAL_UINT8('g',   buf[2]);
    TEST_ASSERT_EQUAL_UINT8('o',   buf[3]);
    TEST_ASSERT_EQUAL_UINT8('l',   buf[4]);
    TEST_ASSERT_EQUAL_UINT8('d',   buf[5]);
    TEST_ASSERT_EQUAL_UINT8('e',   buf[6]);
    TEST_ASSERT_EQUAL_UINT8('n',   buf[7]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, buf[8]);   /* zero padding, not residue */

    /*
     * Pinned checksum over the whole vector. The offline verifier
     * (tools/validation/nx_evidence_verify.py) computes the identical 195
     * canonical bytes for this exact fixture and its own test asserts the same
     * number, so the two independent implementations are provably byte-for-byte
     * in agreement. Its SHA-256 over these bytes is
     * 66566931d2aed8240c0ef79ca45faefbae9b79b8f0d614882bf974d7b6b73e87.
     */
    for (i = 0; i < n; i++) { sum = (sum * 31u) + buf[i]; }
    TEST_ASSERT_EQUAL_UINT32(2809923663u, sum);
}

TEST_CASE("w65a: schema, board, ASIC and profile changes all move the bytes",
          "[tuning_evidence]")
{
    TuningProfile p = synthetic_profile();
    NxTuningEvidence base = good_evidence(&p);
    uint8_t a[NX_TEV_CANONICAL_MAX_BYTES], b[NX_TEV_CANONICAL_MAX_BYTES];
    size_t  na = 0, nb = 0;

    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&base, &p, a, sizeof(a), &na));

    {   /* board */
        NxTuningEvidence v = base;
        v.board = TUNING_BOARD_UNKNOWN;
        TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&v, &p, b, sizeof(b), &nb));
        TEST_ASSERT_TRUE(na != nb || memcmp(a, b, na) != 0);
    }
    {   /* asic */
        NxTuningEvidence v = base;
        v.asic = TUNING_ASIC_UNKNOWN;
        TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&v, &p, b, sizeof(b), &nb));
        TEST_ASSERT_TRUE(na != nb || memcmp(a, b, na) != 0);
    }
    {   /* profile parameter */
        TuningProfile q = p;
        q.rank = (uint8_t)(p.rank + 1u);
        TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&base, &q, b, sizeof(b), &nb));
        TEST_ASSERT_TRUE(na != nb || memcmp(a, b, na) != 0);
    }
    {   /* required cooling */
        TuningProfile q = p;
        q.required_cooling = TUNING_COOLING_SUPERSINK_DUAL_FAN;
        TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&base, &q, b, sizeof(b), &nb));
        TEST_ASSERT_TRUE(na != nb || memcmp(a, b, na) != 0);
    }
}

TEST_CASE("w65a: promotion state is NOT part of the binding",
          "[tuning_evidence]")
{
    TuningProfile p = synthetic_profile();
    NxTuningEvidence ev = good_evidence(&p);
    uint8_t a[NX_TEV_CANONICAL_MAX_BYTES], b[NX_TEV_CANONICAL_MAX_BYTES];
    size_t  na = 0, nb = 0;

    /*
     * The registry's promotion state is the OUTPUT of this process, never an
     * input to it. If it were sealed in, promoting a profile would instantly
     * invalidate the very evidence that justified the promotion.
     */
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, a, sizeof(a), &na));
    p.validation           = TUNING_VALIDATION_VALIDATED;
    p.evidence_fingerprint = 0xDEADBEEFu;
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, b, sizeof(b), &nb));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)na, (uint32_t)nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);

    /* Owner approval likewise does not change evidence identity. */
    ev.owner_approved = true;
    TEST_ASSERT_TRUE(nx_tuning_evidence_canonical_encode(&ev, &p, b, sizeof(b), &nb));
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
}

/* ================================================================== */
/* Criteria matrix                                                     */
/* ================================================================== */

TEST_CASE("w65a: the criteria matrix names its ungoverned thresholds",
          "[tuning_evidence]")
{
    const NxTevCriterion *c;
    size_t                n = 0;
    size_t                i;
    size_t                ungoverned = 0;

    c = nx_tuning_evidence_criteria(&n);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(n > 0u);

    for (i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(c[i].name);
        TEST_ASSERT_NOT_NULL(c[i].source_authority);
        TEST_ASSERT_NOT_NULL(c[i].evidence_field);
        TEST_ASSERT_TRUE(c[i].governance < (uint8_t)NX_TEV_CRIT__COUNT);
        if (c[i].governance == (uint8_t)NX_TEV_CRIT_UNGOVERNED) {
            ungoverned++;
            /* An ungoverned criterion MUST be flagged as an owner decision —
             * otherwise it would silently never get decided. */
            TEST_ASSERT_TRUE_MESSAGE(c[i].requires_owner_decision,
                                     "an ungoverned criterion is not marked as "
                                     "requiring an owner decision");
        }
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ungoverned,
                             (uint32_t)nx_tuning_evidence_ungoverned_count());
    TEST_ASSERT_TRUE_MESSAGE(ungoverned > 0u,
                             "the matrix claims full governance — verify before "
                             "trusting a QUALIFIED verdict");
}

TEST_CASE("w65a: every token is distinct and non-NULL", "[tuning_evidence]")
{
    size_t i, j;

    for (i = 0; i < (size_t)NX_TEV__COUNT; i++) {
        const char *a = nx_tev_verdict_str((NxTevVerdict)i);
        TEST_ASSERT_NOT_NULL(a);
        for (j = i + 1; j < (size_t)NX_TEV__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, nx_tev_verdict_str((NxTevVerdict)j)) != 0);
        }
    }
    for (i = 0; i < (size_t)NX_TEV_REASON__COUNT; i++) {
        const char *a = nx_tev_reason_str((NxTevReason)i);
        TEST_ASSERT_NOT_NULL(a);
        for (j = i + 1; j < (size_t)NX_TEV_REASON__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, nx_tev_reason_str((NxTevReason)j)) != 0);
        }
    }
    TEST_ASSERT_NOT_NULL(nx_tev_verdict_str((NxTevVerdict)99));
    TEST_ASSERT_NOT_NULL(nx_tev_reason_str((NxTevReason)99));
    TEST_ASSERT_NOT_NULL(nx_tev_run_status_str((NxTevRunStatus)99));
}

TEST_CASE("w65a: the evidence bundle carries no private-value channel",
          "[tuning_evidence]")
{
    /*
     * Structural privacy. The only strings in the bundle are a profile id and
     * a firmware revision, both bounded machine keys. Hardware facts are
     * categorical enums, not owner-specific free-form text. There is no field
     * a coordinate, credential, pool identity, payout address or NTP host
     * could occupy.
     */
    NxTuningEvidence ev;

    TEST_ASSERT_EQUAL_UINT32(NX_TEV_PROFILE_ID_MAX, (uint32_t)sizeof(ev.profile_id));
    TEST_ASSERT_EQUAL_UINT32(NX_TEV_FW_REVISION_MAX,
                             (uint32_t)sizeof(ev.firmware_revision));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(ev.installation.declared));
    TEST_ASSERT_EQUAL_UINT32(NX_TEV_DIGEST_BYTES,
                             (uint32_t)sizeof(ev.binding_digest));
    /* Small enough to state exactly; nothing unbounded can be hiding. */
    TEST_ASSERT_TRUE(sizeof(ev) < 256u);
}
