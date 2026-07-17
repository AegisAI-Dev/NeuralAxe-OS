#!/usr/bin/env python3
"""NeuralAxe OS release export (staging) tool.

Copies internal build outputs into deterministic, board-identified release
filenames, computes sizes and SHA-256 hashes, and generates the release
manifest and SHA256SUMS. Internal build outputs are never renamed in place,
so upstream tooling keeps working.

The canonical identity source is main/neuralaxe_identity.h; the frontend
constant (src/app/neuralaxe.ts) is cross-checked against it and the export
FAILS if they diverge. A build whose version string contains "-dirty" is
rejected.

This tool only stages local files. It never uploads, never publishes, never
touches hardware, and refuses to include anything that looks like a private
flash dump.
"""

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

SCHEMA_VERSION = 1

# Patterns that identify private recovery dumps; these must never be exported.
FORBIDDEN_NAME_PATTERNS = re.compile(r"(flashdump|flash-dump|nvs[-_]?dump|nvs[-_]?backup|tch)", re.IGNORECASE)

IDENTITY_KEYS = {
    "NEURALAXE_PRODUCT_NAME": "productName",
    "NEURALAXE_PRODUCT_VERSION": "productVersion",
    "NEURALAXE_BUILD_CHANNEL": "buildChannel",
    "NEURALAXE_VENDOR": "vendor",
    "NEURALAXE_UPSTREAM_PROJECT": "upstreamProject",
    "NEURALAXE_UPSTREAM_VERSION": "upstreamVersion",
    "NEURALAXE_TARGET_BOARD": "targetBoard",
    "NEURALAXE_TARGET_DEVICE": "targetDevice",
    "NEURALAXE_TARGET_ASIC": "targetAsic",
}


def fail(msg: str) -> None:
    print(f"EXPORT FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_identity_header(repo_root: Path) -> dict:
    header = repo_root / "main" / "neuralaxe_identity.h"
    if not header.is_file():
        fail(f"identity header not found: {header}")
    text = header.read_text(encoding="utf-8")
    identity = {}
    for macro, key in IDENTITY_KEYS.items():
        m = re.search(rf'#define\s+{macro}\s+"([^"]*)"', text)
        if not m:
            fail(f"{macro} missing from neuralaxe_identity.h")
        identity[key] = m.group(1)
    return identity


def cross_check_frontend(repo_root: Path, identity: dict) -> None:
    ts = repo_root / "main" / "http_server" / "axe-os" / "src" / "app" / "neuralaxe.ts"
    if not ts.is_file():
        fail(f"frontend identity constant not found: {ts}")
    text = ts.read_text(encoding="utf-8")
    for key, value in identity.items():
        m = re.search(rf"{key}:\s*'([^']*)'", text)
        if not m:
            fail(f"frontend neuralaxe.ts missing key: {key}")
        if m.group(1) != value:
            fail(f"identity divergence for '{key}': firmware header='{value}' frontend='{m.group(1)}'")


APP_DESC_MAGIC = b"\x32\x54\xcd\xab"  # esp_app_desc_t magic_word 0xABCD5432 (LE)

# A git-describe style build revision as embedded in version.txt / app_desc.
# Compressed web assets hide their strings, so the raw SPIFFS image contains
# exactly one plain-text match: the version.txt content (verified against the
# real Phase 2D.1 and 2E www.bin artifacts).
REVISION_PATTERN = re.compile(rb"v\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]{7,12})?(?:-dirty)?")


def read_www_revision(www_bin: Path) -> str:
    """Extract the web revision embedded INSIDE a www.bin SPIFFS image.

    This is the authoritative identity of the web artifact — the same
    version.txt the firmware reads at boot to report axeOSVersion — as opposed
    to the host's dist/version.txt, which can be stale or newer than the
    packed image when build steps run out of order.
    """
    data = www_bin.read_bytes()
    matches = sorted({m.group(0).decode("ascii") for m in REVISION_PATTERN.finditer(data)})
    if len(matches) == 0:
        fail(f"{www_bin.name}: no embedded web revision found — is version.txt missing from the image?")
    if len(matches) > 1:
        fail(f"{www_bin.name}: multiple distinct revision strings embedded ({matches}) — ambiguous web identity")
    return matches[0]


def check_release_pair(fw_bin: Path, www_bin: Path) -> tuple[str, str]:
    """RC gate: the firmware and web artifacts of one release pair must carry
    the same clean source revision. Returns (firmware_revision, web_revision)."""
    fw_revision = check_app_binary(fw_bin)
    web_revision = read_www_revision(www_bin)
    print(f"embedded web revision of {www_bin.name}: {web_revision}")
    if "-dirty" in web_revision:
        fail(f"{www_bin.name} embeds a dirty web identity ('{web_revision}'). Rebuild from a clean committed tree.")
    if fw_revision != web_revision:
        fail(
            f"release pair mismatch: firmware '{fw_revision}' != web '{web_revision}'. "
            "The firmware and web artifacts must be built from the same commit in one "
            "pipeline run (a stale dist/version.txt or out-of-order build produces this)."
        )
    return fw_revision, web_revision


def read_app_desc_version(bin_path: Path) -> str:
    """Read the version string embedded in an ESP-IDF application binary.

    Layout: esp_image_header_t (24B) + esp_image_segment_header_t (8B) put
    esp_app_desc_t at file offset 0x20; its version[32] field sits at +0x10,
    i.e. file offset 0x30. This is the string a live device reports, so it is
    the authoritative build identity of esp-miner.bin — NOT version.txt.
    """
    with bin_path.open("rb") as f:
        header = f.read(0x60)
    if len(header) < 0x60 or header[0x20:0x24] != APP_DESC_MAGIC:
        fail(f"{bin_path.name}: esp_app_desc_t magic not found — not an ESP-IDF app image?")
    return header[0x30:0x50].split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def check_app_binary(bin_path: Path, expected_revision: str | None = None) -> str:
    version = read_app_desc_version(bin_path)
    print(f"app_desc version of {bin_path.name}: {version}")
    if "-dirty" in version:
        fail(
            f"{bin_path.name} embeds a dirty build identity ('{version}'). "
            "Rebuild from a clean committed tree (in the Linux build container set "
            "'git config --global core.autocrlf true' and 'core.filemode false' so a "
            "CRLF Windows checkout is not misread as modified)."
        )
    if expected_revision and version != expected_revision:
        fail(f"{bin_path.name} app_desc version '{version}' does not match frontend sourceRevision '{expected_revision}'")
    return version


def read_source_revision(repo_root: Path) -> str:
    version_txt = repo_root / "main" / "http_server" / "axe-os" / "dist" / "axe-os" / "version.txt"
    if not version_txt.is_file():
        fail(f"version.txt not found (build the frontend first): {version_txt}")
    revision = version_txt.read_text(encoding="utf-8").strip()
    if not revision:
        fail("version.txt is empty")
    if "-dirty" in revision:
        fail(f"refusing to export a dirty build: sourceRevision='{revision}'")
    return revision


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".", help="repository root")
    parser.add_argument("--merged-bin", help="path to the merged/factory image")
    parser.add_argument("--out", help="output directory for release files")
    parser.add_argument("--check-bin", help="standalone mode: verify the app_desc identity of one ESP-IDF app binary and exit")
    parser.add_argument("--check-pair", nargs=2, metavar=("ESP_MINER_BIN", "WWW_BIN"),
                        help="standalone mode: verify a firmware/web release pair carries the same clean revision and exit")
    parser.add_argument("--screenshot-scan",
                        help="RC gate: path to a screenshot privacy-scan report that must state 'PRIVACY SCAN: CLEAN'")
    args = parser.parse_args()

    if args.check_bin:
        check_app_binary(Path(args.check_bin).resolve())
        print("APP DESC OK")
        return

    if args.check_pair:
        fw_revision, _ = check_release_pair(Path(args.check_pair[0]).resolve(), Path(args.check_pair[1]).resolve())
        print(f"PAIR OK: firmware and web both at {fw_revision}")
        return

    if not args.merged_bin or not args.out:
        parser.error("--merged-bin and --out are required unless --check-bin/--check-pair is used")

    if args.screenshot_scan:
        scan_report = Path(args.screenshot_scan).resolve()
        if not scan_report.is_file():
            fail(f"screenshot privacy-scan report not found: {scan_report}")
        if "PRIVACY SCAN: CLEAN" not in scan_report.read_text(encoding="utf-8", errors="replace"):
            fail(f"screenshot privacy scan is not clean: {scan_report}")
        print(f"screenshot privacy scan verified: {scan_report.name}")

    repo_root = Path(args.repo_root).resolve()
    merged_bin = Path(args.merged_bin).resolve()
    out_dir = Path(args.out).resolve()

    identity = parse_identity_header(repo_root)
    cross_check_frontend(repo_root, identity)
    source_revision = read_source_revision(repo_root)

    if identity["targetBoard"] != "601" or identity["targetDevice"] != "Gamma" or identity["targetAsic"] != "BM1370":
        fail(f"unsupported target in identity header: {identity}")

    prefix = (
        f"NeuralAxe-OS-v{identity['productVersion']}-"
        f"{identity['targetDevice']}-{identity['targetBoard']}"
    )

    build_dir = repo_root / "build"
    sources = {
        "www": build_dir / "www.bin",
        "ota": build_dir / "esp-miner.bin",
        "factory": merged_bin,
        "config": repo_root / "config-601.cvs",
    }
    for label, path in sources.items():
        if not path.is_file():
            fail(f"missing input for '{label}': {path}")
        if path.stat().st_size == 0:
            fail(f"zero-size input for '{label}': {path}")
        if FORBIDDEN_NAME_PATTERNS.search(path.name):
            fail(f"input looks like a private dump and must not be exported: {path.name}")

    # The device-reported firmware identity must be clean and match the
    # frontend revision (both derive from git describe of the same tree).
    check_app_binary(sources["ota"], expected_revision=source_revision)

    # RC gate: firmware and the revision embedded INSIDE www.bin must agree —
    # dist/version.txt alone cannot prove what was actually packed.
    firmware_revision, web_revision = check_release_pair(sources["ota"], sources["www"])
    if web_revision != source_revision:
        fail(
            f"stale build order detected: dist/version.txt says '{source_revision}' but "
            f"the staged www.bin embeds '{web_revision}'. Rebuild so the packed image and "
            "version.txt come from the same pipeline run."
        )

    out_dir.mkdir(parents=True, exist_ok=True)

    artifacts = [
        {
            "src": sources["www"],
            "filename": f"{prefix}-www.bin",
            "artifactType": "www-update",
            "flashMethod": "AxeOS Update page or POST /api/system/OTAWWW (writes only the 'www' data partition)",
            "settingsPreserved": True,
            "destructive": False,
            "minimumCompatible": f"upstream {identity['upstreamVersion']} partition layout (www @0x410000, 3MiB)",
        },
        {
            "src": sources["ota"],
            "filename": f"{prefix}-ota.bin",
            "artifactType": "ota-application",
            "flashMethod": "AxeOS Update page or POST /api/system/OTA (writes inactive OTA slot + otadata only)",
            "settingsPreserved": True,
            "destructive": False,
            "minimumCompatible": f"upstream {identity['upstreamVersion']} partition layout (4MiB app slots)",
        },
        {
            "src": sources["factory"],
            "filename": f"{prefix}-factory.bin",
            "artifactType": "factory-image",
            "flashMethod": "esptool/bitaxetool full flash at offset 0x0 (owner-executed; NOT an OTA upload)",
            "settingsPreserved": False,
            "destructive": True,
            "minimumCompatible": "ESP32-S3 N16R8 (16MB flash) board 601 only",
        },
        {
            "src": sources["config"],
            "filename": "config-601.cvs",
            "artifactType": "board-provisioning-config",
            "flashMethod": "bitaxetool --config (generates NVS provisioning; replaces NVS contents)",
            "settingsPreserved": False,
            "destructive": True,
            "minimumCompatible": "board 601 / Gamma / BM1370 only",
        },
    ]

    manifest_artifacts = []
    sums_lines = []
    for a in artifacts:
        dst = out_dir / a["filename"]
        shutil.copyfile(a["src"], dst)
        size = dst.stat().st_size
        digest = sha256_of(dst)
        manifest_artifacts.append({
            "filename": a["filename"],
            "artifactType": a["artifactType"],
            "sizeBytes": size,
            "sha256": digest,
            "flashMethod": a["flashMethod"],
            "settingsPreserved": a["settingsPreserved"],
            "destructive": a["destructive"],
            "minimumCompatible": a["minimumCompatible"],
        })
        sums_lines.append(f"{digest}  {a['filename']}")
        print(f"exported {a['filename']}  ({size} bytes)")

    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "productName": identity["productName"],
        "productVersion": identity["productVersion"],
        "buildChannel": identity["buildChannel"],
        "vendor": identity["vendor"],
        "sourceRevision": source_revision,
        "firmwareRevision": firmware_revision,
        "webRevision": web_revision,
        "upstreamProject": identity["upstreamProject"],
        "upstreamVersion": identity["upstreamVersion"],
        "targetDevice": identity["targetDevice"],
        "targetBoard": identity["targetBoard"],
        "targetAsic": identity["targetAsic"],
        "supportedBoards": [identity["targetBoard"]],
        "license": "GPL-3.0",
        "attribution": "NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects.",
        "privateArtifactsExcluded": True,
        "artifacts": manifest_artifacts,
    }

    manifest_path = out_dir / f"{prefix}-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    sums_path = out_dir / f"{prefix}-SHA256SUMS.txt"
    sums_path.write_text("\n".join(sums_lines) + "\n", encoding="utf-8")

    print(f"wrote {manifest_path.name}")
    print(f"wrote {sums_path.name}")
    print(f"EXPORT OK: {len(manifest_artifacts)} artifacts, sourceRevision={source_revision}")


if __name__ == "__main__":
    main()
