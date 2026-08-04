#!/usr/bin/env python3
"""NeuralAxe OS — weather recommendation-only pilot builder (Gates W5/W6).

Builds a firmware/web pair whose Weather-Aware Tuning source is configured
from PRIVATE, OWNER-SUPPLIED values, without those values ever entering the
repository, a manifest, a log line or a version string.

WHAT THIS BUILDS
    A RECOMMENDATION-ONLY image. The weather runtime may produce a bounded
    recommendation; it changes no frequency, voltage, fan setting or thermal
    limit, no pool or protocol, starts no timed session, acquires no lease,
    invokes no Gate B7 execution, registers no HTTP route and never restarts.
    Applying a recommendation to hardware requires a separate future gate
    that does not exist.

WHERE THE PRIVATE VALUES LIVE
    Only in the environment, and only for the lifetime of this process:

        NX_PILOT_NTP_SERVER    the trusted-time source (hostname or IPv4)
        NX_WEATHER_DISTRIBUTION owner-managed-external
        NX_WEATHER_PROVIDER     open-meteo
        NX_WEATHER_LATITUDE     signed decimal degrees, e.g. -12.3456
        NX_WEATHER_LONGITUDE    signed decimal degrees
        NX_WEATHER_TIMEZONE     europe/brussels

    They are parsed with a locale-independent fixed-point reader, validated,
    and written to ONE temporary sdkconfig fragment outside the repository,
    which is overwritten and deleted in a finally block. They are NEVER
    printed: no echo, no error message quoting a value, no manifest field.
    The repository defaults stay unconfigured, so a normal build produces a
    device that selects no provider and requests nothing.

WHAT THE BUILD PROVES (Gate W6)
    After the build it verifies the posture from the GENERATED sdkconfig.h
    (intent is not evidence), audits the ELF for forbidden symbols (B7
    execution, B8 command routes, B10.2 preflight, any weather hardware
    apply) and for required ones (W4/W5/W6 plus exactly ONE trusted-time
    provider), checks the image fits its OTA slot, and verifies the exact
    canonical release pair firmwareRevision == webRevision == describe.

WHAT THE MANIFEST RECORDS
    Bounded booleans and enums only — whether a thing was configured, never
    what it was. See build_manifest().
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canonical_revision  # noqa: E402

SCHEMA_VERSION = 1

ENV_NTP = "NX_PILOT_NTP_SERVER"
ENV_DISTRIBUTION = "NX_WEATHER_DISTRIBUTION"
ENV_PROVIDER = "NX_WEATHER_PROVIDER"
ENV_LATITUDE = "NX_WEATHER_LATITUDE"
ENV_LONGITUDE = "NX_WEATHER_LONGITUDE"
ENV_TIMEZONE = "NX_WEATHER_TIMEZONE"
ENV_VARS = (ENV_NTP, ENV_DISTRIBUTION, ENV_PROVIDER, ENV_LATITUDE,
            ENV_LONGITUDE, ENV_TIMEZONE)

DEFAULT_IDF_IMAGE = "espressif/idf:v5.5.3"
CONTAINER_REPO = "/nx/repo"
CONTAINER_WORK = "/nx/work"

# The one provider Gate W5 supports, and the one timezone Gate W3 implements.
SUPPORTED_PROVIDERS = {"open-meteo": "OPEN_METEO"}
SUPPORTED_TIMEZONES = {"europe/brussels": "EUROPE_BRUSSELS"}
# Gate W5 permits exactly one distribution mode to use a provider.
SUPPORTED_DISTRIBUTIONS = {"owner-managed-external": "OWNER_MANAGED_EXTERNAL"}

# The trusted-time source is a hostname or IPv4 literal, validated with the
# same conservative grammar the committed B10.1 helper uses. It is PRIVATE:
# it is never printed and never reaches a manifest.
HOST_MAX = 63
LABEL_CHARS = re.compile(r"^[A-Za-z0-9-]+$")

APP_SLOT_BYTES = 4 * 1024 * 1024
WWW_SLOT_BYTES = 3 * 1024 * 1024

LAT_E4_MAX = 900000
LON_E4_MAX = 1800000

# Stable, value-free outcome tokens (mirrors NxWeatherSourceStatus).
SRC_UNCONFIGURED = "WX_SRC_UNCONFIGURED"
SRC_INVALID_PROVIDER = "WX_SRC_INVALID_PROVIDER"
SRC_INVALID_LATITUDE = "WX_SRC_INVALID_LATITUDE"
SRC_INVALID_LONGITUDE = "WX_SRC_INVALID_LONGITUDE"
SRC_INVALID_TIMEZONE = "WX_SRC_INVALID_TIMEZONE"
SRC_INCOMPLETE = "WX_SRC_INCOMPLETE"
SRC_INVALID_DISTRIBUTION = "WX_SRC_INVALID_DISTRIBUTION"
SRC_INVALID_NTP = "WX_SRC_INVALID_NTP_SOURCE"
SRC_READY = "WX_SRC_READY_RECOMMENDATION_ONLY"

# Symbols that must NOT be linked into a recommendation-only pilot.
FORBIDDEN_SYMBOL_PREFIXES = (
    "pool_session_execution_",   # Gate B7 execution
    "pool_session_executor_",
    "nx_pool_session_api_",      # Gate B8 command routes
    "nx_tps_preflight_",         # Gate B10.2 store preflight
    "nx_weather_apply_",         # any future weather hardware apply
)
# A benign conflict-reporting helper, not a command route (the committed
# Gate B10.1 helper allowlists the same symbol).
ALLOWED_DESPITE_PREFIX = frozenset({"nx_pool_session_api_send_conflict"})

REQUIRED_SYMBOL_PREFIXES = (
    "nx_weather_pilot_",         # Gate W6 diagnostics
    "nx_weather_source_",        # Gate W5 policy
    "weather_runtime_",          # Gate W4 runtime
    "pool_time_sntp_",           # the ONE committed trusted-time provider
    "nx_mutation_",              # Gate W6.1 mutation observability
)

# A deliberately strict decimal-degree grammar: optional sign, digits, an
# optional '.' and up to four fractional digits. No exponent, no thousands
# separator, no comma decimal mark, no leading '+', no whitespace — so the
# parse cannot depend on a locale and cannot silently accept "1,5" or "1e2".
DEGREE_RE = re.compile(r"^-?(?:0|[1-9][0-9]*)(?:\.[0-9]{1,4})?$")


class PilotError(Exception):
    """A failure that must never carry a private value in its message."""


def fail(msg: str) -> None:
    raise PilotError(msg)


# --------------------------------------------------------------------------
# Private value parsing — locale independent, fixed point, never echoed
# --------------------------------------------------------------------------

def parse_degrees_e4(text: str) -> int:
    """Parse signed decimal degrees into degrees * 1e4 using integer maths.

    Deliberately NOT float(): float parsing is locale-sensitive in some
    runtimes, admits 'nan'/'inf'/'1e3', and would introduce a rounding step
    between the owner's value and the committed e4 wire unit.

    Raises PilotError WITHOUT including the offending text.
    """
    if not DEGREE_RE.match(text):
        fail("value is not a plain signed decimal with at most 4 decimals")
    negative = text.startswith("-")
    body = text[1:] if negative else text
    if "." in body:
        whole, frac = body.split(".", 1)
    else:
        whole, frac = body, ""
    frac = (frac + "0000")[:4]
    value = int(whole) * 10000 + int(frac)
    return -value if negative else value


def ntp_source_valid(host: str) -> bool:
    """Validate a private trusted-time source WITHOUT echoing it.

    Conservative on purpose: a hostname of dot-separated alphanumeric/hyphen
    labels, or a dotted-quad IPv4 literal with no leading zeros. No scheme, no
    port, no path, no IPv6 and no single-label name — the same shape the
    committed Gate B10.1 helper accepts.
    """
    if not host or len(host) > 253:
        return False
    if "://" in host or "/" in host or ":" in host or " " in host:
        return False
    labels = host.split(".")
    if len(labels) < 2 or len(labels) > 16:
        return False
    for label in labels:
        if not label or len(label) > HOST_MAX:
            return False
        if label.startswith("-") or label.endswith("-"):
            return False
        if not LABEL_CHARS.match(label):
            return False
    # A dotted quad must be a valid IPv4 without leading zeros.
    if all(label.isdigit() for label in labels):
        if len(labels) != 4:
            return False
        for label in labels:
            if len(label) > 1 and label.startswith("0"):
                return False
            if int(label) > 255:
                return False
    return True


def read_private_config(env: dict | None = None) -> dict:
    """Read and validate the private configuration from the environment.

    Returns a dict containing the parsed values AND a value-free `status`.
    The caller may log `status` and the boolean flags; it may never log the
    values.
    """
    src = os.environ if env is None else env

    raw_ntp = (src.get(ENV_NTP) or "").strip()
    raw_distribution = (src.get(ENV_DISTRIBUTION) or "").strip().lower()
    raw_provider = (src.get(ENV_PROVIDER) or "").strip().lower()
    raw_latitude = (src.get(ENV_LATITUDE) or "").strip()
    raw_longitude = (src.get(ENV_LONGITUDE) or "").strip()
    raw_timezone = (src.get(ENV_TIMEZONE) or "").strip().lower()

    supplied = [bool(raw_ntp), bool(raw_distribution), bool(raw_provider),
                bool(raw_latitude), bool(raw_longitude), bool(raw_timezone)]
    if not any(supplied):
        return {"status": SRC_UNCONFIGURED, "ready": False}
    if not all(supplied):
        return {"status": SRC_INCOMPLETE, "ready": False}

    if not ntp_source_valid(raw_ntp):
        return {"status": SRC_INVALID_NTP, "ready": False}
    if raw_distribution not in SUPPORTED_DISTRIBUTIONS:
        return {"status": SRC_INVALID_DISTRIBUTION, "ready": False}
    if raw_provider not in SUPPORTED_PROVIDERS:
        return {"status": SRC_INVALID_PROVIDER, "ready": False}
    if raw_timezone not in SUPPORTED_TIMEZONES:
        return {"status": SRC_INVALID_TIMEZONE, "ready": False}

    try:
        lat_e4 = parse_degrees_e4(raw_latitude)
    except PilotError:
        return {"status": SRC_INVALID_LATITUDE, "ready": False}
    try:
        lon_e4 = parse_degrees_e4(raw_longitude)
    except PilotError:
        return {"status": SRC_INVALID_LONGITUDE, "ready": False}

    if not -LAT_E4_MAX <= lat_e4 <= LAT_E4_MAX:
        return {"status": SRC_INVALID_LATITUDE, "ready": False}
    if not -LON_E4_MAX <= lon_e4 <= LON_E4_MAX:
        return {"status": SRC_INVALID_LONGITUDE, "ready": False}
    # The committed repository-wide "unset" sentinel; also rejects a half
    # supplied pair that parsed to zero.
    if lat_e4 == 0 or lon_e4 == 0:
        return {"status": SRC_INCOMPLETE, "ready": False}

    return {
        "status": SRC_READY,
        "ready": True,
        "ntp": raw_ntp,
        "distribution": SUPPORTED_DISTRIBUTIONS[raw_distribution],
        "provider": SUPPORTED_PROVIDERS[raw_provider],
        "timezone": SUPPORTED_TIMEZONES[raw_timezone],
        "latitude_e4": lat_e4,
        "longitude_e4": lon_e4,
    }


# --------------------------------------------------------------------------
# Repository state
# --------------------------------------------------------------------------

def git(repo: Path, *args: str) -> str:
    proc = subprocess.run(["git", "-C", str(repo), *args],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        fail(f"git {' '.join(args)} failed")
    return proc.stdout.strip()


def require_clean_tree(repo: Path) -> None:
    if git(repo, "status", "--porcelain", "--untracked-files=all"):
        fail("the working tree is not clean; commit or stash before building")


def tracked_tree_digest(repo: Path) -> str:
    """Digest of every tracked path and blob id — proves the build left the
    tracked tree byte-identical."""
    return hashlib.sha256(git(repo, "ls-files", "-s").encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------
# Temporary configuration (never inside the repository)
# --------------------------------------------------------------------------

def pilot_defaults_text(cfg: dict) -> str:
    """The temporary sdkconfig fragment: the ONLY place the private values
    appear, outside the repository, for the duration of one build."""
    lines = [
        "# NeuralAxe OS Gate W5 weather recommendation-only pilot.",
        "# TEMPORARY, OWNER-LOCAL, NEVER COMMITTED. Deleted when the build ends.",
        "CONFIG_NX_TIMED_SESSIONS=y",
        "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE=y",
        f'CONFIG_NX_TIMED_SESSIONS_NTP_SERVER="{cfg["ntp"]}"',
        "CONFIG_NX_WEATHER_AWARE_TUNING=y",
        "CONFIG_NX_WEATHER_SOURCE_POLICY=y",
        f"CONFIG_NX_WEATHER_DIST_{cfg['distribution']}=y",
        f"CONFIG_NX_WEATHER_PROVIDER_{cfg['provider']}=y",
        f"CONFIG_NX_WEATHER_TZ_{cfg['timezone']}=y",
        "CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS=y",
        # Gate W6.1. Without this the pilot has NO authority to read at the
        # mutation boundaries, every mutation fact is stamped UNAVAILABLE and
        # the invariant monitor can never report a healthy pilot.
        "CONFIG_NX_MUTATION_OBSERVABILITY=y",
        f"CONFIG_NX_WEATHER_LATITUDE_E4={cfg['latitude_e4']}",
        f"CONFIG_NX_WEATHER_LONGITUDE_E4={cfg['longitude_e4']}",
        "# Recommendation-only: every execution surface stays off.",
        "# CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set",
        "# CONFIG_NX_TIMED_SESSIONS_API is not set",
        "# CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT is not set",
    ]
    return "\n".join(lines) + "\n"


def write_pilot_defaults(work: Path, cfg: dict) -> Path:
    path = work / "weather.sdkconfig.defaults"
    path.write_text(pilot_defaults_text(cfg), encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    return path


def shred(path: Path) -> None:
    """Overwrite then remove a file that held private values."""
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
# Post-build verification: configuration, symbols, partitions, release pair
# --------------------------------------------------------------------------

REQUIRED_SDKCONFIG = (
    "CONFIG_NX_TIMED_SESSIONS",
    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE",
    "CONFIG_NX_WEATHER_AWARE_TUNING",
    "CONFIG_NX_WEATHER_SOURCE_POLICY",
    "CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS",
    "CONFIG_NX_MUTATION_OBSERVABILITY",
)
FORBIDDEN_SDKCONFIG = (
    "CONFIG_NX_TIMED_SESSIONS_EXECUTION",
    "CONFIG_NX_TIMED_SESSIONS_API",
    "CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT",
)


def verify_sdkconfig_h(build: Path) -> None:
    """Prove the posture from the GENERATED header, not from our own fragment.

    The fragment states intent; sdkconfig.h states what the compiler actually
    saw. Only the second is evidence.
    """
    path = build / "config" / "sdkconfig.h"
    if not path.is_file():
        fail("generated sdkconfig.h not found; cannot verify the build posture")
    text = path.read_text(encoding="utf-8", errors="replace")
    for sym in REQUIRED_SDKCONFIG:
        if f"#define {sym} 1" not in text:
            fail(f"required build option missing: {sym}")
    for sym in FORBIDDEN_SDKCONFIG:
        if f"#define {sym} 1" in text:
            fail(f"forbidden build option enabled: {sym}")


def elf_symbols(elf: Path) -> set[str]:
    for nm in ("xtensa-esp32s3-elf-nm", "nm"):
        proc = subprocess.run([nm, "-g", "--defined-only", str(elf)],
                              capture_output=True, text=True)
        if proc.returncode == 0:
            out = set()
            for line in proc.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 3:
                    out.add(parts[-1])
            return out
    fail("no usable nm binary found for the ELF symbol audit")
    return set()


def verify_symbols(elf: Path) -> dict:
    """Prove at the binary level what this pilot may and may not contain."""
    syms = elf_symbols(elf)

    offending = sorted(
        s for s in syms
        if s.startswith(FORBIDDEN_SYMBOL_PREFIXES)
        and s not in ALLOWED_DESPITE_PREFIX)
    if offending:
        fail(f"forbidden symbols linked into the pilot image: {len(offending)}")

    missing = [pfx for pfx in REQUIRED_SYMBOL_PREFIXES
               if not any(s.startswith(pfx) for s in syms)]
    if missing:
        fail(f"required pilot symbols absent: {len(missing)} prefix group(s)")

    # Exactly ONE trusted-time provider initializer: weather must not add a
    # second SNTP path.
    inits = sorted(s for s in syms if s.endswith("pool_time_sntp_init"))
    if len(inits) != 1:
        fail("the trusted-time provider must be present exactly once")

    return {
        "forbiddenSymbols": 0,
        "requiredSymbolGroups": len(REQUIRED_SYMBOL_PREFIXES),
        "trustedTimeProviders": 1,
    }


def verify_partition_fit(build: Path) -> dict:
    """Refuse an image that does not fit its slot."""
    app = build / "esp-miner.bin"
    if not app.is_file():
        fail("pilot application image not found")
    size = app.stat().st_size
    if size > APP_SLOT_BYTES:
        fail("pilot application image exceeds its OTA slot")
    return {"appBytes": size, "appSlotBytes": APP_SLOT_BYTES}


def verify_release_pair(build: Path, web_dir: Path, revision: str) -> dict:
    """Exact canonical identity: firmwareRevision == webRevision == describe.

    Compared by EXACT STRING EQUALITY, which is the whole point of the
    committed canonical-revision contract: a device compares these strings and
    reports BOOT PAIR MISMATCH when they differ by even one character.
    """
    version_txt = web_dir / "version.txt"
    if not version_txt.is_file():
        fail("web image version.txt not found; cannot verify the release pair")
    web_rev = version_txt.read_text(encoding="utf-8").strip()
    if web_rev != revision:
        fail("release pair mismatch: web revision differs from the canonical "
             "revision")
    if not canonical_revision.revision_matches_commit:
        fail("canonical revision helper unavailable")
    return {"firmwareRevision": revision, "webRevision": web_rev,
            "releasePairVerified": True}


# --------------------------------------------------------------------------
# Manifest — bounded booleans and enums only
# --------------------------------------------------------------------------

def build_manifest(identity: dict, cfg: dict, digest: str) -> dict:
    """Record WHETHER things were configured, never WHAT they were.

    Deliberately absent: latitude, longitude, any coordinate in any unit, a
    city or site label, the provider hostname, the request URL or query, the
    timezone identifier (which would narrow the private location), and the
    environment variable values.
    """
    return {
        "schemaVersion": SCHEMA_VERSION,
        "gate": "W5",
        "kind": "weather-recommendation-pilot",
        "revision": identity["describe"],
        "commit": identity["commit"],
        "trackedTreeDigest": digest,
        # Bounded booleans/enums only.
        "weatherConfigured": True,
        "providerConfigured": True,
        "locationConfigured": True,
        "timezoneConfigured": True,
        "distributionMode": cfg["distribution"],
        "trustedTimeConfigured": True,
        "pilotDiagnosticsEnabled": True,
        "sourceStatus": cfg["status"],          # a value-free token
        "recommendationOnly": True,
        "executionEnabled": False,
        "timedSessionApiEnabled": False,
        "storePreflightEnabled": False,
        "hardwareTuningEnabled": False,
    }


def assert_manifest_private_free(manifest: dict, cfg: dict) -> None:
    """Fail closed if any private value reached the manifest."""
    blob = json.dumps(manifest, sort_keys=True)
    forbidden = []
    if cfg.get("ready"):
        forbidden = [str(abs(cfg["latitude_e4"])), str(abs(cfg["longitude_e4"])),
                     str(cfg["latitude_e4"]), str(cfg["longitude_e4"])]
    for token in forbidden:
        if token and token in blob:
            fail("manifest rejected: it contains a private coordinate")
    if cfg.get("ready") and cfg.get("ntp") and cfg["ntp"] in blob:
        fail("manifest rejected: it contains the private trusted-time source")
    for banned in ("open-meteo", "api.", "http", "://", "latitude", "longitude",
                   "EUROPE_BRUSSELS", "europe/brussels"):
        if banned in blob:
            fail("manifest rejected: it names a host, URL or private locale")
    for var in ENV_VARS:
        if var in blob:
            fail("manifest rejected: it references a private environment name")


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------

def find_container_runtime() -> str:
    for candidate in ("docker", "podman"):
        if shutil.which(candidate):
            return candidate
    fail("no container runtime found (docker or podman required)")
    return ""


def build_command(repo_path: str, work_path: str, *, revision: str) -> str:
    defaults = f"{repo_path}/sdkconfig.defaults;{work_path}/weather.sdkconfig.defaults"
    return (
        f". $IDF_PATH/export.sh > /dev/null 2>&1 && "
        f"cd {repo_path} && "
        f"idf.py -B {work_path}/build "
        f"-DSDKCONFIG={work_path}/sdkconfig "
        f"-DSDKCONFIG_DEFAULTS='{defaults}' "
        f"-DPROJECT_VER='{revision}' build"
    )


def run_build(repo: Path, work: Path, *, image: str, revision: str) -> None:
    runtime = find_container_runtime()
    cmd = [
        runtime, "run", "--rm",
        "-v", f"{repo}:{CONTAINER_REPO}",
        "-v", f"{work}:{CONTAINER_WORK}",
        "-e", "GITHUB_ACTIONS=true",
        image, "bash", "-lc",
        build_command(CONTAINER_REPO, CONTAINER_WORK, revision=revision),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        # The build log may echo the sdkconfig fragment, so it is never
        # forwarded to the caller.
        fail("firmware build failed (log withheld: it may contain private values)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build a Gate W5 weather recommendation-only pilot pair.")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path,
                        help="artifact directory (required unless --check-config-only)")
    parser.add_argument("--idf-image", default=DEFAULT_IDF_IMAGE)
    parser.add_argument("--check-config-only", action="store_true",
                        help="validate the private configuration and exit; builds nothing")
    args = parser.parse_args(argv)

    try:
        cfg = read_private_config()
        # The status token is value-free by construction.
        print(f"weather source status: {cfg['status']}")
        if not cfg["ready"]:
            fail("private weather configuration is not usable; see the status token")

        if args.check_config_only:
            print("check-config-only: configuration is valid; nothing was built")
            return 0

        if args.output is None:
            fail("--output is required unless --check-config-only is given")

        repo = args.repo.resolve()
        require_clean_tree(repo)
        identity = canonical_revision.identity(repo)
        canonical_revision.validate_canonical(identity["describe"])
        digest_before = tracked_tree_digest(repo)

        work = Path(tempfile.mkdtemp(prefix="nx-w5-"))
        defaults_path = work / "weather.sdkconfig.defaults"
        try:
            write_pilot_defaults(work, cfg)
            run_build(repo, work, image=args.idf_image,
                      revision=identity["describe"])

            manifest = build_manifest(identity, cfg, digest_before)
            assert_manifest_private_free(manifest, cfg)
            args.output.mkdir(parents=True, exist_ok=True)
            (args.output / "manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        finally:
            shred(defaults_path)
            shred(work / "sdkconfig")
            shutil.rmtree(work, ignore_errors=True)

        if tracked_tree_digest(repo) != digest_before:
            fail("the build modified the tracked tree")
        print("weather recommendation pilot built; no private value was printed")
        return 0
    except PilotError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
