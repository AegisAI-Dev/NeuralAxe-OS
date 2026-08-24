#!/usr/bin/env python3
"""NeuralAxe — offline Gamma 601 tuning-profile evidence verifier (Gate W6.5A).

WHAT THIS DOES
    Parses a candidate evidence bundle, reproduces the SAME canonical byte
    encoding the firmware-side component produces, computes the SHA-256 binding
    digest over those bytes, and emits a deterministic qualification result plus
    a review summary an owner can actually read.

WHAT THIS DELIBERATELY CANNOT DO
    - It cannot promote anything. There is no code path here that edits a
      registry, a profile, a source file or a Git object. The best verdict it
      can emit is QUALIFIED_FOR_OWNER_REVIEW.
    - It cannot invent an acceptance threshold. Where committed source does not
      govern a physical limit, the verdict is REQUIREMENT_MISSING and the
      missing criteria are listed by name.
    - It touches no network, no hardware, no NVS and no owner-private value.

The canonical encoding here is the normative twin of
components/tuning_evidence/nx_tuning_evidence.c. A shared golden vector proves
the two implementations agree byte for byte; if you change one, the golden
vector will fail until you change the other.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Contract constants — mirror nx_tuning_evidence.h exactly
# --------------------------------------------------------------------------

SCHEMA_VERSION = 1
SCHEMA_VERSION_MIN = 1
PROFILE_ID_MAX = 24
FW_REVISION_MAX = 32
FAN_CURVE_MAX_POINTS = 4
DIGEST_BYTES = 32
CANONICAL_BYTES = 195

RUN_INCOMPLETE, RUN_COMPLETED, RUN_ABORTED = 0, 1, 2

# Verdicts. Note the absence of any VALIDATED member — by construction.
UNVALIDATED = "TEV_UNVALIDATED"
EVIDENCE_INCOMPLETE = "TEV_EVIDENCE_INCOMPLETE"
EVIDENCE_REJECTED = "TEV_EVIDENCE_REJECTED"
REQUIREMENT_MISSING = "TEV_REQUIREMENT_MISSING"
QUALIFIED_FOR_OWNER_REVIEW = "TEV_QUALIFIED_FOR_OWNER_REVIEW"

# Criteria with no committed acceptance rule. Mirrors the C criteria matrix.
# These are OWNER DECISIONS, not defaults waiting to be filled in by a tool.
UNGOVERNED_CRITERIA = (
    "maximum acceptable asic temperature",
    "maximum acceptable vrm temperature",
    "minimum required fan rpm",
    "minimum stability duration",
    "minimum valid sample count",
    "allowable telemetry invalidity ratio",
    "acceptable rejected-share / error rate",
    "required hashrate range",
    "psu electrical margin",
    "ambient temperature envelope",
    "restart / fault tolerance during run",
    "profile tuning payload under test",
    "owner promotion approval",
)


class EvidenceError(Exception):
    """A failure that never carries an owner-private value in its message."""


# --------------------------------------------------------------------------
# Canonical serialization — the normative twin of the C encoder
# --------------------------------------------------------------------------

def _fixed_str(value: str, width: int) -> bytes:
    """Fixed-width, zero-padded. A shorter id can never alias a longer one."""
    raw = value.encode("ascii", errors="strict")
    if len(raw) >= width:
        raise EvidenceError(f"string exceeds its {width}-byte field")
    return raw + b"\x00" * (width - len(raw))


def canonical_encode(ev: dict, profile: dict) -> bytes:
    """Produce the ONE canonical byte string. Little-endian, fixed widths.

    Field order is identical to nx_tuning_evidence.c. The binding digest and
    the owner-approval flag are excluded: a digest cannot be an input to
    itself, and approval is a later authority that must not change the identity
    of the physical evidence. The registry's promotion state is excluded for
    the same reason — it is the OUTPUT of this process, never an input.
    """
    schema = int(ev["schema_version"])
    if not (SCHEMA_VERSION_MIN <= schema <= SCHEMA_VERSION):
        raise EvidenceError("unknown evidence schema — refusing to encode")

    tel = ev["telemetry"]
    inst = ev["installation"]
    out = bytearray()

    # --- 0: schema
    out += struct.pack("<H", schema)
    # --- 1: what is being validated
    out += _fixed_str(ev["profile_id"], PROFILE_ID_MAX)
    out += struct.pack("<H", int(ev["profile_revision"]))
    # --- 2: hardware identity
    out += struct.pack("<BB", int(ev["board"]), int(ev["asic"]))
    # --- 3: provenance
    out += _fixed_str(ev["firmware_revision"], FW_REVISION_MAX)
    out += struct.pack("<I", int(ev["run_generation"]))
    # --- 4: declared installation
    out += struct.pack("<BBB", 1 if inst["declared"] else 0,
                       int(inst["cooling_installed"]), int(inst["psu_installed"]))
    # --- 5: the run
    out += struct.pack("<I", int(ev["run_duration_s"]))
    out += struct.pack("<B", int(ev["run_status"]))
    # --- 6: observed telemetry
    out += struct.pack("<B", 1 if tel["present"] else 0)
    out += struct.pack("<I", int(tel["samples_total"]))
    out += struct.pack("<I", int(tel["samples_valid"]))
    out += struct.pack("<i", int(tel["asic_temp_max_dc"]))
    out += struct.pack("<i", int(tel["asic_temp_min_dc"]))
    out += struct.pack("<B", 1 if tel["asic_temp_valid_all"] else 0)
    out += struct.pack("<i", int(tel["vrm_temp_max_dc"]))
    out += struct.pack("<B", 1 if tel["vrm_expected"] else 0)
    out += struct.pack("<B", 1 if tel["vrm_read_ok_all"] else 0)
    out += struct.pack("<H", int(tel["fan_rpm_min"]))
    out += struct.pack("<B", 1 if tel["fan_expected"] else 0)
    out += struct.pack("<B", 1 if tel["fan_control_fault_seen"] else 0)
    out += struct.pack("<B", 1 if tel["emergency_thermal_seen"] else 0)
    out += struct.pack("<I", int(tel["restart_count"]))
    # --- 7: the governed profile parameters (this is the binding)
    out += _fixed_str(profile["id"], PROFILE_ID_MAX)
    out += struct.pack("<H", int(profile["revision"]))
    out += struct.pack("<H", int(profile["model_version"]))
    out += struct.pack("<BBBB", int(profile["board"]), int(profile["asic"]),
                       int(profile["required_cooling"]), int(profile["required_psu"]))
    out += struct.pack("<B", int(profile["rank"]))
    out += struct.pack("<B", 1 if profile["payload_present"] else 0)
    out += struct.pack("<H", int(profile["frequency_mhz"]))
    out += struct.pack("<H", int(profile["core_voltage_mv"]))
    out += struct.pack("<B", int(profile["fan_curve_count"]))
    curve = profile.get("fan_curve", [])
    for i in range(FAN_CURVE_MAX_POINTS):
        pt = curve[i] if i < len(curve) else {"temp_c": 0, "fan_percent": 0}
        out += struct.pack("<BB", int(pt["temp_c"]), int(pt["fan_percent"]))
    for key in ("asic_warn_dc", "asic_crit_dc", "vrm_warn_dc", "vrm_crit_dc"):
        out += struct.pack("<i", int(profile[key]))
    out += _fixed_str(profile.get("rollback_profile_id", ""), PROFILE_ID_MAX)
    out += struct.pack("<B", 1 if profile["disabled"] else 0)

    data = bytes(out)
    if len(data) != CANONICAL_BYTES:
        raise EvidenceError(
            f"canonical length {len(data)} != contract {CANONICAL_BYTES} — the "
            "C encoder and this verifier have diverged")
    return data


def binding_digest(ev: dict, profile: dict) -> bytes:
    """SHA-256 over the canonical bytes. The firmware side never computes this."""
    return hashlib.sha256(canonical_encode(ev, profile)).digest()


# --------------------------------------------------------------------------
# Qualification — the same decision order as the C implementation
# --------------------------------------------------------------------------

def qualify(ev: dict, profile: dict) -> dict:
    """Return a deterministic qualification result. Never promotes anything."""
    def verdict(v, reason, **extra):
        out = {"verdict": v, "reason": reason,
               "binding_verified": False,
               "ungoverned_criteria": list(UNGOVERNED_CRITERIA)}
        out.update(extra)
        return out

    schema = int(ev.get("schema_version", 0))
    if schema == 0 or schema < SCHEMA_VERSION_MIN:
        return verdict(EVIDENCE_INCOMPLETE, "REASON_SCHEMA_UNKNOWN")
    if schema > SCHEMA_VERSION:
        return verdict(EVIDENCE_REJECTED, "REASON_SCHEMA_FUTURE")

    if not ev.get("profile_id"):
        return verdict(EVIDENCE_INCOMPLETE, "REASON_PROFILE_ID_INVALID")
    if ev["profile_id"] != profile["id"]:
        return verdict(EVIDENCE_REJECTED, "REASON_PROFILE_UNKNOWN")
    if int(ev["profile_revision"]) != int(profile["revision"]):
        return verdict(EVIDENCE_REJECTED, "REASON_PROFILE_REVISION_MISMATCH")
    if int(ev["board"]) != int(profile["board"]):
        return verdict(EVIDENCE_REJECTED, "REASON_BOARD_MISMATCH")
    if int(ev["asic"]) != int(profile["asic"]):
        return verdict(EVIDENCE_REJECTED, "REASON_ASIC_MISMATCH")

    if not ev.get("firmware_revision"):
        return verdict(EVIDENCE_INCOMPLETE, "REASON_FIRMWARE_REVISION_MISSING")
    if int(ev.get("run_generation", 0)) == 0:
        return verdict(EVIDENCE_INCOMPLETE, "REASON_RUN_GENERATION_MISSING")

    inst = ev["installation"]
    if not inst.get("declared"):
        return verdict(EVIDENCE_INCOMPLETE, "REASON_INSTALLATION_UNDECLARED")
    # Ordered capability tiers, exactly as tuning_profile_compatible does.
    if int(inst["cooling_installed"]) < int(profile["required_cooling"]):
        return verdict(EVIDENCE_REJECTED, "REASON_COOLING_INSUFFICIENT")
    if int(inst["psu_installed"]) < int(profile["required_psu"]):
        return verdict(EVIDENCE_REJECTED, "REASON_PSU_INSUFFICIENT")

    status = int(ev["run_status"])
    if status == RUN_ABORTED:
        return verdict(EVIDENCE_REJECTED, "REASON_RUN_ABORTED")
    if status != RUN_COMPLETED:
        return verdict(EVIDENCE_INCOMPLETE, "REASON_RUN_INCOMPLETE")

    tel = ev["telemetry"]
    if not tel.get("present"):
        return verdict(EVIDENCE_INCOMPLETE, "REASON_TELEMETRY_ABSENT")
    if int(tel["samples_total"]) == 0 or \
            int(tel["samples_valid"]) > int(tel["samples_total"]):
        return verdict(EVIDENCE_REJECTED, "REASON_SAMPLE_ACCOUNTING_INVALID")
    if not tel["asic_temp_valid_all"]:
        return verdict(EVIDENCE_REJECTED, "REASON_ASIC_TELEMETRY_INVALID")
    if tel["vrm_expected"] and not tel["vrm_read_ok_all"]:
        return verdict(EVIDENCE_REJECTED, "REASON_VRM_TELEMETRY_INVALID")
    if tel["fan_expected"] and int(tel["fan_rpm_min"]) == 0:
        return verdict(EVIDENCE_REJECTED, "REASON_FAN_TELEMETRY_INVALID")
    if tel["fan_control_fault_seen"]:
        return verdict(EVIDENCE_REJECTED, "REASON_FAN_CONTROL_FAULT")
    if tel["emergency_thermal_seen"]:
        return verdict(EVIDENCE_REJECTED, "REASON_EMERGENCY_THERMAL")
    if int(tel["restart_count"]) > 0:
        return verdict(EVIDENCE_REJECTED, "REASON_RESTART_DURING_RUN")

    try:
        expected = binding_digest(ev, profile)
    except EvidenceError:
        return verdict(EVIDENCE_INCOMPLETE, "REASON_SCHEMA_UNKNOWN")
    supplied = bytes.fromhex(ev.get("binding_digest", ""))
    if len(supplied) != DIGEST_BYTES or supplied != expected:
        return verdict(EVIDENCE_REJECTED, "REASON_BINDING_MISMATCH")

    # THE GOVERNANCE GATE. Structure and integrity are sound; physical
    # acceptance is not governed by anything committed, so this is where an
    # honest tool stops.
    if not profile.get("payload_present"):
        return verdict(REQUIREMENT_MISSING, "REASON_PROFILE_PAYLOAD_ABSENT",
                       binding_verified=True)
    if UNGOVERNED_CRITERIA:
        return verdict(REQUIREMENT_MISSING, "REASON_THRESHOLD_UNGOVERNED",
                       binding_verified=True)

    return verdict(QUALIFIED_FOR_OWNER_REVIEW, "REASON_OK", binding_verified=True)


def review_summary(ev: dict, profile: dict, result: dict) -> str:
    """A bounded, human-readable summary for the owner. No private values."""
    lines = [
        "NeuralAxe — Gamma 601 evidence review summary",
        "=" * 46,
        f"profile              : {ev.get('profile_id', '?')} rev {ev.get('profile_revision', '?')}",
        f"board / asic         : {ev.get('board', '?')} / {ev.get('asic', '?')}",
        f"firmware revision    : {ev.get('firmware_revision', '?')}",
        f"run generation       : {ev.get('run_generation', '?')}",
        f"run duration (s)     : {ev.get('run_duration_s', '?')}",
        f"binding verified     : {result['binding_verified']}",
        f"automated verdict    : {result['verdict']}",
        f"reason               : {result['reason']}",
        "",
        "THIS IS NOT A VALIDATION. The best automated verdict possible is",
        "QUALIFIED_FOR_OWNER_REVIEW, and promotion to TUNING_VALIDATION_VALIDATED",
        "remains a separate owner decision that no tool performs.",
    ]
    if result["verdict"] == REQUIREMENT_MISSING:
        lines += ["", "Blocked on owner decisions — no committed acceptance rule exists for:"]
        lines += [f"  - {c}" for c in result["ungoverned_criteria"]]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Offline Gamma 601 evidence verifier. Reads only; never "
                    "promotes, never edits source, never touches hardware.")
    ap.add_argument("evidence", type=Path, help="candidate evidence bundle (JSON)")
    ap.add_argument("profile", type=Path, help="profile descriptor (JSON)")
    ap.add_argument("--json", action="store_true", help="emit the raw result")
    ap.add_argument("--canonical-hex", action="store_true",
                    help="print the canonical bytes and their SHA-256")
    args = ap.parse_args(argv)

    try:
        ev = json.loads(args.evidence.read_text(encoding="utf-8"))
        profile = json.loads(args.profile.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"cannot read input: {exc.__class__.__name__}", file=sys.stderr)
        return 2

    if args.canonical_hex:
        data = canonical_encode(ev, profile)
        print(f"canonical_len : {len(data)}")
        print(f"canonical_hex : {data.hex()}")
        print(f"sha256        : {hashlib.sha256(data).hexdigest()}")
        return 0

    result = qualify(ev, profile)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(review_summary(ev, profile, result))

    # Exit non-zero unless the evidence is genuinely reviewable, so a caller
    # cannot mistake "not decided" for "fine".
    return 0 if result["verdict"] == QUALIFIED_FOR_OWNER_REVIEW else 1


if __name__ == "__main__":
    sys.exit(main())
