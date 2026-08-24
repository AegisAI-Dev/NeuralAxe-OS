#ifndef NX_TUNING_EVIDENCE_H_
#define NX_TUNING_EVIDENCE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tuning_profile.h"

/*
 * NeuralAxe — Gamma 601 tuning-profile VALIDATION EVIDENCE contract
 * (Phase 2W, Gate W6.5A).
 *
 * ==================== WHAT THIS IS, AND IS NOT ====================
 *
 * This module defines the STRUCTURE and INTEGRITY contract for the evidence
 * that would one day justify promoting a Gamma 601 tuning profile out of
 * TUNING_VALIDATION_UNVALIDATED. It can check that a candidate evidence bundle
 * is complete, self-consistent, and cryptographically bound to the exact
 * profile and hardware identity it claims to describe.
 *
 * IT CANNOT MANUFACTURE THE PHYSICAL TRUTH INSIDE THAT EVIDENCE, and it is
 * built so that it cannot pretend to. In particular:
 *
 *   - The verdict enum below contains NO "VALIDATED" value. The best answer
 *     this module can ever return is QUALIFIED_FOR_OWNER_REVIEW. Production
 *     promotion is a separate, later, owner-controlled authority, and there is
 *     deliberately no function here that writes a registry.
 *
 *   - Where a physical acceptance threshold is NOT governed by committed
 *     source, qualification returns REQUIREMENT_MISSING. It does not invent a
 *     number and it does not pass. Missing governance is made VISIBLE rather
 *     than silently defaulted, which is the whole point of the gate.
 *
 *   - UNKNOWN NEVER MEANS PASS. Every absent, unreadable, unexpected or
 *     out-of-range input fails closed.
 *
 * ==================== WHY IT IS OFFLINE-FIRST ====================
 *
 * Nothing here is linked into firmware. It creates no task, opens no socket,
 * touches no NVS and has no runtime entry point. Evidence qualification is a
 * build-time / offline governance activity, so putting it on the device would
 * add permanent runtime cost and — far worse — would create a path by which a
 * running device could reason about its own promotion. It cannot, because the
 * code is not there.
 *
 * ==================== THE DIGEST SEAM ====================
 *
 * The binding digest is INJECTED, never computed here. That keeps this
 * component free of any crypto dependency and keeps firmware free of both.
 * The offline verifier computes SHA-256 over the identical canonical bytes
 * this module produces; a shared golden vector proves the two agree.
 */

/* ------------------------------------------------------------------ */
/* Schema                                                              */
/* ------------------------------------------------------------------ */

#define NX_TEV_SCHEMA_VERSION      1u
#define NX_TEV_SCHEMA_VERSION_MIN  1u

/* Bounded identity strings. Both are machine keys, never presentation. */
#define NX_TEV_PROFILE_ID_MAX      TUNING_PROFILE_ID_MAX   /* 24 */
#define NX_TEV_FW_REVISION_MAX     32u   /* canonical git-describe form */

/*
 * Canonical encoding is fixed-width and fully bounded. The exact size is
 * pinned by a static assertion below and by a golden vector, so a field added,
 * removed or resized cannot slip through unnoticed.
 */
#define NX_TEV_CANONICAL_MAX_BYTES 195u

/* Binding digest width. 32 bytes, because a binding that must resist
 * tampering cannot be 32 BITS — see the type-contract note at the bottom of
 * this header regarding TuningProfile::evidence_fingerprint. */
#define NX_TEV_DIGEST_BYTES        32u

/* ------------------------------------------------------------------ */
/* Run outcome                                                         */
/* ------------------------------------------------------------------ */

/*
 * How the physical run ended. INCOMPLETE is the zero so an unfilled bundle
 * can never read as a finished run.
 */
typedef enum {
    NX_TEV_RUN_INCOMPLETE = 0,
    NX_TEV_RUN_COMPLETED,
    NX_TEV_RUN_ABORTED,
    NX_TEV_RUN__COUNT
} NxTevRunStatus;

const char *nx_tev_run_status_str(NxTevRunStatus s);

/* ------------------------------------------------------------------ */
/* Declared installation facts                                         */
/* ------------------------------------------------------------------ */

/*
 * What the OWNER declared about the physical installation. Nothing here is
 * inferred, measured or guessed by software: the committed W1 model already
 * treats installation as UNSPECIFIED when no authority exists, and W6.5A does
 * not change that. `declared == false` means the owner declared nothing, which
 * is an incomplete bundle rather than a permissive default.
 */
typedef struct {
    bool               declared;
    TuningCoolingClass cooling_installed;
    TuningPsuClass     psu_installed;
} NxTevInstallation;

/* ------------------------------------------------------------------ */
/* Observed telemetry aggregate                                        */
/* ------------------------------------------------------------------ */

/*
 * Aggregated over the run, and deliberately restricted to the facts the
 * committed coherent telemetry snapshot actually publishes
 * (NxTelemetrySafetySnapshot). Power, current, input voltage, hashrate and
 * share counts are NOT part of that snapshot and therefore have NO authoritative
 * coherent source in this firmware; they are absent here rather than invented.
 * Adding them would mean asserting a provenance the tree cannot back.
 *
 * `present == false` means no telemetry was captured at all — an incomplete
 * bundle, never a clean run.
 */
typedef struct {
    bool     present;

    uint32_t samples_total;
    uint32_t samples_valid;        /* asic valid AND vrm acceptable         */

    int32_t  asic_temp_max_dc;     /* deci-C                                */
    int32_t  asic_temp_min_dc;
    bool     asic_temp_valid_all;  /* asic_temp_valid held for every sample */

    int32_t  vrm_temp_max_dc;
    bool     vrm_expected;         /* declared TPS546 presence              */
    bool     vrm_read_ok_all;      /* vrm_read_ok held for every sample     */

    uint16_t fan_rpm_min;          /* 0 is the "no tach signal" answer      */
    bool     fan_expected;         /* declared fan controller presence      */
    bool     fan_control_fault_seen;

    bool     emergency_thermal_seen;
    uint32_t restart_count;        /* restarts observed during the run      */
} NxTevTelemetry;

/* ------------------------------------------------------------------ */
/* The evidence bundle                                                 */
/* ------------------------------------------------------------------ */

/*
 * PRIVACY. Every member is a bounded scalar, a token id, a profile id or a
 * firmware revision string. There is no coordinate, Wi-Fi credential, pool
 * identity, payout address, NTP host, provider secret or owner filesystem
 * path, and no free-form field that a validation decision reads. Hardware
 * facts are CATEGORICAL (cooling/PSU class enums), not owner-specific strings.
 */
typedef struct {
    uint16_t schema_version;

    /* --- what is being validated --- */
    char     profile_id[NX_TEV_PROFILE_ID_MAX];
    uint16_t profile_revision;

    /* --- for what exact hardware identity --- */
    TuningBoardClass board;
    TuningAsicClass  asic;

    /* --- provenance: which firmware produced this run --- */
    char     firmware_revision[NX_TEV_FW_REVISION_MAX];

    /* --- declared physical installation --- */
    NxTevInstallation installation;

    /* --- the run itself --- */
    uint32_t run_duration_s;
    uint32_t run_generation;   /* distinguishes repeated runs; >= 1         */
    uint8_t  run_status;       /* NxTevRunStatus token                      */

    /* --- what was measured --- */
    NxTevTelemetry telemetry;

    /* --- integrity binding over ALL of the above plus the profile --- */
    uint8_t  binding_digest[NX_TEV_DIGEST_BYTES];

    /*
     * OWNER APPROVAL — a separate authority, kept structurally apart from
     * everything automated qualification can conclude. Software never sets
     * this; qualification never requires it to be true in order to reach
     * QUALIFIED_FOR_OWNER_REVIEW, and reaching that verdict never sets it.
     */
    bool     owner_approved;
} NxTuningEvidence;

/* ------------------------------------------------------------------ */
/* Qualification verdict                                               */
/* ------------------------------------------------------------------ */

/*
 * NOTE WHAT IS ABSENT: there is no VALIDATED member, by construction. This
 * enum cannot express production authorization, so no amount of successful
 * automated checking can be mistaken for it or accidentally assigned to a
 * TuningProfile::validation field (the types do not even match).
 *
 * UNVALIDATED is the zero: a zeroed verdict authorizes nothing.
 */
typedef enum {
    NX_TEV_UNVALIDATED = 0,           /* fail-closed zero                   */
    NX_TEV_EVIDENCE_INCOMPLETE,       /* structure/required fields missing  */
    NX_TEV_EVIDENCE_REJECTED,         /* readable, but proves failure       */
    NX_TEV_REQUIREMENT_MISSING,       /* governance gap — NOT a pass        */
    NX_TEV_QUALIFIED_FOR_OWNER_REVIEW,/* the best this module may return    */
    NX_TEV__COUNT
} NxTevVerdict;

const char *nx_tev_verdict_str(NxTevVerdict v);

/* True only for the one verdict that permits presenting evidence to the owner.
 * It is NOT a promotion and NOT an authorization to apply anything. */
bool nx_tev_verdict_is_reviewable(NxTevVerdict v);

/* Bounded reason for the verdict. Zero is the fail-closed "not evaluated". */
typedef enum {
    NX_TEV_REASON_NOT_EVALUATED = 0,
    NX_TEV_REASON_OK,
    NX_TEV_REASON_NULL_INPUT,
    NX_TEV_REASON_SCHEMA_UNKNOWN,
    NX_TEV_REASON_SCHEMA_FUTURE,
    NX_TEV_REASON_PROFILE_ID_INVALID,
    NX_TEV_REASON_PROFILE_UNKNOWN,
    NX_TEV_REASON_PROFILE_REVISION_MISMATCH,
    NX_TEV_REASON_BOARD_MISMATCH,
    NX_TEV_REASON_ASIC_MISMATCH,
    NX_TEV_REASON_FIRMWARE_REVISION_MISSING,
    NX_TEV_REASON_RUN_GENERATION_MISSING,
    NX_TEV_REASON_INSTALLATION_UNDECLARED,
    NX_TEV_REASON_COOLING_INSUFFICIENT,
    NX_TEV_REASON_PSU_INSUFFICIENT,
    NX_TEV_REASON_RUN_INCOMPLETE,
    NX_TEV_REASON_RUN_ABORTED,
    NX_TEV_REASON_TELEMETRY_ABSENT,
    NX_TEV_REASON_ASIC_TELEMETRY_INVALID,
    NX_TEV_REASON_VRM_TELEMETRY_INVALID,
    NX_TEV_REASON_FAN_TELEMETRY_INVALID,
    NX_TEV_REASON_FAN_CONTROL_FAULT,
    NX_TEV_REASON_EMERGENCY_THERMAL,
    NX_TEV_REASON_RESTART_DURING_RUN,
    NX_TEV_REASON_SAMPLE_ACCOUNTING_INVALID,
    NX_TEV_REASON_TEMPERATURE_IMPLAUSIBLE,
    NX_TEV_REASON_BINDING_MISMATCH,
    NX_TEV_REASON_DIGEST_UNAVAILABLE,
    NX_TEV_REASON_PROFILE_PAYLOAD_ABSENT,
    NX_TEV_REASON_THRESHOLD_UNGOVERNED,
    NX_TEV_REASON__COUNT
} NxTevReason;

const char *nx_tev_reason_str(NxTevReason r);

/* Bounded, privacy-safe qualification result. Scalars and tokens only. */
typedef struct {
    uint8_t  verdict;              /* NxTevVerdict token                    */
    uint8_t  reason;               /* NxTevReason token                     */
    bool     binding_verified;     /* the digest matched the canonical bytes */
    bool     owner_approval_present;/* reported, never required by this layer */
    uint16_t canonical_len;        /* bytes of canonical encoding produced   */
    uint8_t  ungoverned_criteria;  /* how many criteria have no threshold    */
} NxTevQualification;

/* ------------------------------------------------------------------ */
/* Canonical serialization                                             */
/* ------------------------------------------------------------------ */

/*
 * THE ONE deterministic encoding. Explicit field order, explicit widths,
 * little-endian throughout, no struct-padding dependence, no float, no
 * pointer, schema version first. Strings are emitted as a fixed-width
 * zero-padded field so a shorter id cannot alias a longer one.
 *
 * It covers the evidence bundle AND the governed profile parameters, which is
 * what makes "changing the profile after evidence generation invalidates the
 * binding" structural rather than procedural.
 *
 * The binding digest itself is EXCLUDED from its own input, for the obvious
 * reason. `owner_approved` is also excluded: approval is a later, separate
 * authority and must not change the identity of the physical evidence.
 *
 * Returns false on NULL, unknown schema, or insufficient capacity.
 */
bool nx_tuning_evidence_canonical_encode(const NxTuningEvidence *ev,
                                         const TuningProfile *profile,
                                         uint8_t *out, size_t cap,
                                         size_t *out_len);

/* ------------------------------------------------------------------ */
/* The digest seam                                                     */
/* ------------------------------------------------------------------ */

/*
 * Compute a NX_TEV_DIGEST_BYTES digest over `len` bytes. Injected so this
 * component links no crypto and firmware links neither. The offline verifier
 * supplies SHA-256; tests may supply a deterministic stand-in when the property
 * under test is the BINDING LOGIC rather than hash strength.
 */
typedef struct {
    bool (*digest)(void *ctx, const uint8_t *data, size_t len,
                   uint8_t out[NX_TEV_DIGEST_BYTES]);
} NxTevDigestOps;

/* ------------------------------------------------------------------ */
/* Qualification                                                       */
/* ------------------------------------------------------------------ */

/*
 * THE automated gate. Pure: it allocates nothing, blocks on nothing, touches
 * no global state, and modifies neither the evidence nor the profile.
 *
 * `profile` must be the production registry entry the evidence names. A NULL
 * digest seam is not a pass — it yields DIGEST_UNAVAILABLE, because an
 * unverifiable binding is exactly the case that must fail closed.
 *
 * The BEST possible return is QUALIFIED_FOR_OWNER_REVIEW. This function has no
 * way to express, return or cause production validation.
 */
NxTevVerdict nx_tuning_evidence_qualify(const NxTuningEvidence *ev,
                                        const TuningProfile *profile,
                                        const NxTevDigestOps *digest_ops,
                                        void *digest_ctx,
                                        NxTevQualification *out);

/* ------------------------------------------------------------------ */
/* Criteria matrix                                                     */
/* ------------------------------------------------------------------ */

/*
 * Whether an acceptance rule for a criterion exists in committed source.
 * This is the mechanism that makes missing governance visible instead of
 * letting it be quietly defaulted.
 */
typedef enum {
    NX_TEV_CRIT_UNGOVERNED = 0,   /* fail-closed zero: no committed rule    */
    NX_TEV_CRIT_SOURCE_DEFINED,   /* a committed rule exists and is applied */
    NX_TEV_CRIT__COUNT
} NxTevCriterionGovernance;

typedef struct {
    const char *name;
    const char *source_authority;  /* where the rule lives, or why it does not */
    const char *evidence_field;
    uint8_t     governance;        /* NxTevCriterionGovernance token         */
    bool        requires_physical_measurement;
    bool        requires_owner_decision;
} NxTevCriterion;

/*
 * The committed criteria matrix. Returns a static table; never NULL.
 * Criteria whose governance is UNGOVERNED are precisely the W6.5B owner
 * decisions, and qualification counts them rather than assuming them.
 */
const NxTevCriterion *nx_tuning_evidence_criteria(size_t *out_count);

/* How many criteria currently have no committed acceptance rule. */
size_t nx_tuning_evidence_ungoverned_count(void);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(NX_TEV_UNVALIDATED == 0,
               "a zeroed verdict must authorize nothing");
_Static_assert(NX_TEV_RUN_INCOMPLETE == 0,
               "a zeroed run status must not read as a finished run");
_Static_assert(NX_TEV_REASON_NOT_EVALUATED == 0,
               "a zeroed reason must not read as OK");
_Static_assert(NX_TEV_CRIT_UNGOVERNED == 0,
               "a zeroed criterion must not read as governed");
_Static_assert(NX_TEV_DIGEST_BYTES == 32u,
               "binding digest width is part of the canonical contract");
/* The canonical length is a contract, not an implementation detail: the
 * offline verifier reproduces these bytes exactly. */
_Static_assert(NX_TEV_CANONICAL_MAX_BYTES ==
                   (2u + NX_TEV_PROFILE_ID_MAX + 2u + 1u + 1u +
                    NX_TEV_FW_REVISION_MAX + 4u + 3u + 4u + 1u) +
                   (1u + 4u + 4u + 4u + 4u + 1u + 4u + 1u + 1u + 2u + 1u + 1u +
                    1u + 4u) +
                   (TUNING_PROFILE_ID_MAX + 2u + 2u + 1u + 1u + 1u + 1u + 1u +
                    1u + 2u + 2u + 1u + (TUNING_FAN_CURVE_MAX_POINTS * 2u) +
                    16u + TUNING_PROFILE_ID_MAX + 1u),
               "canonical encoding size changed — update the golden vector and "
               "the offline verifier together");
_Static_assert(NX_TEV_SCHEMA_VERSION == 1u && NX_TEV_SCHEMA_VERSION_MIN == 1u,
               "evidence schema range changed — review canonical encoding, "
               "golden vectors and the offline verifier together");

/*
 * ==================== TYPE-CONTRACT BLOCKER, RECORDED ====================
 *
 * TuningProfile::evidence_fingerprint is a uint32_t documented as "Opaque
 * stable evidence tag ... NOT a recomputable hash — a stable reference to
 * recorded evidence."
 *
 * That field therefore CANNOT carry the binding this framework produces:
 *   - 32 bits cannot resist tampering (birthday bound ~2^16); and
 *   - its committed semantics are explicitly "reference", not "digest".
 *
 * W6.5A does NOT redefine it, does not compute it, and does not write it.
 * The full-width binding lives inside the evidence bundle, which is a type
 * this gate owns. Reconciling the two — either widening the registry field or
 * formally defining it as an owner-assigned reference into an evidence
 * archive — is an OWNER DECISION and is reported as a blocker for W6.5B
 * rather than resolved by invention here.
 */

#endif /* NX_TUNING_EVIDENCE_H_ */
