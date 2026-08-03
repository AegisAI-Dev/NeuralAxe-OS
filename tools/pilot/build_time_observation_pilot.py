#!/usr/bin/env python3
"""NeuralAxe OS — Gate B10.1 trusted-time OBSERVATION pilot build helper.

Builds a reproducible, owner-only pilot firmware from the exact committed HEAD
with exactly these five timed-session flags:

    CONFIG_NX_TIMED_SESSIONS                             = y
    CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE                = y
    CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS = y
    CONFIG_NX_TIMED_SESSIONS_EXECUTION                   = n
    CONFIG_NX_TIMED_SESSIONS_API                         = n

THE TRUSTED-TIME SOURCE IS NEVER STORED IN THIS REPOSITORY. It is supplied
only through the required environment variable NX_PILOT_NTP_SERVER, validated
with the SAME bounded rules the firmware applies (a port of the committed
Gate B10 pool_time_source_validate), written into a temporary sdkconfig
fragment OUTSIDE the tracked tree, and deleted when the build finishes.

This tool NEVER prints, logs, hashes into a filename, copies into the
repository or writes into the manifest the configured hostname. The manifest
records only `trustedTimeSourceConfigured: true`.

It performs no hardware access: no serial port, no USB, no COM enumeration, no
esptool invocation, no OTA upload, no device restart, no DNS lookup and no NTP
request. It builds, verifies and stages files. Flashing is owner-executed.

Usage:
    NX_PILOT_NTP_SERVER=<owner-chosen source> \\
      python tools/pilot/build_time_observation_pilot.py --out-dir <package dir>

    python tools/pilot/build_time_observation_pilot.py --check-source-only
    python tools/pilot/build_time_observation_pilot.py --print-flags
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCHEMA_VERSION = 1
ENV_VAR = "NX_PILOT_NTP_SERVER"
ARTIFACT_ROOT_ENV = "NX_PILOT_ARTIFACT_ROOT"

DEFAULT_IDF_IMAGE = "espressif/idf:v5.5.3"
CONTAINER_REPO = "/nx/repo"
CONTAINER_WORK = "/nx/work"

# The exact Gate B10.1 pilot posture. Order is significant only for display.
PILOT_FLAGS_ON = (
    "CONFIG_NX_TIMED_SESSIONS",
    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE",
    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS",
)
PILOT_FLAGS_OFF = (
    "CONFIG_NX_TIMED_SESSIONS_EXECUTION",
    "CONFIG_NX_TIMED_SESSIONS_API",
)

# Partition bounds from partitions.csv (factory/ota app slots 4 MiB, www 3 MiB).
APP_SLOT_BYTES = 4 * 1024 * 1024
WWW_SLOT_BYTES = 3 * 1024 * 1024

# Symbols that must NOT exist in a pilot image. Gate B7 execution and Gate B8
# API are compiled out, so no controlled apply, restore, mining grant, session
# creation command or command mailbox can be reachable.
FORBIDDEN_SYMBOL_PREFIXES = (
    "pool_session_execution",
    "pool_exec_",
    "nx_pool_execution_",
    "pool_session_api_",
    "pool_session_command",
    "nx_pool_session_api_",
    "pool_api_command_",
)
# Verified against real posture builds: this one symbol is compiled by
# CONFIG_NX_TIMED_SESSIONS alone. It is the standardized HTTP 409 conflict
# BODY WRITER used by the Gate B7 mutation fence in front of pool PATCH /
# restart / OTA — a refusal reporter, not an API route and not an executor.
# It must stay linked in a pilot build, because the fence is what keeps the
# owner's rollback OTA correctly gated rather than silently unguarded.
ALLOWED_DESPITE_PREFIX = frozenset({"nx_pool_session_api_send_conflict"})
# Symbols that MUST exist in a pilot image (the diagnostics are really linked).
REQUIRED_SYMBOL_PREFIXES = (
    "pool_pilot_",
    "pool_time_source_",
)


class PilotError(Exception):
    """A fatal, non-leaking failure. The message never contains the source."""


def fail(msg: str) -> None:
    raise PilotError(msg)


# --------------------------------------------------------------------------
# Bounded source validation — a faithful port of the committed Gate B10
# validator (components/pool_time/pool_time_source.c). It reads the candidate
# and reports only its SHAPE; it never returns, prints or stores the value.
# --------------------------------------------------------------------------

HOST_MAX = 63
LABEL_MAX = 63
LABELS_MAX = 16

LABEL_CHARS = re.compile(r"^[A-Za-z0-9-]+$")

# Verdicts mirror PoolTimeSourceState.
SRC_UNCONFIGURED = "TIME_SOURCE_UNCONFIGURED"
SRC_CONFIGURED = "TIME_SOURCE_CONFIGURED"
SRC_INVALID = "TIME_SOURCE_INVALID"


def _octet_valid(text: str) -> bool:
    if len(text) == 0 or len(text) > 3:
        return False
    if len(text) > 1 and text[0] == "0":
        return False
    if not text.isdigit():
        return False
    return int(text) <= 255


def validate_source(host: str | None) -> dict:
    """Return the bounded verdict {state, usable, length, labelCount, literalIpv4}.

    Deliberately mirrors the firmware rule set, including the rejections that
    exist for safety rather than syntax: no URL scheme/path/credential form,
    no whitespace, no control or non-ASCII byte, no single-label name, no IPv6
    literal and no leading-zero IPv4 octet.
    """
    reject = {"state": SRC_INVALID, "usable": False, "length": 0,
              "labelCount": 0, "literalIpv4": False}
    unconfigured = {"state": SRC_UNCONFIGURED, "usable": False, "length": 0,
                    "labelCount": 0, "literalIpv4": False}

    if host is None:
        return unconfigured
    if len(host) == 0:
        return unconfigured
    if len(host) > HOST_MAX:
        return reject
    # Character set: [A-Za-z0-9.-] only, ASCII only.
    for ch in host:
        if ch == ".":
            continue
        if ord(ch) > 127 or LABEL_CHARS.match(ch) is None:
            return reject

    labels = host.split(".")
    if len(labels) > LABELS_MAX:
        return reject
    for label in labels:
        if len(label) == 0 or len(label) > LABEL_MAX:
            return reject
        if label[0] == "-" or label[-1] == "-":
            return reject
    if len(labels) < 2:
        return reject

    numeric = [lb.isdigit() for lb in labels]
    if all(numeric):
        if len(labels) != 4:
            return reject
        for label in labels:
            if not _octet_valid(label):
                return reject
        return {"state": SRC_CONFIGURED, "usable": True, "length": len(host),
                "labelCount": len(labels), "literalIpv4": True}
    if numeric[-1]:
        return reject
    return {"state": SRC_CONFIGURED, "usable": True, "length": len(host),
            "labelCount": len(labels), "literalIpv4": False}


def read_source_from_env(env: dict | None = None) -> str:
    """Read and validate the owner-supplied source. Never returns it to a log."""
    env = os.environ if env is None else env
    if ENV_VAR not in env:
        fail(f"{ENV_VAR} is not set. The pilot trusted-time source must be supplied "
             "through the environment; it is never stored in this repository.")
    host = env[ENV_VAR]
    if host.strip() != host:
        # Reported without echoing the value.
        fail(f"{ENV_VAR} has leading or trailing whitespace — rejected before any build.")
    verdict = validate_source(host)
    if verdict["state"] != SRC_CONFIGURED:
        fail(f"{ENV_VAR} rejected by the bounded Gate B10 validator "
             f"({verdict['state']}). No build was started and the value was not printed.")
    return host


# --------------------------------------------------------------------------
# Repository state
# --------------------------------------------------------------------------

def git(repo: Path, *args: str) -> str:
    out = subprocess.run(["git", *args], cwd=str(repo), text=True,
                         capture_output=True)
    if out.returncode != 0:
        fail(f"git {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout.strip()


def repo_state(repo: Path) -> dict:
    status = git(repo, "status", "--porcelain")
    if status:
        fail("the working tree is not clean — a pilot artifact must be built from an "
             "exact committed HEAD. Commit or stash first.")
    describe = git(repo, "describe", "--tags", "--always", "--dirty")
    if "-dirty" in describe:
        fail(f"refusing to build a dirty revision ({describe}).")
    return {
        "commit": git(repo, "rev-parse", "HEAD"),
        "describe": describe,
        "branch": git(repo, "rev-parse", "--abbrev-ref", "HEAD"),
    }


def tracked_tree_digest(repo: Path) -> str:
    """A digest of every tracked path AND its blob id — proves the build left
    the tracked tree byte-identical without reading file contents twice."""
    listing = git(repo, "ls-files", "-s")
    return hashlib.sha256(listing.encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------
# Temporary pilot configuration (never inside the repository)
# --------------------------------------------------------------------------

def pilot_defaults_text(host: str) -> str:
    """The temporary sdkconfig fragment. The ONLY place the source appears."""
    lines = [
        "# NeuralAxe OS Gate B10.1 trusted-time observation pilot.",
        "# TEMPORARY, OWNER-LOCAL, NEVER COMMITTED. Deleted when the build ends.",
        "CONFIG_NX_TIMED_SESSIONS=y",
        f'CONFIG_NX_TIMED_SESSIONS_NTP_SERVER="{host}"',
        "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE=y",
        "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS=y",
        "# CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set",
        "# CONFIG_NX_TIMED_SESSIONS_API is not set",
    ]
    return "\n".join(lines) + "\n"


def write_pilot_defaults(work: Path, host: str) -> Path:
    path = work / "pilot.sdkconfig.defaults"
    path.write_text(pilot_defaults_text(host), encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass  # best effort on filesystems without POSIX modes
    return path


def shred(path: Path) -> None:
    """Overwrite then remove a file that held the source."""
    if not path.is_file():
        return
    try:
        size = path.stat().st_size
        with path.open("r+b") as handle:
            handle.write(b"\x00" * size)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError:
        pass
    try:
        path.unlink()
    except OSError:
        pass


# --------------------------------------------------------------------------
# Build execution
# --------------------------------------------------------------------------

def find_container_runtime() -> str:
    for runtime in ("docker", "podman"):
        if shutil.which(runtime):
            return runtime
    fail("neither docker nor podman was found in PATH, and IDF_PATH is not set. "
         "Run inside the ESP-IDF devcontainer or install a container runtime.")
    raise AssertionError("unreachable")


def build_command(repo_path: str, work_path: str) -> str:
    """The idf.py invocation, expressed with container-side paths."""
    defaults = f"{repo_path}/sdkconfig.defaults;{work_path}/pilot.sdkconfig.defaults"
    return (
        f"idf.py -B {work_path}/build "
        f"-DSDKCONFIG={work_path}/sdkconfig "
        f'-DSDKCONFIG_DEFAULTS="{defaults}" '
        f"set-target esp32s3 build"
    )


def run_build(repo: Path, work: Path, image: str, native: bool) -> None:
    if native:
        cmd = build_command(str(repo), str(work))
        print("building natively (IDF_PATH is set)")
        proc = subprocess.run(["bash", "-lc", cmd], cwd=str(repo),
                              env={**os.environ, "GITHUB_ACTIONS": "true"})
        if proc.returncode != 0:
            fail(f"pilot build failed (exit {proc.returncode})")
        return

    runtime = find_container_runtime()
    inner = (
        f"git config --global --add safe.directory {CONTAINER_REPO} && "
        "git config --global core.autocrlf true && "
        "git config --global core.filemode false && "
        + build_command(CONTAINER_REPO, CONTAINER_WORK)
    )
    cmd = [
        runtime, "run", "--rm",
        "-v", f"{repo}:{CONTAINER_REPO}:rw",
        "-v", f"{work}:{CONTAINER_WORK}:rw",
        "-w", CONTAINER_REPO,
        "-e", "GITHUB_ACTIONS=true",
        image,
        "bash", "-lc", inner,
    ]
    # The command line carries no hostname: the source lives only in the file.
    print(f"building in {runtime} image {image}")
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        fail(f"pilot build failed (exit {proc.returncode})")


# --------------------------------------------------------------------------
# Build verification
# --------------------------------------------------------------------------

def parse_sdkconfig_h(path: Path) -> dict:
    if not path.is_file():
        fail(f"generated sdkconfig.h not found: {path.name}")
    defines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"#define\s+(\S+)\s*(.*)$", line.strip())
        if m:
            defines[m.group(1)] = m.group(2).strip()
    return defines


def verify_build_config(defines: dict, host: str) -> dict:
    """Prove the exact pilot posture. Returns a hostname-free flag summary."""
    for flag in PILOT_FLAGS_ON:
        if defines.get(flag) != "1":
            fail(f"{flag} is not enabled in the generated configuration")
    for flag in PILOT_FLAGS_OFF:
        if flag in defines:
            fail(f"{flag} IS enabled in the generated configuration — refusing to "
                 "stage a pilot artifact that can execute or expose an API")

    key = "CONFIG_NX_TIMED_SESSIONS_NTP_SERVER"
    if key not in defines:
        fail("the trusted-time source is not present in the generated configuration")
    configured = defines[key].strip()
    if not (configured.startswith('"') and configured.endswith('"')):
        fail("the trusted-time source define has an unexpected form")
    if configured[1:-1] != host:
        # Compared, never printed.
        fail("the generated configuration does not carry the supplied trusted-time "
             "source — refusing to stage a mismatched artifact")

    summary = {flag: True for flag in PILOT_FLAGS_ON}
    summary.update({flag: False for flag in PILOT_FLAGS_OFF})
    summary["CONFIG_NX_TIMED_SESSIONS_NTP_SERVER"] = "<redacted: supplied via environment>"
    return summary


def list_symbols(elf: Path, work: Path, image: str, native: bool) -> list[str]:
    nm = "xtensa-esp32s3-elf-nm"
    if native:
        proc = subprocess.run([nm, str(elf)], text=True, capture_output=True)
    else:
        runtime = find_container_runtime()
        proc = subprocess.run(
            [runtime, "run", "--rm", "-v", f"{work}:{CONTAINER_WORK}:ro",
             image, nm, f"{CONTAINER_WORK}/{elf.relative_to(work).as_posix()}"],
            text=True, capture_output=True)
    if proc.returncode != 0:
        fail(f"symbol listing failed: {proc.stderr.strip()[:200]}")
    names = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if parts:
            names.append(parts[-1])
    return names


def verify_symbols(names: list[str]) -> dict:
    forbidden = sorted({n for n in names
                        if any(n.startswith(p) for p in FORBIDDEN_SYMBOL_PREFIXES)
                        and n not in ALLOWED_DESPITE_PREFIX})
    if forbidden:
        fail("the pilot image links forbidden execution/API symbols: "
             + ", ".join(forbidden[:10]))
    found = {}
    for prefix in REQUIRED_SYMBOL_PREFIXES:
        hits = sorted({n for n in names if n.startswith(prefix)})
        if not hits:
            fail(f"the pilot image links no '{prefix}*' symbol — the diagnostics "
                 "are not actually in this build")
        found[prefix] = len(hits)
    return found


def verify_sizes(build: Path) -> dict:
    app = build / "esp-miner.bin"
    www = build / "www.bin"
    for path in (app, www):
        if not path.is_file():
            fail(f"expected build output missing: {path.name}")
    app_size = app.stat().st_size
    www_size = www.stat().st_size
    if app_size >= APP_SLOT_BYTES:
        fail(f"esp-miner.bin ({app_size} B) does not fit a 4 MiB app slot")
    if www_size > WWW_SLOT_BYTES:
        fail(f"www.bin ({www_size} B) does not fit the 3 MiB www partition")
    return {
        "espMinerBinBytes": app_size,
        "espMinerBinFreeBytes": APP_SLOT_BYTES - app_size,
        "wwwBinBytes": www_size,
        "wwwBinFreeBytes": WWW_SLOT_BYTES - www_size,
    }


APP_DESC_MAGIC = b"\x32\x54\xcd\xab"


def read_app_desc_version(path: Path) -> str:
    with path.open("rb") as handle:
        header = handle.read(0x60)
    if len(header) < 0x60 or header[0x20:0x24] != APP_DESC_MAGIC:
        fail(f"{path.name}: esp_app_desc_t magic not found — not an ESP-IDF app image?")
    return header[0x30:0x50].split(b"\x00", 1)[0].decode("utf-8", errors="replace")


# --------------------------------------------------------------------------
# Web build and release-pair identity
#
# WHY THIS EXISTS. main/CMakeLists.txt has two web paths. With
# GITHUB_ACTIONS=true it takes the "Web ui will be prebuilt" branch and packs
# whatever already sits in main/http_server/axe-os/dist — running NO npm build
# and NO generate-version.js. The firmware still takes its identity from
# `git describe` at compile time, so a leftover dist produces an image whose
# esp-miner.bin says one revision and whose www.bin says an older one. A real
# device then reports BOOT PAIR MISMATCH, which is exactly what happened to the
# first B10.2 package (firmware v2.14.2-69-g3120c1cc, web v2.14.2-63-ga7793af).
#
# The corrected pipeline therefore (a) deletes any existing web output, (b)
# builds the production frontend from the working tree at the verified clean
# HEAD, (c) checks version.txt on disk, and (d) — the real gate — extracts the
# revision EMBEDDED IN THE GENERATED www.bin and requires it to equal both the
# firmware app_desc and `git describe`.
# --------------------------------------------------------------------------

WEB_SRC_REL = "main/http_server/axe-os"
WEB_DIST_REL = "main/http_server/axe-os/dist"
WEB_DIST_INNER = "main/http_server/axe-os/dist/axe-os"

# Compressed web assets hide their strings, so a www SPIFFS image contains
# exactly one plain-text git-describe match: the version.txt content. This MUST
# stay identical to tools/release/export_release.py REVISION_PATTERN — a test
# asserts the two are the same string.
WWW_REVISION_PATTERN = re.compile(
    rb"v\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]{7,12})?(?:-dirty)?")


def remove_stale_web_output(repo: Path) -> None:
    """Delete any existing web build output.

    A package must never be able to reuse an image it did not just build, so
    the directory is removed rather than reused, and its absence is verified.
    """
    dist = repo / WEB_DIST_REL
    if dist.exists():
        shutil.rmtree(dist, ignore_errors=True)
    if dist.exists():
        fail("could not remove the existing web build output; refusing to package "
             "a possibly stale www image")


def build_frontend(repo: Path, describe: str, npm_install: str = "ci") -> str:
    """Build the production frontend fresh and return its version.txt content."""
    remove_stale_web_output(repo)

    npm = shutil.which("npm") or shutil.which("npm.cmd")
    if npm is None:
        fail("npm was not found in PATH; the pilot package requires a freshly built "
             "web image and will not reuse an existing one")

    web = repo / WEB_SRC_REL
    if not web.is_dir():
        fail("the frontend source directory is missing")

    if npm_install != "skip":
        print(f"building the web UI: npm {npm_install}")
        if subprocess.run([npm, npm_install], cwd=str(web)).returncode != 0:
            fail(f"npm {npm_install} failed")
    print("building the web UI: npm run build (production)")
    if subprocess.run([npm, "run", "build"], cwd=str(web)).returncode != 0:
        fail("the production web build failed")

    version_txt = repo / WEB_DIST_INNER / "version.txt"
    if not version_txt.is_file():
        fail("the web build produced no version.txt; refusing to package an image "
             "with no identity")
    built = version_txt.read_text(encoding="utf-8").strip()
    if not built:
        fail("the web build produced an empty version.txt")
    if "-dirty" in built:
        fail(f"the web build embeds a dirty identity ({built})")
    if built != describe:
        fail(f"the freshly built web revision '{built}' does not match HEAD "
             f"'{describe}' — refusing to package a mismatched pair")
    return built


def read_embedded_www_revision(www_bin: Path) -> str:
    """Extract the revision EMBEDDED INSIDE a generated www SPIFFS image.

    This is the authoritative web identity — the same version.txt the firmware
    serves as axeOSVersion — as opposed to anything on the build host, which can
    be stale or newer than what was actually packed.
    """
    if not www_bin.is_file():
        fail("the generated web image is missing")
    data = www_bin.read_bytes()
    matches = sorted({m.group(0).decode("ascii")
                      for m in WWW_REVISION_PATTERN.finditer(data)})
    if not matches:
        fail("no embedded web revision found in the generated www image — is "
             "version.txt missing from it?")
    if len(matches) > 1:
        fail(f"the generated www image embeds multiple distinct revisions "
             f"({matches}) — ambiguous web identity")
    return matches[0]


def verify_release_pair(build: Path, describe: str) -> dict:
    """THE gate. Firmware and web must both be the expected revision.

    Checksums are deliberately NOT part of this: a stale www.bin has a perfectly
    valid SHA-256 of the wrong content. Only the embedded identities can catch
    the mismatch that a device reports as BOOT PAIR MISMATCH.
    """
    fw = read_app_desc_version(build / "esp-miner.bin")
    web = read_embedded_www_revision(build / "www.bin")

    if "-dirty" in fw:
        fail(f"the built firmware embeds a dirty identity ({fw})")
    if "-dirty" in web:
        fail(f"the generated web image embeds a dirty identity ({web})")
    if fw != web:
        fail(f"BOOT PAIR MISMATCH would ship: firmware '{fw}' != web '{web}'")
    if fw != describe:
        fail(f"the built firmware '{fw}' does not match HEAD '{describe}'")
    if web != describe:
        fail(f"the generated web image '{web}' does not match HEAD '{describe}'")
    return {"firmwareRevision": fw, "webRevision": web}


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------

def build_manifest(state: dict, flags: dict, sizes: dict, symbols: dict,
                   fw_version: str, artifacts: list[dict],
                   build_timestamp: str, pair: dict | None = None) -> dict:
    """The pilot manifest. It records that a source WAS configured and nothing
    about which one."""
    pair = pair or {}
    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "package": "NeuralAxe OS Gate B10.1 trusted-time observation pilot",
        "productName": "NeuralAxe OS",
        "productVersion": "0.1.0-dev",
        "buildChannel": "development",
        "vendor": "NeuralShield",
        "targetDevice": "Gamma",
        "targetBoard": "601",
        "targetAsic": "BM1370",
        "gitCommit": state["commit"],
        "gitDescribe": state["describe"],
        "firmwareVersion": fw_version,
        # The two EMBEDDED identities, extracted from the generated binaries —
        # the pair a device compares at boot. A matching SHA-256 says nothing
        # about them, so both are recorded explicitly.
        "firmwareRevision": pair.get("firmwareRevision", fw_version),
        "webRevision": pair.get("webRevision", "<not verified>"),
        "releasePairVerified": bool(pair) and
                               pair.get("firmwareRevision") == pair.get("webRevision") ==
                               state["describe"],
        "webImageFreshlyBuilt": True,
        "buildTimestampUtc": build_timestamp,
        "trustedTimeSourceConfigured": True,
        "trustedTimeSourceValue": "<never recorded>",
        "flags": flags,
        "sizes": sizes,
        "linkedSymbolGroups": symbols,
        "executionReachable": False,
        "apiReachable": False,
        "canCreateTimedSession": False,
        "canMutatePoolConfiguration": False,
        "flashMethod": (
            "AxeOS Update page (or POST /api/system/OTAWWW then POST /api/system/OTA). "
            "Writes the www partition and the inactive OTA slot + otadata only; NVS "
            "settings (Wi-Fi, pools, tuning, fan) are preserved. Owner-executed."
        ),
        "rollbackMethod": (
            "Re-upload the previous known-good www.bin and esp-miner.bin through the "
            "same Update page. No factory/merged image is included in this package: a "
            "full flash at 0x0 would ERASE NVS and is not a pilot rollback path."
        ),
        "factoryImageIncluded": False,
        "privateArtifactsExcluded": True,
        "artifacts": artifacts,
    }
    return manifest


def resolve_out_dir(arg_out_dir: str | None, env: dict | None = None) -> Path:
    """Where the package is staged. ALWAYS caller-supplied.

    No location is hardcoded: this tool has no idea where the owner keeps
    build artifacts, and baking a path in would leak one owner's directory
    layout into every checkout. Precedence: --out-dir, then
    NX_PILOT_ARTIFACT_ROOT. Neither present is a hard error.
    """
    env = os.environ if env is None else env
    chosen = arg_out_dir or env.get(ARTIFACT_ROOT_ENV) or ""
    if not chosen.strip():
        fail(f"no output directory: pass --out-dir <external-artifact-directory> or set "
             f"{ARTIFACT_ROOT_ENV}. The package location is never hardcoded.")
    return Path(chosen).expanduser().resolve()


def stage_artifacts(build: Path, out_dir: Path, prefix: str) -> list[dict]:
    plan = [
        (build / "esp-miner.bin", f"{prefix}-pilot-ota.bin", "ota-application",
         "AxeOS Update page / POST /api/system/OTA (inactive OTA slot + otadata)",
         True, False),
        (build / "www.bin", f"{prefix}-pilot-www.bin", "www-update",
         "AxeOS Update page / POST /api/system/OTAWWW (www partition only)",
         True, False),
    ]
    out_dir.mkdir(parents=True, exist_ok=True)
    artifacts = []
    for src, name, kind, method, preserved, destructive in plan:
        dst = out_dir / name
        shutil.copyfile(src, dst)
        artifacts.append({
            "filename": name,
            "artifactType": kind,
            "sizeBytes": dst.stat().st_size,
            "sha256": sha256_of(dst),
            "flashMethod": method,
            "settingsPreserved": preserved,
            "destructive": destructive,
        })
        print(f"staged {name} ({dst.stat().st_size} bytes)")
    return artifacts


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-root", default=None, help="repository root (default: git toplevel)")
    parser.add_argument("--out-dir", default=None,
                        help="<external-artifact-directory> to stage the pilot package into; "
                             f"falls back to ${ARTIFACT_ROOT_ENV}. Never hardcoded.")
    parser.add_argument("--work-dir", default=None,
                        help="temporary build root (default: a system temp dir OUTSIDE the repo)")
    parser.add_argument("--idf-image", default=DEFAULT_IDF_IMAGE, help="ESP-IDF container image")
    parser.add_argument("--build-timestamp", default=None,
                        help="ISO-8601 UTC build timestamp recorded in the manifest")
    parser.add_argument("--check-source-only", action="store_true",
                        help="validate NX_PILOT_NTP_SERVER and exit; builds nothing")
    parser.add_argument("--print-flags", action="store_true",
                        help="print the exact pilot flag posture and exit")
    parser.add_argument("--keep-work-dir", action="store_true",
                        help="keep the temporary build tree (the config fragment is still shredded)")
    parser.add_argument("--npm-install", choices=("ci", "install", "skip"), default="ci",
                        help="how to prepare node_modules before the fresh web build. "
                             "'ci' (default) installs exactly package-lock.json; 'skip' "
                             "reuses an existing node_modules for an offline rebuild. It "
                             "never skips the web BUILD itself.")
    args = parser.parse_args()

    if args.print_flags:
        for flag in PILOT_FLAGS_ON:
            print(f"{flag}=y")
        print('CONFIG_NX_TIMED_SESSIONS_NTP_SERVER="<supplied via ' + ENV_VAR + '>"')
        for flag in PILOT_FLAGS_OFF:
            print(f"{flag}=n")
        return 0

    repo = Path(args.repo_root).resolve() if args.repo_root else Path(
        subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                cwd=str(Path(__file__).resolve().parent),
                                text=True).strip()).resolve()

    # 1 — the source, before anything else happens.
    host = read_source_from_env()
    verdict = validate_source(host)
    print(f"trusted-time source: accepted ({verdict['state']}, "
          f"{'IPv4 literal' if verdict['literalIpv4'] else 'DNS name'}, "
          f"{verdict['labelCount']} labels) — value not printed")

    if args.check_source_only:
        return 0

    # The staging location is always caller-supplied; nothing is baked in.
    out_dir = resolve_out_dir(args.out_dir)

    # 2 — the exact committed HEAD.
    state = repo_state(repo)
    before_digest = tracked_tree_digest(repo)
    print(f"building from {state['describe']} ({state['commit'][:12]}) on {state['branch']}")

    native = bool(os.environ.get("IDF_PATH"))
    work = Path(args.work_dir).resolve() if args.work_dir else Path(
        tempfile.mkdtemp(prefix="nx-b101-pilot-")).resolve()
    try:
        work.relative_to(repo)
    except ValueError:
        pass
    else:
        fail("the temporary build tree must live OUTSIDE the repository so no "
             "configuration carrying the trusted-time source can ever be committed")

    config_path = write_pilot_defaults(work, host)
    try:
        # THE web stage, before the firmware build. The firmware build runs with
        # GITHUB_ACTIONS=true, which packs main/http_server/axe-os/dist verbatim
        # without running npm — so that directory must have been produced by THIS
        # run, from THIS commit, or a stale image ships and the device reports
        # BOOT PAIR MISMATCH.
        web_revision = build_frontend(repo, state["describe"], args.npm_install)
        print(f"web UI built fresh at {web_revision}")

        run_build(repo, work, args.idf_image, native)

        build = work / "build"
        flags = verify_build_config(parse_sdkconfig_h(build / "config" / "sdkconfig.h"), host)
        symbols = verify_symbols(list_symbols(build / "esp-miner.elf", work,
                                              args.idf_image, native))
        sizes = verify_sizes(build)
        fw_version = read_app_desc_version(build / "esp-miner.bin")
        if "-dirty" in fw_version:
            fail(f"the built image embeds a dirty identity ({fw_version})")

        # THE release-pair gate, BEFORE anything is staged.
        pair = verify_release_pair(build, state["describe"])
        print(f"release pair verified: firmware {pair['firmwareRevision']} == "
              f"web {pair['webRevision']} == HEAD {state['describe']}")

        prefix = f"NeuralAxe-OS-v0.1.0-dev-Gamma-601-{state['describe']}"
        artifacts = stage_artifacts(build, out_dir, prefix)
        manifest = build_manifest(state, flags, sizes, symbols, fw_version, artifacts,
                                  args.build_timestamp or "<not recorded>", pair)

        manifest_path = out_dir / f"{prefix}-pilot-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        sums = "\n".join(f"{a['sha256']}  {a['filename']}" for a in artifacts) + "\n"
        (out_dir / f"{prefix}-pilot-SHA256SUMS.txt").write_text(sums, encoding="utf-8")

        blob = manifest_path.read_text(encoding="utf-8")
        if host in blob:
            fail("the manifest would carry the trusted-time source — refusing to write it")
        # The manifest describes an artifact, not a machine: it must never
        # record where this build happened to run.
        for leak in (str(repo), str(work), str(out_dir)):
            if leak and leak in blob:
                fail("the manifest would record a local filesystem path — refusing to write it")
        print(f"wrote {manifest_path.name}")
    finally:
        shred(config_path)
        if not args.keep_work_dir and args.work_dir is None:
            shutil.rmtree(work, ignore_errors=True)
        # Remove the web build output we produced: leaving it behind is exactly
        # how a later build packs a stale image once HEAD moves.
        remove_stale_web_output(repo)

    after_digest = tracked_tree_digest(repo)
    if before_digest != after_digest:
        fail("the build modified tracked files — the pilot build must leave the "
             "repository byte-identical")
    if git(repo, "status", "--porcelain"):
        fail("the build left the working tree dirty")
    print("tracked tree unchanged; temporary configuration removed")
    print("PILOT BUILD OK — no hardware was touched and no flash was performed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PilotError as exc:
        print(f"PILOT BUILD FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
