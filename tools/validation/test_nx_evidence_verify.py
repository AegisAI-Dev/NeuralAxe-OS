#!/usr/bin/env python3
"""Tests for the offline Gamma 601 evidence verifier (Gate W6.5A).

The load-bearing test in this file is the GOLDEN VECTOR: it asserts the exact
same canonical length and checksum that the firmware-side C test
(components/tuning_evidence/test/test_nx_tuning_evidence.c) asserts over the
same fixture. Two independent implementations agreeing byte for byte is what
makes the binding meaningful; if either drifts, both fail.

Nothing here touches hardware, the network, Git or any owner-private value.
"""

import copy
import hashlib
import json
import unittest
from pathlib import Path

import nx_evidence_verify as v


# --------------------------------------------------------------------------
# Fixtures — synthetic, and they stay synthetic
# --------------------------------------------------------------------------

GOLDEN_PROFILE = dict(
    id="golden", revision=1, model_version=1, board=1, asic=1,
    required_cooling=1, required_psu=1, rank=1, payload_present=True,
    frequency_mhz=500, core_voltage_mv=0, fan_curve_count=0, fan_curve=[],
    asic_warn_dc=0, asic_crit_dc=0, vrm_warn_dc=0, vrm_crit_dc=0,
    rollback_profile_id="", disabled=False)

GOLDEN_EVIDENCE = dict(
    schema_version=1, profile_id="golden", profile_revision=1, board=1, asic=1,
    firmware_revision="v1.0.0-0-gabcdef1",
    installation=dict(declared=True, cooling_installed=1, psu_installed=1),
    run_duration_s=60, run_generation=1, run_status=v.RUN_COMPLETED,
    telemetry=dict(present=True, samples_total=60, samples_valid=60,
                   asic_temp_max_dc=600, asic_temp_min_dc=400,
                   asic_temp_valid_all=True, vrm_temp_max_dc=700,
                   vrm_expected=True, vrm_read_ok_all=True, fan_rpm_min=3000,
                   fan_expected=True, fan_control_fault_seen=False,
                   emergency_thermal_seen=False, restart_count=0))


def sealed(ev, profile):
    """Return a copy whose binding digest is correct for `profile`."""
    out = copy.deepcopy(ev)
    out["binding_digest"] = v.binding_digest(out, profile).hex()
    return out


class GoldenVector(unittest.TestCase):
    def test_canonical_length_matches_the_c_contract(self):
        data = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        self.assertEqual(len(data), v.CANONICAL_BYTES)
        self.assertEqual(len(data), 195)

    def test_golden_checksum_matches_the_c_test_exactly(self):
        """THE cross-implementation proof.

        components/tuning_evidence/test/test_nx_tuning_evidence.c asserts this
        same number over the same fixture. A field reorder, width change or
        endianness slip in EITHER implementation breaks this.
        """
        data = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        checksum = 0
        for byte in data:
            checksum = ((checksum * 31) + byte) & 0xFFFFFFFF
        self.assertEqual(checksum, 2809923663)

    def test_golden_sha256_is_pinned(self):
        data = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        self.assertEqual(
            hashlib.sha256(data).hexdigest(),
            "66566931d2aed8240c0ef79ca45faefbae9b79b8f0d614882bf974d7b6b73e87")

    def test_leading_bytes_are_schema_then_padded_id(self):
        data = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        self.assertEqual(data[0], 0x01)          # schema lo
        self.assertEqual(data[1], 0x00)          # schema hi
        self.assertEqual(data[2:8], b"golden")
        self.assertEqual(data[8], 0x00)          # zero padding, not residue

    def test_encoding_is_deterministic(self):
        a = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        b = v.canonical_encode(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        self.assertEqual(a, b)


class BindingSensitivity(unittest.TestCase):
    def _digest(self, ev=None, profile=None):
        return v.binding_digest(ev or GOLDEN_EVIDENCE, profile or GOLDEN_PROFILE)

    def test_every_load_bearing_field_changes_the_digest(self):
        base = self._digest()
        mutations = [
            ("schema", {"schema_version": 1}, None),   # unchanged control
            ("board", {"board": 0}, None),
            ("asic", {"asic": 0}, None),
            ("profile_revision", {"profile_revision": 2}, None),
            ("run_generation", {"run_generation": 2}, None),
            ("run_duration", {"run_duration_s": 61}, None),
            ("firmware_revision", {"firmware_revision": "v9.9.9-0-gffffff1"}, None),
        ]
        for name, patch, _ in mutations:
            ev = copy.deepcopy(GOLDEN_EVIDENCE)
            ev.update(patch)
            digest = self._digest(ev)
            if name == "schema":
                self.assertEqual(digest, base, "control mutation changed the digest")
            else:
                self.assertNotEqual(digest, base, f"{name} did not affect the digest")

    def test_profile_parameter_change_invalidates_evidence(self):
        base = self._digest()
        for field, value in (("frequency_mhz", 550), ("core_voltage_mv", 1150),
                             ("rank", 2), ("required_cooling", 2),
                             ("revision", 2), ("disabled", True)):
            profile = copy.deepcopy(GOLDEN_PROFILE)
            profile[field] = value
            self.assertNotEqual(self._digest(None, profile), base,
                                f"profile.{field} is not part of the binding")

    def test_telemetry_change_invalidates_evidence(self):
        base = self._digest()
        for field, value in (("asic_temp_max_dc", 601), ("fan_rpm_min", 3001),
                             ("samples_total", 61), ("restart_count", 1)):
            ev = copy.deepcopy(GOLDEN_EVIDENCE)
            ev["telemetry"][field] = value
            self.assertNotEqual(self._digest(ev), base,
                                f"telemetry.{field} is not part of the binding")

    def test_owner_approval_is_not_part_of_the_binding(self):
        """Approval is a later authority; it must not change evidence identity."""
        base = self._digest()
        ev = copy.deepcopy(GOLDEN_EVIDENCE)
        ev["owner_approved"] = True
        self.assertEqual(self._digest(ev), base)

    def test_promotion_state_is_not_part_of_the_binding(self):
        """Otherwise promoting a profile would invalidate its own evidence."""
        base = self._digest()
        profile = copy.deepcopy(GOLDEN_PROFILE)
        profile["validation"] = 1
        profile["evidence_fingerprint"] = 0xDEADBEEF
        self.assertEqual(self._digest(None, profile), base)


class Qualification(unittest.TestCase):
    def test_structurally_perfect_evidence_is_REQUIREMENT_MISSING(self):
        """The central governance assertion, mirrored from the C suite."""
        ev = sealed(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        result = v.qualify(ev, GOLDEN_PROFILE)
        self.assertEqual(result["verdict"], v.REQUIREMENT_MISSING)
        self.assertEqual(result["reason"], "REASON_THRESHOLD_UNGOVERNED")
        self.assertTrue(result["binding_verified"])
        self.assertTrue(len(result["ungoverned_criteria"]) > 0)

    def test_payload_free_profile_cannot_qualify(self):
        profile = copy.deepcopy(GOLDEN_PROFILE)
        profile["payload_present"] = False
        profile["frequency_mhz"] = 0
        ev = sealed(GOLDEN_EVIDENCE, profile)
        result = v.qualify(ev, profile)
        self.assertEqual(result["verdict"], v.REQUIREMENT_MISSING)
        self.assertEqual(result["reason"], "REASON_PROFILE_PAYLOAD_ABSENT")

    def test_future_schema_is_rejected(self):
        ev = sealed(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        ev["schema_version"] = v.SCHEMA_VERSION + 1
        self.assertEqual(v.qualify(ev, GOLDEN_PROFILE)["verdict"],
                         v.EVIDENCE_REJECTED)

    def test_tampered_digest_is_rejected(self):
        ev = sealed(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        raw = bytearray(bytes.fromhex(ev["binding_digest"]))
        raw[0] ^= 0xFF
        ev["binding_digest"] = raw.hex()
        result = v.qualify(ev, GOLDEN_PROFILE)
        self.assertEqual(result["verdict"], v.EVIDENCE_REJECTED)
        self.assertEqual(result["reason"], "REASON_BINDING_MISMATCH")
        self.assertFalse(result["binding_verified"])

    def test_missing_digest_is_rejected(self):
        ev = copy.deepcopy(GOLDEN_EVIDENCE)
        ev["binding_digest"] = ""
        self.assertEqual(v.qualify(ev, GOLDEN_PROFILE)["verdict"],
                         v.EVIDENCE_REJECTED)

    def test_each_failure_mode_fails_closed(self):
        cases = [
            ({"run_status": v.RUN_ABORTED}, v.EVIDENCE_REJECTED),
            ({"run_status": v.RUN_INCOMPLETE}, v.EVIDENCE_INCOMPLETE),
            ({"run_generation": 0}, v.EVIDENCE_INCOMPLETE),
            ({"firmware_revision": ""}, v.EVIDENCE_INCOMPLETE),
            ({"board": 0}, v.EVIDENCE_REJECTED),
            ({"asic": 0}, v.EVIDENCE_REJECTED),
            ({"profile_id": "nope"}, v.EVIDENCE_REJECTED),
            ({"profile_revision": 99}, v.EVIDENCE_REJECTED),
        ]
        for patch, expected in cases:
            ev = copy.deepcopy(GOLDEN_EVIDENCE)
            ev.update(patch)
            ev = sealed(ev, GOLDEN_PROFILE)
            self.assertEqual(v.qualify(ev, GOLDEN_PROFILE)["verdict"], expected,
                             f"{patch} did not fail closed")

    def test_telemetry_failures_fail_closed(self):
        cases = [
            ({"present": False}, v.EVIDENCE_INCOMPLETE),
            ({"asic_temp_valid_all": False}, v.EVIDENCE_REJECTED),
            ({"vrm_read_ok_all": False}, v.EVIDENCE_REJECTED),
            ({"fan_rpm_min": 0}, v.EVIDENCE_REJECTED),
            ({"fan_control_fault_seen": True}, v.EVIDENCE_REJECTED),
            ({"emergency_thermal_seen": True}, v.EVIDENCE_REJECTED),
            ({"restart_count": 1}, v.EVIDENCE_REJECTED),
            ({"samples_total": 0}, v.EVIDENCE_REJECTED),
        ]
        for patch, expected in cases:
            ev = copy.deepcopy(GOLDEN_EVIDENCE)
            ev["telemetry"].update(patch)
            ev = sealed(ev, GOLDEN_PROFILE)
            self.assertEqual(v.qualify(ev, GOLDEN_PROFILE)["verdict"], expected,
                             f"telemetry {patch} did not fail closed")

    def test_undeclared_installation_is_incomplete(self):
        ev = copy.deepcopy(GOLDEN_EVIDENCE)
        ev["installation"]["declared"] = False
        ev = sealed(ev, GOLDEN_PROFILE)
        result = v.qualify(ev, GOLDEN_PROFILE)
        self.assertEqual(result["verdict"], v.EVIDENCE_INCOMPLETE)
        self.assertEqual(result["reason"], "REASON_INSTALLATION_UNDECLARED")

    def test_insufficient_cooling_is_rejected(self):
        profile = copy.deepcopy(GOLDEN_PROFILE)
        profile["required_cooling"] = 2
        ev = copy.deepcopy(GOLDEN_EVIDENCE)
        ev["installation"]["cooling_installed"] = 1
        ev = sealed(ev, profile)
        self.assertEqual(v.qualify(ev, profile)["reason"],
                         "REASON_COOLING_INSUFFICIENT")


class GovernanceBoundary(unittest.TestCase):
    def test_no_verdict_can_express_production_validation(self):
        """The tool literally has no VALIDATED verdict to emit."""
        verdicts = {v.UNVALIDATED, v.EVIDENCE_INCOMPLETE, v.EVIDENCE_REJECTED,
                    v.REQUIREMENT_MISSING, v.QUALIFIED_FOR_OWNER_REVIEW}
        for verdict in verdicts:
            self.assertNotIn("TEV_VALIDATED", verdict)
        self.assertNotIn("VALIDATED", {x.replace("TEV_UNVALIDATED", "")
                                       for x in verdicts})

    def test_the_module_exposes_no_promotion_function(self):
        for name in dir(v):
            lowered = name.lower()
            self.assertFalse(
                "promote" in lowered or "validate_profile" in lowered
                or "write_registry" in lowered,
                f"the offline verifier exposes a promotion-shaped symbol: {name}")

    def test_ungoverned_criteria_are_named_not_defaulted(self):
        self.assertTrue(len(v.UNGOVERNED_CRITERIA) > 0)
        for criterion in v.UNGOVERNED_CRITERIA:
            self.assertIsInstance(criterion, str)
            self.assertTrue(criterion.strip())

    def test_review_summary_carries_no_private_value(self):
        ev = sealed(GOLDEN_EVIDENCE, GOLDEN_PROFILE)
        text = v.review_summary(ev, GOLDEN_PROFILE, v.qualify(ev, GOLDEN_PROFILE))
        for token in ("ssid", "psk", "password", "pool", "payout", "ntp",
                      "latitude", "longitude", "wifi"):
            self.assertNotIn(token, text.lower())
        self.assertIn("THIS IS NOT A VALIDATION", text)


class ProductionRegistryUntouched(unittest.TestCase):
    def test_no_synthetic_fingerprint_appears_in_committed_source(self):
        """Synthetic evidence must never leak into the production registry."""
        repo = Path(__file__).resolve().parents[2]
        registry = (repo / "components" / "tuning_profile" / "tuning_profile.c")
        text = registry.read_text(encoding="utf-8")
        digest = v.binding_digest(GOLDEN_EVIDENCE, GOLDEN_PROFILE).hex()
        self.assertNotIn(digest[:16], text)
        # And every production entry is still evidence-free and UNVALIDATED.
        self.assertEqual(text.count(".evidence_fingerprint = 0,"), 3)
        self.assertEqual(text.count(".validation = TUNING_VALIDATION_UNVALIDATED,"), 3)
        self.assertNotIn("TUNING_VALIDATION_VALIDATED,", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
