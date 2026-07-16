#!/usr/bin/env python3
"""NeuralAxe OS release manifest validator.

Fails (exit 1) when:
- board identity is missing or is not exactly Gamma/601/BM1370;
- any unsupported board (e.g. 702) appears anywhere in the manifest;
- an artifact filename disagrees with the manifest board identity;
- a listed file is missing, or its size or SHA-256 disagrees;
- an artifact type is not in the allowed set;
- anything matching a private recovery dump pattern is listed or present;
- the source revision contains "-dirty";
- required fields are absent.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ALLOWED_TYPES = {
    "www-update",
    "ota-application",
    "factory-image",
    "board-provisioning-config",
}

REQUIRED_FIELDS = [
    "schemaVersion", "productName", "productVersion", "buildChannel", "vendor",
    "sourceRevision", "upstreamProject", "upstreamVersion",
    "targetDevice", "targetBoard", "targetAsic", "supportedBoards", "artifacts",
]

REQUIRED_ARTIFACT_FIELDS = [
    "filename", "artifactType", "sizeBytes", "sha256",
    "flashMethod", "settingsPreserved", "destructive",
]

FORBIDDEN_NAME_PATTERNS = re.compile(r"(flashdump|flash-dump|nvs[-_]?dump|nvs[-_]?backup|tch)", re.IGNORECASE)
UNSUPPORTED_BOARD_PATTERN = re.compile(r"\b(102|201|202|203|204|205|207|302|303|400|401|402|403|602|603|650|701|702|801)\b")

errors: list[str] = []


def err(msg: str) -> None:
    errors.append(msg)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, help="path to the release manifest JSON")
    args = parser.parse_args()

    manifest_path = Path(args.manifest).resolve()
    if not manifest_path.is_file():
        print(f"VALIDATION FAIL: manifest not found: {manifest_path}", file=sys.stderr)
        sys.exit(1)
    release_dir = manifest_path.parent

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    for field in REQUIRED_FIELDS:
        if field not in manifest or manifest[field] in (None, "", []):
            err(f"required field missing/empty: {field}")

    board = str(manifest.get("targetBoard", ""))
    device = str(manifest.get("targetDevice", ""))
    asic = str(manifest.get("targetAsic", ""))
    if (board, device, asic) != ("601", "Gamma", "BM1370"):
        err(f"board identity must be Gamma/601/BM1370, got {device}/{board}/{asic}")

    if manifest.get("supportedBoards") != ["601"]:
        err(f"supportedBoards must be exactly ['601'], got {manifest.get('supportedBoards')}")

    if "-dirty" in str(manifest.get("sourceRevision", "")):
        err("sourceRevision contains -dirty; dirty builds must not be released")

    # No unsupported board number may appear in any manifest string value.
    def walk(value, path="manifest"):
        if isinstance(value, dict):
            for k, v in value.items():
                walk(v, f"{path}.{k}")
        elif isinstance(value, list):
            for i, v in enumerate(value):
                walk(v, f"{path}[{i}]")
        elif isinstance(value, str):
            for m in UNSUPPORTED_BOARD_PATTERN.finditer(value):
                # 601 is the only supported board; any other board token is a violation
                err(f"unsupported board '{m.group(1)}' mentioned at {path}: '{value}'")
    walk(manifest)

    expected_token = f"{device}-{board}"
    for a in manifest.get("artifacts", []):
        for field in REQUIRED_ARTIFACT_FIELDS:
            if field not in a:
                err(f"artifact missing field '{field}': {a.get('filename', '<unnamed>')}")
                continue

        name = a.get("filename", "")
        if FORBIDDEN_NAME_PATTERNS.search(name):
            err(f"private recovery dump must not be listed as a release artifact: {name}")

        if a.get("artifactType") not in ALLOWED_TYPES:
            err(f"artifact type not allowed: {name}: {a.get('artifactType')}")

        # Filename/board agreement: release-named binaries carry Device-Board;
        # the provisioning config carries the plain board number.
        if a.get("artifactType") == "board-provisioning-config":
            if board not in name:
                err(f"provisioning config filename does not carry board {board}: {name}")
        elif expected_token not in name:
            err(f"artifact filename does not carry board identity '{expected_token}': {name}")

        f = release_dir / name
        if not f.is_file():
            err(f"artifact file missing: {name}")
            continue
        actual_size = f.stat().st_size
        if actual_size != a.get("sizeBytes"):
            err(f"size mismatch for {name}: manifest={a.get('sizeBytes')} actual={actual_size}")
        actual_hash = sha256_of(f)
        if actual_hash != a.get("sha256"):
            err(f"sha256 mismatch for {name}: manifest={a.get('sha256')} actual={actual_hash}")

    # Nothing in the release directory may look like a private dump.
    for f in release_dir.iterdir():
        if f.is_file() and FORBIDDEN_NAME_PATTERNS.search(f.name):
            err(f"private-dump-like file present in release directory: {f.name}")

    if errors:
        print("VALIDATION FAIL:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        sys.exit(1)

    print(f"VALIDATION OK: {manifest_path.name} ({len(manifest.get('artifacts', []))} artifacts, board {device}/{board}/{asic})")


if __name__ == "__main__":
    main()
