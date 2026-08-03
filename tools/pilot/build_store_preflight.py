#!/usr/bin/env python3
"""NeuralAxe OS — Gate B10.2 READ-ONLY store-preflight build helper.

Builds a reproducible, generic preflight firmware from the exact committed
HEAD with exactly this posture:

    CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT                 = y
    CONFIG_NX_TIMED_SESSIONS_EXECUTION                       = n
    CONFIG_NX_TIMED_SESSIONS_API                             = n
    CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE                    = n
    CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS  = n

Unlike the Gate B10.1 trusted-time pilot build, this artifact needs NO private
input: it selects no NTP source, resolves nothing and contacts nothing. It
boots, performs exactly one read-only classification of the timed-session NVS
namespace, prints one bounded token, and otherwise behaves like the normal
firmware.

It performs no hardware access: no serial port, no COM enumeration, no
esptool, no bitaxetool, no OTA upload, no device restart, no DNS and no NTP
request. It builds, verifies and stages files. Flashing is owner-executed.

Usage:
    python tools/pilot/build_store_preflight.py --out-dir '<external-artifact-directory>'
    NX_PREFLIGHT_ARTIFACT_ROOT='<external-artifact-directory>' python tools/pilot/build_store_preflight.py
    python tools/pilot/build_store_preflight.py --print-flags
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
ARTIFACT_ROOT_ENV = "NX_PREFLIGHT_ARTIFACT_ROOT"

DEFAULT_IDF_IMAGE = "espressif/idf:v5.5.3"
CONTAINER_REPO = "/nx/repo"
CONTAINER_WORK = "/nx/work"

PREFLIGHT_FLAGS_ON = ("CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT",)
PREFLIGHT_FLAGS_OFF = (
    # The PARENT flag is in this list deliberately: the four sub-flags all
    # depend on it, so excluding only them would still permit
    # CONFIG_NX_TIMED_SESSIONS=y, whose Gate B6 bootstrap opens nx_tps with
    # NVS_READWRITE and CREATES the namespace the preflight must only read.
    "CONFIG_NX_TIMED_SESSIONS",
    "CONFIG_NX_TIMED_SESSIONS_EXECUTION",
    "CONFIG_NX_TIMED_SESSIONS_API",
    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE",
    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS",
)

APP_SLOT_BYTES = 4 * 1024 * 1024
WWW_SLOT_BYTES = 3 * 1024 * 1024

# Nothing that can act on the store, the pool, the protocol or the clock may
# be linked into a preflight image.
FORBIDDEN_SYMBOL_PREFIXES = (
    "pool_session_execution",
    "pool_exec_",
    "nx_pool_execution_",
    "pool_session_api_",
    "pool_session_command",
    "nx_pool_session_api_",
    "pool_api_command_",
    "pool_pilot_",            # Gate B10.1 observation diagnostics
    "pool_time_sntp_",        # trusted-time provider
    "pool_time_source_",      # trusted-time source policy
)
# The preflight itself must really be there.
REQUIRED_SYMBOL_PREFIXES = ("nx_tps_preflight_",)

# The committed Gate B3 mutation entry points must be unreachable: a preflight
# image has no caller for them, so the linker must not pull them in.
FORBIDDEN_STORE_WRITE_SYMBOLS = (
    "pool_session_store_commit_record",
    "pool_session_store_commit_clear",
)

EXPECTED_TOKENS = [
    "TPS_PREFLIGHT_BOOT",
    "TPS_PREFLIGHT_EMPTY",
    "TPS_PREFLIGHT_CLEARED",
    "TPS_PREFLIGHT_BLOCKED_RECORD",
    "TPS_PREFLIGHT_BLOCKED_TERMINAL",
    "TPS_PREFLIGHT_BLOCKED_UNCERTAIN",
    "TPS_PREFLIGHT_BLOCKED_CORRUPT",
    "TPS_PREFLIGHT_BLOCKED_SCHEMA",
    "TPS_PREFLIGHT_BLOCKED_IO",
    "TPS_PREFLIGHT_BLOCKED_NVS_INIT",
    "TPS_PREFLIGHT_INTERNAL_ERROR",
    "TPS_PREFLIGHT_COMPLETE",
]
ACCEPTABLE_TOKENS = ["TPS_PREFLIGHT_EMPTY", "TPS_PREFLIGHT_CLEARED"]
BLOCKING_TOKENS = [t for t in EXPECTED_TOKENS
                   if t not in ACCEPTABLE_TOKENS and t not in
                   ("TPS_PREFLIGHT_BOOT", "TPS_PREFLIGHT_COMPLETE")]


# --------------------------------------------------------------------------
# THE canonical revision provider — one identity for firmware AND web.
# --------------------------------------------------------------------------
import importlib.util as _ilu

_canon_spec = _ilu.spec_from_file_location(
    "nx_canonical_revision", Path(__file__).resolve().parent / "canonical_revision.py")
canon = _ilu.module_from_spec(_canon_spec)
_canon_spec.loader.exec_module(canon)


class PreflightError(Exception):
    """A fatal failure. Messages never contain a local path or private value."""


def fail(msg: str) -> None:
    raise PreflightError(msg)


# --------------------------------------------------------------------------
# Repository state
# --------------------------------------------------------------------------

def git(repo: Path, *args: str) -> str:
    out = subprocess.run(["git", *args], cwd=str(repo), text=True, capture_output=True)
    if out.returncode != 0:
        fail(f"git {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout.strip()


def repo_state(repo: Path) -> dict:
    """The build identity. ONE canonical revision, plus the full commit.

    Dirtiness is decided by `git status --porcelain`, not by
    `git describe --dirty`: on this repository the host's --dirty did not
    report a genuinely dirty tree while porcelain did.
    """
    if canon.working_tree_dirty(repo):
        fail("the working tree is not clean — an artifact must be built from an "
             "exact committed HEAD. Commit or stash first.")
    try:
        commit = canon.full_commit(repo)
        describe = canon.canonical_revision(repo)
    except canon.RevisionError as exc:
        fail(str(exc))
    return {
        "commit": commit,          # full 40 characters, never abbreviated
        "describe": describe,      # the ONE canonical display revision
        "branch": git(repo, "rev-parse", "--abbrev-ref", "HEAD"),
    }


def tracked_tree_digest(repo: Path) -> str:
    return hashlib.sha256(git(repo, "ls-files", "-s").encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------
# Output location — always caller-supplied, never hardcoded
# --------------------------------------------------------------------------

def resolve_out_dir(arg_out_dir: str | None, env: dict | None = None) -> Path:
    env = os.environ if env is None else env
    chosen = arg_out_dir or env.get(ARTIFACT_ROOT_ENV) or ""
    if not chosen.strip():
        fail(f"no output directory: pass --out-dir <external-artifact-directory> or set "
             f"{ARTIFACT_ROOT_ENV}. The package location is never hardcoded.")
    return Path(chosen).expanduser().resolve()


# --------------------------------------------------------------------------
# Temporary configuration (never inside the repository)
# --------------------------------------------------------------------------

def preflight_defaults_text() -> str:
    lines = [
        "# NeuralAxe OS Gate B10.2 read-only store preflight.",
        "# TEMPORARY, OWNER-LOCAL, NEVER COMMITTED. Deleted when the build ends.",
        "CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT=y",
        "# CONFIG_NX_TIMED_SESSIONS is not set",
        "# CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set",
        "# CONFIG_NX_TIMED_SESSIONS_API is not set",
        "# CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE is not set",
        "# CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS is not set",
    ]
    return "\n".join(lines) + "\n"


def write_preflight_defaults(work: Path) -> Path:
    path = work / "preflight.sdkconfig.defaults"
    path.write_text(preflight_defaults_text(), encoding="utf-8")
    return path


# --------------------------------------------------------------------------
# Build execution
# --------------------------------------------------------------------------

def find_container_runtime() -> str:
    for runtime in ("docker", "podman"):
        if shutil.which(runtime):
            return runtime
    fail("neither docker nor podman was found in PATH, and IDF_PATH is not set.")
    raise AssertionError("unreachable")


def build_command(repo_path: str, work_path: str, *, revision: str) -> str:
    defaults = f"{repo_path}/sdkconfig.defaults;{work_path}/preflight.sdkconfig.defaults"
    # -DPROJECT_VER pins the firmware identity to the ONE canonical
    # revision. Without it ESP-IDF calls git_describe() with no --abbrev
    # and the container's git picks its own length, which is how the
    # firmware ended up 8 hex characters while the host-built web image
    # was 7 — an exact-string BOOT PAIR MISMATCH on a real device.
    return (f"idf.py -B {work_path}/build -DSDKCONFIG={work_path}/sdkconfig "
            f'-DPROJECT_VER="{revision}" '
            f'-DSDKCONFIG_DEFAULTS="{defaults}" set-target esp32s3 build')


def run_build(repo: Path, work: Path, image: str, native: bool, *,
              revision: str) -> None:
    if native:
        proc = subprocess.run(["bash", "-lc",
                               build_command(str(repo), str(work), revision=revision)],
                              cwd=str(repo), env={**os.environ, "GITHUB_ACTIONS": "true"})
        if proc.returncode != 0:
            fail(f"preflight build failed (exit {proc.returncode})")
        return

    runtime = find_container_runtime()
    inner = (f"git config --global --add safe.directory {CONTAINER_REPO} && "
             "git config --global core.autocrlf true && "
             "git config --global core.filemode false && "
             + build_command(CONTAINER_REPO, CONTAINER_WORK, revision=revision))
    cmd = [runtime, "run", "--rm",
           "-v", f"{repo}:{CONTAINER_REPO}:rw",
           "-v", f"{work}:{CONTAINER_WORK}:rw",
           "-w", CONTAINER_REPO, "-e", "GITHUB_ACTIONS=true", image,
           "bash", "-lc", inner]
    print(f"building in {runtime} image {image}")
    if subprocess.run(cmd).returncode != 0:
        fail("preflight build failed")


# --------------------------------------------------------------------------
# Verification
# --------------------------------------------------------------------------

def parse_sdkconfig_h(path: Path) -> dict:
    if not path.is_file():
        fail("generated sdkconfig.h not found")
    defines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"#define\s+(\S+)\s*(.*)$", line.strip())
        if m:
            defines[m.group(1)] = m.group(2).strip()
    return defines


def verify_build_config(defines: dict) -> dict:
    for flag in PREFLIGHT_FLAGS_ON:
        if defines.get(flag) != "1":
            fail(f"{flag} is not enabled in the generated configuration")
    for flag in PREFLIGHT_FLAGS_OFF:
        if flag in defines:
            fail(f"{flag} IS enabled — refusing to stage a preflight artifact that can "
                 "execute, expose an API, observe trusted time or emit pilot diagnostics")
    # A preflight must not carry a trusted-time source either.
    src = defines.get("CONFIG_NX_TIMED_SESSIONS_NTP_SERVER", '""').strip()
    if src not in ('""', ""):
        fail("the generated configuration carries a trusted-time source — a preflight "
             "build must select none")
    summary = {flag: True for flag in PREFLIGHT_FLAGS_ON}
    summary.update({flag: False for flag in PREFLIGHT_FLAGS_OFF})
    return summary


def list_symbols(elf: Path, work: Path, image: str, native: bool) -> list[str]:
    nm = "xtensa-esp32s3-elf-nm"
    if native:
        proc = subprocess.run([nm, str(elf)], text=True, capture_output=True)
    else:
        runtime = find_container_runtime()
        proc = subprocess.run(
            [runtime, "run", "--rm", "-v", f"{work}:{CONTAINER_WORK}:ro", image, nm,
             f"{CONTAINER_WORK}/{elf.relative_to(work).as_posix()}"],
            text=True, capture_output=True)
    if proc.returncode != 0:
        fail("symbol listing failed")
    return [ln.split()[-1] for ln in proc.stdout.splitlines() if ln.split()]


def verify_symbols(names: list[str]) -> dict:
    forbidden = sorted({n for n in names
                        if any(n.startswith(p) for p in FORBIDDEN_SYMBOL_PREFIXES)})
    if forbidden:
        fail("the preflight image links forbidden execution/API/trusted-time symbols: "
             + ", ".join(forbidden[:10]))
    writers = sorted({n for n in names if n in FORBIDDEN_STORE_WRITE_SYMBOLS})
    if writers:
        fail("the preflight image links a Gate B3 store MUTATION entry point: "
             + ", ".join(writers))
    found = {}
    for prefix in REQUIRED_SYMBOL_PREFIXES:
        hits = sorted({n for n in names if n.startswith(prefix)})
        if not hits:
            fail(f"the preflight image links no '{prefix}*' symbol — the read-only "
                 "inspector is not actually in this build")
        found[prefix] = len(hits)
    return found


def verify_sizes(build: Path) -> dict:
    app, www = build / "esp-miner.bin", build / "www.bin"
    for path in (app, www):
        if not path.is_file():
            fail(f"expected build output missing: {path.name}")
    a, w = app.stat().st_size, www.stat().st_size
    if a >= APP_SLOT_BYTES:
        fail(f"esp-miner.bin ({a} B) does not fit a 4 MiB app slot")
    if w > WWW_SLOT_BYTES:
        fail(f"www.bin ({w} B) does not fit the 3 MiB www partition")
    return {"espMinerBinBytes": a, "espMinerBinFreeBytes": APP_SLOT_BYTES - a,
            "wwwBinBytes": w, "wwwBinFreeBytes": WWW_SLOT_BYTES - w}


APP_DESC_MAGIC = b"\x32\x54\xcd\xab"


def read_app_desc_version(path: Path) -> str:
    with path.open("rb") as h:
        header = h.read(0x60)
    if len(header) < 0x60 or header[0x20:0x24] != APP_DESC_MAGIC:
        fail(f"{path.name}: esp_app_desc_t magic not found")
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

    # Hand generate-version.js the EXACT canonical revision the firmware build
    # also receives, so neither side derives its own abbreviation.
    env = {**os.environ, canon.REVISION_ENV: describe}
    print("building the web UI: npm run build (production)")
    if subprocess.run([npm, "run", "build"], cwd=str(web), env=env).returncode != 0:
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

    # EXACT STRING EQUALITY, in both directions, against the ONE canonical
    # revision. Never a prefix test: 'v2.14.2-70-g34a5150' is a prefix of
    # 'v2.14.2-70-g34a51508' and names the same commit, yet the device compares
    # strings and reports BOOT PAIR MISMATCH.
    if fw != web:
        fail(f"BOOT PAIR MISMATCH would ship: firmware '{fw}' != web '{web}'")
    if fw != describe:
        fail(f"the built firmware '{fw}' does not match HEAD '{describe}'")
    if web != describe:
        fail(f"the generated web image '{web}' does not match HEAD '{describe}'")

    # Both sides agreeing is necessary but not sufficient: they could agree on a
    # non-canonical shape (for example if -DPROJECT_VER were ever dropped and
    # both fell back on git's default abbreviation). Pin the shape too.
    for label, value in (("firmware", fw), ("web", web)):
        try:
            canon.validate_canonical(value)
        except canon.RevisionError as exc:
            fail(f"the {label} identity is not canonical: {exc}")
    return {"firmwareRevision": fw, "webRevision": web}


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# Staging and manifest
# --------------------------------------------------------------------------

def stage_artifacts(build: Path, out_dir: Path, prefix: str) -> list[dict]:
    plan = [
        (build / "esp-miner.bin", f"{prefix}-preflight-ota.bin", "ota-application",
         "AxeOS Update page / POST /api/system/OTA (inactive OTA slot + otadata)"),
        (build / "www.bin", f"{prefix}-preflight-www.bin", "www-update",
         "AxeOS Update page / POST /api/system/OTAWWW (www partition only)"),
    ]
    out_dir.mkdir(parents=True, exist_ok=True)
    artifacts = []
    for src, name, kind, method in plan:
        dst = out_dir / name
        shutil.copyfile(src, dst)
        artifacts.append({"filename": name, "artifactType": kind,
                          "sizeBytes": dst.stat().st_size, "sha256": sha256_of(dst),
                          "flashMethod": method, "settingsPreserved": True,
                          "destructive": False})
        print(f"staged {name} ({dst.stat().st_size} bytes)")
    return artifacts


def build_manifest(state: dict, flags: dict, sizes: dict, symbols: dict,
                   fw_version: str, artifacts: list[dict], build_timestamp: str,
                   pair: dict | None = None) -> dict:
    pair = pair or {}
    return {
        "schemaVersion": SCHEMA_VERSION,
        "package": "NeuralAxe OS Gate B10.2 read-only store preflight",
        "productName": "NeuralAxe OS",
        "productVersion": "0.1.0-dev",
        "buildChannel": "development",
        "vendor": "NeuralShield",
        "targetDevice": "Gamma",
        "targetBoard": "601",
        "targetAsic": "BM1370",
        # The full 40-character commit, recorded SEPARATELY and never
        # abbreviated. The canonical revision below is a display identity only.
        "gitCommit": state["commit"],
        "gitDescribe": state["describe"],
        "canonicalGitDescribe": state["describe"],
        "canonicalAbbrevLength": canon.CANONICAL_ABBREV,
        "canonicalDescribeCommand": "git " + " ".join(canon.CANONICAL_DESCRIBE_ARGS),
        "firmwareVersion": fw_version,
        # The two EMBEDDED identities, extracted from the generated binaries.
        # They are the pair a device compares at boot; a matching SHA-256 says
        # nothing about them, which is why both are recorded explicitly.
        "firmwareRevision": pair.get("firmwareRevision", fw_version),
        "webRevision": pair.get("webRevision", "<not verified>"),
        "releasePairVerified": bool(pair) and
                               pair.get("firmwareRevision") == pair.get("webRevision") ==
                               state["describe"],
        "webImageFreshlyBuilt": True,
        "buildTimestampUtc": build_timestamp,
        "storePreflightEnabled": True,
        "executionEnabled": False,
        "apiEnabled": False,
        "timeObservationEnabled": False,
        "pilotDiagnosticsEnabled": False,
        "trustedTimeSourceConfigured": False,
        "trustedTimeSourceValue": "<none: a preflight build selects no NTP source>",
        "readOnlyStoreAccess": True,
        "timedSessionStoreWritesPossible": False,
        "wholeImageNvsWriteFree": False,
        "destructiveNvsRecoveryDisabled": True,
        "wholeImageNvsWriteNote": (
            "The dedicated inspector never writes nx_tps, and the classification "
            "happens BEFORE any write-capable configuration path runs. The IMAGE is "
            "still not NVS write-free: after classification, nvs_config_init() may "
            "write the 'main' namespace (schema migrations, first-boot defaults, the "
            "settings queue). DESTRUCTIVE NVS RECOVERY IS COMPILED OUT in this "
            "posture: nvs_flash_erase() has no call site, and any NVS init failure "
            "emits TPS_PREFLIGHT_BLOCKED_NVS_INIT and halts boot inert."
        ),
        "flags": flags,
        "sizes": sizes,
        "linkedSymbolGroups": symbols,
        "expectedSerialTokens": EXPECTED_TOKENS,
        "acceptableResults": ACCEPTABLE_TOKENS,
        "blockingResults": BLOCKING_TOKENS,
        "flashMethod": (
            "AxeOS Update page (POST /api/system/OTAWWW then POST /api/system/OTA). "
            "Writes the www partition and the inactive OTA slot + otadata only; NVS "
            "settings (Wi-Fi, pools, tuning, fan) are preserved. Owner-executed."
        ),
        "recoveryMethod": (
            "Re-upload the previous known-good www.bin and esp-miner.bin through the "
            "same Update page. No factory/merged image is included: a full flash at 0x0 "
            "would ERASE NVS, which would destroy the very store this image inspects."
        ),
        "factoryImageIncluded": False,
        "privateArtifactsExcluded": True,
        "artifacts": artifacts,
    }


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-root", default=None)
    parser.add_argument("--out-dir", default=None,
                        help="<external-artifact-directory>; falls back to "
                             f"${ARTIFACT_ROOT_ENV}. Never hardcoded.")
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--idf-image", default=DEFAULT_IDF_IMAGE)
    parser.add_argument("--build-timestamp", default=None)
    parser.add_argument("--print-flags", action="store_true")
    parser.add_argument("--keep-work-dir", action="store_true")
    parser.add_argument("--npm-install", choices=("ci", "install", "skip"), default="ci",
                        help="how to prepare node_modules before the fresh web build. "
                             "'ci' (default) installs exactly package-lock.json; 'skip' "
                             "reuses an existing node_modules for an offline rebuild. It "
                             "never skips the web BUILD itself.")
    args = parser.parse_args()

    if args.print_flags:
        for flag in PREFLIGHT_FLAGS_ON:
            print(f"{flag}=y")
        for flag in PREFLIGHT_FLAGS_OFF:
            print(f"{flag}=n")
        return 0

    repo = Path(args.repo_root).resolve() if args.repo_root else Path(
        subprocess.check_output(["git", "rev-parse", "--show-toplevel"],
                                cwd=str(Path(__file__).resolve().parent),
                                text=True).strip()).resolve()

    out_dir = resolve_out_dir(args.out_dir)
    state = repo_state(repo)
    before_digest = tracked_tree_digest(repo)
    print(f"building from {state['describe']} ({state['commit'][:12]}) on {state['branch']}")

    native = bool(os.environ.get("IDF_PATH"))
    work = Path(args.work_dir).resolve() if args.work_dir else Path(
        tempfile.mkdtemp(prefix="nx-b102-preflight-")).resolve()
    try:
        work.relative_to(repo)
    except ValueError:
        pass
    else:
        fail("the temporary build tree must live OUTSIDE the repository")

    config_path = write_preflight_defaults(work)
    try:
        # THE web stage, before the firmware build. The firmware build runs with
        # GITHUB_ACTIONS=true, which packs main/http_server/axe-os/dist verbatim
        # without running npm — so that directory must have been produced by THIS
        # run, from THIS commit, or a stale image ships.
        web_revision = build_frontend(repo, state["describe"], args.npm_install)
        print(f"web UI built fresh at {web_revision}")

        run_build(repo, work, args.idf_image, native,
                  revision=state["describe"])
        build = work / "build"
        flags = verify_build_config(parse_sdkconfig_h(build / "config" / "sdkconfig.h"))
        symbols = verify_symbols(list_symbols(build / "esp-miner.elf", work,
                                              args.idf_image, native))
        sizes = verify_sizes(build)
        fw_version = read_app_desc_version(build / "esp-miner.bin")
        if "-dirty" in fw_version:
            fail(f"the built image embeds a dirty identity ({fw_version})")

        # THE release-pair gate, BEFORE anything is staged. Reads the identity
        # embedded in each generated binary and refuses a mismatch — the check
        # whose absence shipped a BOOT PAIR MISMATCH package.
        pair = verify_release_pair(build, state["describe"])
        print(f"release pair verified: firmware {pair['firmwareRevision']} == "
              f"web {pair['webRevision']} == HEAD {state['describe']}")

        prefix = f"NeuralAxe-OS-v0.1.0-dev-Gamma-601-{state['describe']}"
        artifacts = stage_artifacts(build, out_dir, prefix)
        manifest = build_manifest(state, flags, sizes, symbols, fw_version, artifacts,
                                  args.build_timestamp or "<not recorded>", pair)
        manifest_path = out_dir / f"{prefix}-preflight-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        sums = "\n".join(f"{a['sha256']}  {a['filename']}" for a in artifacts) + "\n"
        (out_dir / f"{prefix}-preflight-SHA256SUMS.txt").write_text(sums, encoding="utf-8")

        blob = manifest_path.read_text(encoding="utf-8")
        for leak in (str(repo), str(work), str(out_dir)):
            if leak and leak in blob:
                fail("the manifest would record a local filesystem path — refusing")
        print(f"wrote {manifest_path.name}")
    finally:
        try:
            config_path.unlink()
        except OSError:
            pass
        if not args.keep_work_dir and args.work_dir is None:
            shutil.rmtree(work, ignore_errors=True)
        # Remove the web build output we produced. Leaving it behind is exactly
        # how a later build packs a stale image: the moment HEAD moves, that
        # directory is wrong, and the GITHUB_ACTIONS path would pack it anyway.
        remove_stale_web_output(repo)

    if before_digest != tracked_tree_digest(repo):
        fail("the build modified tracked files")
    if git(repo, "status", "--porcelain"):
        fail("the build left the working tree dirty")
    print("tracked tree unchanged; temporary configuration and web output removed")
    print("PREFLIGHT BUILD OK — no hardware was touched and no flash was performed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PreflightError as exc:
        print(f"PREFLIGHT BUILD FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
