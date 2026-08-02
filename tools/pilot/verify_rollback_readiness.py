#!/usr/bin/env python3
"""NeuralAxe OS — Gate B10.2 OFFLINE rollback-readiness verifier.

Validates an owner-recorded INSTALLED firmware posture against a local
rollback package, so "we can roll back" is a checked fact rather than an
assumption, before any preflight or pilot image is flashed.

It is strictly offline and read-only. It NEVER contacts the device, resolves a
hostname, opens a socket, reads NVS, runs esptool, flashes anything or writes
to Git. It reads local files, hashes them, and prints a verdict.

The installed firmware identity is supplied BY THE OWNER (read off the AxeOS
dashboard or the serial log). This tool cannot and does not obtain it from the
device.

Stable outcomes:
    ROLLBACK_READY_EXACT_MATCH
    ROLLBACK_READY_EXPLICIT_DOWNGRADE
    ROLLBACK_BLOCKED_VERSION_UNKNOWN
    ROLLBACK_BLOCKED_BOARD_MISMATCH
    ROLLBACK_BLOCKED_PAIR_MISMATCH
    ROLLBACK_BLOCKED_HASH_MISMATCH
    ROLLBACK_BLOCKED_FACTORY_MISSING
    ROLLBACK_BLOCKED_MANIFEST_INVALID

Usage:
    python tools/pilot/verify_rollback_readiness.py \\
        --manifest '<rollback-manifest.json>' \\
        --installed-version v2.14.2-43-gd333dc4 \\
        --installed-axeos-version v2.14.2-43-gd333dc4 \\
        --board 601 --require-factory
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SCHEMA_NOTE = "reads the manifest produced by tools/release/export_release.py"

READY_EXACT = "ROLLBACK_READY_EXACT_MATCH"
READY_DOWNGRADE = "ROLLBACK_READY_EXPLICIT_DOWNGRADE"
BLOCKED_VERSION_UNKNOWN = "ROLLBACK_BLOCKED_VERSION_UNKNOWN"
BLOCKED_BOARD_MISMATCH = "ROLLBACK_BLOCKED_BOARD_MISMATCH"
BLOCKED_PAIR_MISMATCH = "ROLLBACK_BLOCKED_PAIR_MISMATCH"
BLOCKED_HASH_MISMATCH = "ROLLBACK_BLOCKED_HASH_MISMATCH"
BLOCKED_FACTORY_MISSING = "ROLLBACK_BLOCKED_FACTORY_MISSING"
BLOCKED_MANIFEST_INVALID = "ROLLBACK_BLOCKED_MANIFEST_INVALID"

ALL_OUTCOMES = (
    READY_EXACT, READY_DOWNGRADE, BLOCKED_VERSION_UNKNOWN, BLOCKED_BOARD_MISMATCH,
    BLOCKED_PAIR_MISMATCH, BLOCKED_HASH_MISMATCH, BLOCKED_FACTORY_MISSING,
    BLOCKED_MANIFEST_INVALID,
)

# Board 601 / Gamma / BM1370 is the only supported target of this release line.
EXPECTED_BOARD = "601"
EXPECTED_DEVICE = "Gamma"
EXPECTED_ASIC = "BM1370"

REVISION_RE = re.compile(r"^v\d+\.\d+\.\d+(?:-(\d+)-g[0-9a-f]{7,12})?$")


def is_ready(outcome: str) -> bool:
    """Total: only the two READY outcomes permit proceeding."""
    return outcome in (READY_EXACT, READY_DOWNGRADE)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def revision_distance(rev: str) -> int | None:
    """Commit count encoded in a git-describe revision, or None when absent."""
    m = REVISION_RE.match(rev.strip()) if rev else None
    if m is None:
        return None
    return int(m.group(1)) if m.group(1) else 0


class Verdict:
    def __init__(self) -> None:
        self.outcome = BLOCKED_MANIFEST_INVALID  # fail closed
        self.reasons: list[str] = []
        self.checked: list[str] = []

    def block(self, outcome: str, reason: str) -> "Verdict":
        self.outcome = outcome
        self.reasons.append(reason)
        return self

    def note(self, text: str) -> None:
        self.checked.append(text)


def verify(manifest_path: Path, installed_version: str | None,
           installed_axeos_version: str | None, board: str,
           allow_downgrade: bool, require_factory: bool,
           artifact_dir: Path | None = None) -> Verdict:
    """The whole offline check. Total: always returns a Verdict, never raises
    for ordinary bad input."""
    v = Verdict()

    if not manifest_path.is_file():
        return v.block(BLOCKED_MANIFEST_INVALID, "rollback manifest not found")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        return v.block(BLOCKED_MANIFEST_INVALID, "rollback manifest is not valid JSON")
    if not isinstance(manifest, dict):
        return v.block(BLOCKED_MANIFEST_INVALID, "rollback manifest is not an object")

    root = artifact_dir.resolve() if artifact_dir else manifest_path.resolve().parent
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        return v.block(BLOCKED_MANIFEST_INVALID, "rollback manifest lists no artifacts")

    fw_rev = manifest.get("firmwareRevision")
    web_rev = manifest.get("webRevision")
    if not isinstance(fw_rev, str) or not isinstance(web_rev, str) or not fw_rev or not web_rev:
        return v.block(BLOCKED_MANIFEST_INVALID,
                       "rollback manifest does not record both artifact revisions")
    if "-dirty" in fw_rev or "-dirty" in web_rev:
        return v.block(BLOCKED_MANIFEST_INVALID, "rollback manifest records a dirty revision")

    # --- board target -----------------------------------------------------
    m_board = str(manifest.get("targetBoard", ""))
    if m_board != EXPECTED_BOARD or m_board != str(board):
        return v.block(BLOCKED_BOARD_MISMATCH,
                       f"manifest board '{m_board}' is not the expected '{board}'")
    if manifest.get("targetDevice") != EXPECTED_DEVICE or manifest.get("targetAsic") != EXPECTED_ASIC:
        return v.block(BLOCKED_BOARD_MISMATCH,
                       "manifest device/ASIC is not Gamma / BM1370")
    supported = manifest.get("supportedBoards")
    if isinstance(supported, list) and [str(s) for s in supported] != [EXPECTED_BOARD]:
        return v.block(BLOCKED_BOARD_MISMATCH,
                       "manifest declares boards other than 601")
    v.note(f"board target {EXPECTED_DEVICE}/{EXPECTED_BOARD}/{EXPECTED_ASIC}")

    # --- files exist and hash correctly -----------------------------------
    by_type: dict[str, dict] = {}
    for a in artifacts:
        if not isinstance(a, dict):
            return v.block(BLOCKED_MANIFEST_INVALID, "malformed artifact entry")
        name, digest, kind = a.get("filename"), a.get("sha256"), a.get("artifactType")
        if not isinstance(name, str) or not isinstance(digest, str) or len(digest) != 64:
            return v.block(BLOCKED_MANIFEST_INVALID,
                           "artifact entry lacks a filename or a SHA-256")
        path = root / name
        if not path.is_file():
            return v.block(BLOCKED_HASH_MISMATCH, f"artifact missing: {name}")
        actual = sha256_of(path)
        if actual != digest:
            # A silently substituted file is exactly this case.
            return v.block(BLOCKED_HASH_MISMATCH, f"checksum mismatch: {name}")
        size = a.get("sizeBytes")
        if isinstance(size, int) and path.stat().st_size != size:
            return v.block(BLOCKED_HASH_MISMATCH, f"size mismatch: {name}")
        if isinstance(kind, str):
            by_type[kind] = a
    v.note(f"{len(artifacts)} artifact(s) present with matching SHA-256")

    if "ota-application" not in by_type:
        return v.block(BLOCKED_MANIFEST_INVALID, "no OTA application artifact listed")
    if "www-update" not in by_type:
        return v.block(BLOCKED_MANIFEST_INVALID, "no web (www) artifact listed")

    # --- application/www coherence ---------------------------------------
    if fw_rev != web_rev:
        return v.block(BLOCKED_PAIR_MISMATCH,
                       f"application '{fw_rev}' and web '{web_rev}' are not one pair")
    v.note(f"application and web artifacts are a coherent pair at {fw_rev}")

    # --- serial recovery prerequisite ------------------------------------
    if require_factory:
        factory = by_type.get("factory-image")
        if factory is None:
            return v.block(BLOCKED_FACTORY_MISSING,
                           "serial recovery claimed but no factory image is listed")
        if not (root / factory["filename"]).is_file():
            return v.block(BLOCKED_FACTORY_MISSING, "factory image file is missing")
        v.note("factory recovery image present with a matching checksum")

    # --- installed posture ------------------------------------------------
    if not installed_version or not installed_version.strip():
        return v.block(BLOCKED_VERSION_UNKNOWN,
                       "the installed firmware version was not supplied — it must be read "
                       "off the device by the owner, never guessed")
    installed_version = installed_version.strip()
    if REVISION_RE.match(installed_version) is None:
        return v.block(BLOCKED_VERSION_UNKNOWN,
                       "the supplied installed version is not a recognizable revision")

    if installed_axeos_version and installed_axeos_version.strip():
        if installed_axeos_version.strip() != installed_version:
            return v.block(BLOCKED_PAIR_MISMATCH,
                           "the installed application and web versions disagree — the "
                           "device is not running a coherent pair")
        v.note("installed application and web versions agree")

    if installed_version == fw_rev:
        v.outcome = READY_EXACT
        v.note("the rollback pair is exactly the installed posture")
        return v

    # Different from what is installed: this is a downgrade (or a sidegrade),
    # and it must be an explicit, deliberate owner decision.
    if not allow_downgrade:
        return v.block(BLOCKED_VERSION_UNKNOWN,
                       f"the rollback pair ({fw_rev}) is not the installed posture "
                       f"({installed_version}); re-run with --allow-downgrade to accept "
                       "this deliberately")

    inst_d, roll_d = revision_distance(installed_version), revision_distance(fw_rev)
    if inst_d is None or roll_d is None:
        return v.block(BLOCKED_VERSION_UNKNOWN,
                       "cannot order the installed and rollback revisions")
    v.outcome = READY_DOWNGRADE
    v.note(f"explicitly approved change from {installed_version} to {fw_rev} "
           f"({'downgrade' if roll_d < inst_d else 'sidegrade/upgrade'})")
    return v


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--manifest", required=True, help="path to the rollback manifest JSON")
    p.add_argument("--artifact-dir", default=None,
                   help="directory holding the rollback files (default: the manifest's own)")
    p.add_argument("--installed-version", default=None,
                   help="firmware version READ OFF THE DEVICE by the owner")
    p.add_argument("--installed-axeos-version", default=None,
                   help="axeOSVersion read off the device by the owner")
    p.add_argument("--board", default=EXPECTED_BOARD)
    p.add_argument("--allow-downgrade", action="store_true",
                   help="explicitly accept a rollback pair that is not the installed posture")
    p.add_argument("--require-factory", action="store_true",
                   help="require a factory image, for the serial/USB recovery path")
    args = p.parse_args()

    v = verify(Path(args.manifest), args.installed_version, args.installed_axeos_version,
               args.board, args.allow_downgrade, args.require_factory,
               Path(args.artifact_dir) if args.artifact_dir else None)

    for note in v.checked:
        print(f"  ok   {note}")
    for reason in v.reasons:
        print(f"  !!   {reason}")
    print(f"\n{v.outcome}")
    return 0 if is_ready(v.outcome) else 1


if __name__ == "__main__":
    raise SystemExit(main())
