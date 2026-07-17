#!/usr/bin/env python3
"""NeuralAxe OS release-gate test battery (Phase 2F).

Self-contained (no pytest): exercises the version-pair and RC gates with
synthetic artifacts. Exit 0 = all gates behave as specified.

Covered:
- matching clean firmware/web pair passes;
- mismatched firmware/web pair is rejected;
- dirty firmware or dirty web identity is rejected;
- www image without an embedded revision is rejected;
- www image with multiple distinct revisions is rejected;
- stale dist/version.txt vs packed image is detected (function-level);
- manifest validator rejects pair mismatch, dirty revisions and local paths;
- manifest validator accepts a coherent pair.
"""

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

spec = importlib.util.spec_from_file_location("export_release", HERE / "export_release.py")
export_release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export_release)

PASS = 0
FAIL = 0


def report(name: str, ok: bool, detail: str = "") -> None:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name} {detail}")


def make_app_bin(path: Path, version: str) -> None:
    """Minimal ESP-IDF-like app image: magic at 0x20, version[32] at 0x30."""
    data = bytearray(0x100)
    data[0x20:0x24] = b"\x32\x54\xcd\xab"
    v = version.encode("ascii")
    data[0x30:0x30 + len(v)] = v
    path.write_bytes(bytes(data))


def make_www_bin(path: Path, *versions: str, filler: bytes = b"\x00") -> None:
    """Synthetic SPIFFS-like blob embedding the given plain revision strings."""
    blob = bytearray(filler * 512)
    offset = 64
    for v in versions:
        enc = v.encode("ascii")
        blob[offset:offset + len(enc)] = enc
        offset += len(enc) + 32
    path.write_bytes(bytes(blob))


def expect_exit(fn, *args):
    """Run a gate function; return (raised_SystemExit, return_value)."""
    try:
        return False, fn(*args)
    except SystemExit:
        return True, None


def main() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="nx-release-gates-"))
    rev = "v2.14.2-13-g388287da"
    other = "v2.14.2-9-gc630e1a"

    print("== pair gates (export_release.check_release_pair) ==")

    fw = tmp / "fw.bin"; make_app_bin(fw, rev)
    www = tmp / "www.bin"; make_www_bin(www, rev)
    exited, result = expect_exit(export_release.check_release_pair, fw, www)
    report("matching clean pair passes", not exited and result == (rev, rev))

    www_mismatch = tmp / "www-mismatch.bin"; make_www_bin(www_mismatch, other)
    exited, _ = expect_exit(export_release.check_release_pair, fw, www_mismatch)
    report("mismatched firmware/web pair rejected", exited)

    fw_dirty = tmp / "fw-dirty.bin"; make_app_bin(fw_dirty, rev + "-dirty")
    exited, _ = expect_exit(export_release.check_release_pair, fw_dirty, www)
    report("dirty firmware identity rejected", exited)

    www_dirty = tmp / "www-dirty.bin"; make_www_bin(www_dirty, rev + "-dirty")
    exited, _ = expect_exit(export_release.check_release_pair, fw, www_dirty)
    report("dirty web identity rejected", exited)

    www_none = tmp / "www-none.bin"; make_www_bin(www_none)
    exited, _ = expect_exit(export_release.check_release_pair, fw, www_none)
    report("www image without embedded revision rejected", exited)

    www_multi = tmp / "www-multi.bin"; make_www_bin(www_multi, rev, other)
    exited, _ = expect_exit(export_release.check_release_pair, fw, www_multi)
    report("www image with multiple distinct revisions rejected", exited)

    print("== stale build-order detection (read_www_revision vs version.txt) ==")
    # The export flow fails when dist/version.txt disagrees with the packed
    # image; the primitive it relies on must read the PACKED identity.
    embedded = export_release.read_www_revision(www_mismatch)
    report("packed web identity read from artifact, not from dist", embedded == other,
           f"got {embedded}")
    stale_detected = embedded != rev
    report("stale dist/version.txt (rev != packed) is detectable", stale_detected)

    print("== manifest validator gates ==")

    def run_validator(manifest: dict, directory: Path) -> subprocess.CompletedProcess:
        mpath = directory / "m.json"
        mpath.write_text(json.dumps(manifest), encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(HERE / "validate_manifest.py"), "--manifest", str(mpath)],
            capture_output=True, text=True)

    vdir = tmp / "validator"; vdir.mkdir()
    art_name = "NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin"
    art_path = vdir / art_name
    art_path.write_bytes(b"synthetic artifact")
    art_entry = {
        "filename": art_name,
        "artifactType": "www-update",
        "sizeBytes": art_path.stat().st_size,
        "sha256": export_release.sha256_of(art_path),
        "flashMethod": "Update page",
        "settingsPreserved": True,
        "destructive": False,
    }

    def base_manifest(**overrides) -> dict:
        m = {
            "schemaVersion": 1,
            "productName": "NeuralAxe OS",
            "productVersion": "0.1.0-dev",
            "buildChannel": "development",
            "vendor": "NeuralShield",
            "sourceRevision": rev,
            "firmwareRevision": rev,
            "webRevision": rev,
            "upstreamProject": "ESP-Miner / AxeOS",
            "upstreamVersion": "v2.14.2",
            "targetDevice": "Gamma",
            "targetBoard": "601",
            "targetAsic": "BM1370",
            "supportedBoards": ["601"],
            "artifacts": [art_entry],
        }
        m.update(overrides)
        return m

    r = run_validator(base_manifest(), vdir)
    report("coherent manifest pair accepted", r.returncode == 0, r.stderr.strip()[:200])

    r = run_validator(base_manifest(webRevision=other), vdir)
    report("manifest pair mismatch rejected", r.returncode != 0)

    r = run_validator(base_manifest(firmwareRevision=rev + "-dirty", sourceRevision=rev + "-dirty"), vdir)
    report("manifest dirty firmwareRevision rejected", r.returncode != 0)

    r = run_validator({k: v for k, v in base_manifest().items() if k != "webRevision"}, vdir)
    report("manifest missing webRevision rejected", r.returncode != 0)

    r = run_validator(base_manifest(vendor="D:\\Companys\\Neuralshield"), vdir)
    report("manifest local path rejected", r.returncode != 0)

    print(f"== battery result: {PASS} passed, {FAIL} failed ==")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
