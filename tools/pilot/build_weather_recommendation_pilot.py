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

# Importing a sibling module writes tools/pilot/__pycache__, which
# `git status --porcelain` reports as untracked — so the FIRST run left the
# tree dirty and the SECOND run failed its own clean-tree gate before doing
# anything. The helper must not modify the repository merely by running.
sys.dont_write_bytecode = True

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

WEB_SRC_REL = "main/http_server/axe-os"
WEB_DIST_REL = "main/http_server/axe-os/dist"
WEB_DIST_INNER = "main/http_server/axe-os/dist/axe-os"

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
    "nx_tuning_apply_",          # any future tuning apply (Gate W6.3.1 emits
    "tuning_profile_apply",      # INPUTS to a decision, never an action)
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
    # Gate W6.3.1. Without these five the artifact could pass every other gate
    # while the whole W1 input chain had been garbage-collected out of it —
    # which is exactly the claim this gate makes about the flashed image.
    "nx_telemetry_safety_",      # Gate W6.3T-B coherent snapshot
    "tuning_classify_",          # Gate W6.3T-A sensor classifiers
    "nx_tuning_input_",          # Gate W6.3.1 projection
    "tuning_registry_",          # the production Gamma 601 registry
    "tuning_policy_",            # the committed W1 evaluator itself
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
        # Gate W6.3.2. The explicit authorization that lets the committed
        # Brussels schedule become DUE. Without it the pilot is CONFIGURED but
        # not AUTHORIZED: it composes no request, the W6.3 worker is never
        # asked to fetch and the schedule evaluates to DISABLED forever. It is
        # written ONLY here, so an ordinary build cannot acquire it.
        "CONFIG_NX_WEATHER_PILOT_SCHEDULE=y",
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
    # Gate W6.3.2 schedule authorization, verified from the GENERATED header
    # rather than from our own fragment: the fragment states intent, only
    # sdkconfig.h proves the compiler agreed (the symbol depends on the pilot
    # diagnostics flag, so a mis-ordered fragment could silently drop it).
    "CONFIG_NX_WEATHER_PILOT_SCHEDULE",
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


def _parse_nm(text: str) -> set[str]:
    out = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            out.add(parts[-1])
    return out


def elf_symbols(elf: Path, *, work: Path | None = None,
                image: str = DEFAULT_IDF_IMAGE) -> set[str]:
    """Read the defined symbols of a cross-compiled ELF.

    The xtensa toolchain lives INSIDE the build image, not on the host: on a
    Windows or bare host `xtensa-esp32s3-elf-nm` simply does not exist, and
    invoking it raised FileNotFoundError straight out of subprocess — an
    unhandled traceback immediately after a SUCCESSFUL firmware build. The host
    is tried first (it is faster when a toolchain is present) and the container
    is the fallback that always works, because it is the same image that just
    produced the ELF.
    """
    for nm in ("xtensa-esp32s3-elf-nm", "nm"):
        try:
            proc = subprocess.run([nm, "-g", "--defined-only", str(elf)],
                                  capture_output=True, text=True)
        except (FileNotFoundError, OSError):
            continue          # not on this host; try the next candidate
        if proc.returncode == 0:
            return _parse_nm(proc.stdout)

    if work is not None:
        try:
            runtime = find_container_runtime()
        except PilotError:
            runtime = ""
        if runtime:
            rel = elf.relative_to(work).as_posix()
            proc = subprocess.run(
                [runtime, "run", "--rm", "-v", f"{work}:{CONTAINER_WORK}", image,
                 "bash", "-lc",
                 f". $IDF_PATH/export.sh > /dev/null 2>&1 && "
                 f"xtensa-esp32s3-elf-nm -g --defined-only {CONTAINER_WORK}/{rel}"],
                capture_output=True, text=True)
            if proc.returncode == 0:
                return _parse_nm(proc.stdout)

    fail("no usable nm found for the ELF symbol audit, on the host or in the "
         "build image")
    return set()


def verify_symbols(elf: Path, *, work: Path | None = None,
                   image: str = DEFAULT_IDF_IMAGE) -> dict:
    """Prove at the binary level what this pilot may and may not contain."""
    syms = elf_symbols(elf, work=work, image=image)

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


APP_DESC_MAGIC = b"\x32\x54\xcd\xab"
WWW_REVISION_PATTERN = re.compile(
    rb"v\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]{7,12})?(?:-dirty)?")


def read_app_desc_version(path: Path) -> str:
    """The revision the FIRMWARE actually embeds, from its app descriptor."""
    if not path.is_file():
        fail("the built firmware image is missing")
    with path.open("rb") as handle:
        header = handle.read(0x60)
    if len(header) < 0x60 or header[0x20:0x24] != APP_DESC_MAGIC:
        fail("esp_app_desc_t magic not found — not an ESP-IDF app image?")
    return header[0x30:0x50].split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def read_embedded_www_revision(www_bin: Path) -> str:
    """The revision the packed WEB IMAGE actually embeds.

    Read from the generated SPIFFS image rather than from the build host's
    dist directory, which can be stale or newer than what was packed.
    """
    if not www_bin.is_file():
        fail("the generated web image is missing")
    matches = sorted({m.group(0).decode("ascii")
                      for m in WWW_REVISION_PATTERN.finditer(www_bin.read_bytes())})
    if not matches:
        fail("no embedded web revision found in the generated www image")
    if len(matches) > 1:
        fail("the generated www image embeds multiple distinct revisions — "
             "ambiguous web identity")
    return matches[0]


def verify_release_pair(build: Path, revision: str) -> dict:
    """THE gate: firmwareRevision == webRevision == canonicalGitDescribe.

    Both sides are read from the BUILT ARTEFACTS, never assumed from the value
    that was passed in. Compared by EXACT STRING EQUALITY — a device compares
    these strings and reports BOOT PAIR MISMATCH when they differ by even one
    character, so a prefix test would not do.
    """
    fw = read_app_desc_version(build / "esp-miner.bin")
    web = read_embedded_www_revision(build / "www.bin")

    if "-dirty" in fw or "-dirty" in web:
        fail("a built image embeds a dirty identity")
    if fw != web:
        fail(f"BOOT PAIR MISMATCH would ship: firmware '{fw}' != web '{web}'")
    if fw != revision:
        fail(f"the built firmware '{fw}' does not match the canonical "
             f"revision '{revision}'")
    if web != revision:
        fail(f"the packed web image '{web}' does not match the canonical "
             f"revision '{revision}'")

    # Agreement is necessary but not sufficient: both could agree on a
    # non-canonical shape. Pin the shape too.
    for value in (fw, web):
        try:
            canonical_revision.validate_canonical(value)
        except canonical_revision.RevisionError as exc:
            fail(f"a built identity is not canonical: {exc}")
    return {"firmwareRevision": fw, "webRevision": web,
            "releasePairVerified": True}


# --------------------------------------------------------------------------
# Manifest — bounded booleans and enums only
# --------------------------------------------------------------------------

def build_manifest(identity: canonical_revision.BuildIdentity, cfg: dict,
                   digest: str, pair: dict | None = None,
                   artifacts: list | None = None) -> dict:
    """Record WHETHER things were configured, never WHAT they were.

    Deliberately absent: latitude, longitude, any coordinate in any unit, a
    city or site label, the provider hostname, the request URL or query, the
    timezone identifier (which would narrow the private location), and the
    environment variable values.
    """
    # ONE canonical revision, recorded under every name a consumer may look
    # for. When the release pair has been verified against the built images,
    # the manifest carries the values READ FROM THEM; otherwise it carries the
    # canonical revision for all three, and they are equal by construction.
    fw_rev = pair["firmwareRevision"] if pair else identity.revision
    web_rev = pair["webRevision"] if pair else identity.revision
    if fw_rev != web_rev or fw_rev != identity.revision:
        fail("refusing to write a manifest whose revisions disagree")

    return {
        "schemaVersion": SCHEMA_VERSION,
        "gate": "W6",
        "kind": "weather-recommendation-pilot",
        # Identity: the full commit, never abbreviated, plus the ONE canonical
        # revision that both images embed.
        "gitCommit": identity.commit,
        "canonicalGitDescribe": identity.revision,
        "firmwareRevision": fw_rev,
        "webRevision": web_rev,
        "releasePairVerified": bool(pair),
        "webImageFreshlyBuilt": True,
        "branch": identity.branch,
        "trackedTreeDigest": digest,
        # Target.
        "boardVersion": 601,
        "asicModel": "BM1370",
        # The complete deliverable set, each with its size and SHA-256 taken
        # from the STAGED COPY.
        "artifacts": artifacts or [],
        # Bounded booleans/enums only.
        "weatherConfigured": True,
        "providerConfigured": True,
        "locationConfigured": True,
        "timezoneConfigured": True,
        "distributionMode": cfg["distribution"],
        "trustedTimeConfigured": True,
        "pilotDiagnosticsEnabled": True,
        # Gate W6.3.2. CONFIGURED and AUTHORIZED are separate facts: the four
        # *Configured booleans above say where weather would come from, this
        # one says the owner permitted this image to ask. A bounded boolean —
        # it names no slot time, no window and no timezone.
        "scheduleEnabled": True,
        "sourceStatus": cfg["status"],          # a value-free token
        "recommendationOnly": True,
        "executionEnabled": False,
        "timedSessionApiEnabled": False,
        "storePreflightEnabled": False,
        "hardwareTuningEnabled": False,
        "mutationObservabilityEnabled": True,
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

def resolve_identity(repo: Path) -> canonical_revision.BuildIdentity:
    """THE single source of build identity. Never recomputed here.

    Everything comes from the shared canonical_revision contract through its
    TYPED accessor, so a field this helper does not have is an AttributeError
    at the point of misuse rather than a KeyError deep inside a build. Any
    failure — a missing tag, an unreadable repository, a contract change —
    becomes a bounded PilotError instead of a traceback.
    """
    try:
        identity = canonical_revision.build_identity(repo)
        identity.require_clean()
        canonical_revision.validate_canonical(identity.revision)
        if not canonical_revision.revision_matches_commit(identity.revision,
                                                          identity.commit):
            fail("the canonical revision does not name HEAD")
    except canonical_revision.RevisionError as exc:
        fail(f"canonical identity unavailable: {exc}")
    except (AttributeError, KeyError, TypeError) as exc:
        # A changed canonical-revision contract must surface as a controlled
        # failure naming the field, never as a raw traceback mid-build.
        fail(f"the canonical-revision contract is not the expected shape: "
             f"{type(exc).__name__}: {exc}")
    return identity


def remove_stale_web_output(repo: Path) -> None:
    """Delete any pre-existing dist so a stale identity cannot be packaged.

    The firmware build runs with GITHUB_ACTIONS=true, which makes CMake pack
    the PREBUILT dist rather than rebuild it. Without this removal the image
    could carry a web revision from an earlier commit — precisely the
    mismatched pair the canonical-revision contract exists to prevent.
    """
    dist = repo / WEB_DIST_REL
    if dist.exists():
        shutil.rmtree(dist, ignore_errors=True)


def build_frontend(repo: Path, revision: str, npm_install: str = "ci") -> str:
    """Build the production frontend FRESH and return its version.txt.

    `generate-version.js` is handed the EXACT canonical revision through the
    shared environment variable, so neither the web nor the firmware side
    derives its own abbreviation.
    """
    remove_stale_web_output(repo)

    npm = shutil.which("npm") or shutil.which("npm.cmd")
    if npm is None:
        fail("npm was not found in PATH; the pilot package requires a freshly "
             "built web image and will not reuse an existing one")

    web = repo / WEB_SRC_REL
    if not web.is_dir():
        fail("the frontend source directory is missing")

    if npm_install != "skip":
        print(f"building the web UI: npm {npm_install}")
        if subprocess.run([npm, npm_install], cwd=str(web)).returncode != 0:
            fail(f"npm {npm_install} failed")

    env = {**os.environ, canonical_revision.REVISION_ENV: revision}
    print("building the web UI: npm run build (production)")
    if subprocess.run([npm, "run", "build"], cwd=str(web), env=env).returncode != 0:
        fail("the production web build failed")

    version_txt = repo / WEB_DIST_INNER / "version.txt"
    if not version_txt.is_file():
        fail("the web build produced no version.txt; refusing to package an "
             "image with no identity")
    built = version_txt.read_text(encoding="utf-8").strip()
    if not built:
        fail("the web build produced an empty version.txt")
    if "-dirty" in built:
        fail("the web build embeds a dirty identity")
    if built != revision:
        fail("the freshly built web revision does not match the canonical "
             "revision — refusing to package a mismatched pair")
    return built


# --------------------------------------------------------------------------
# Package staging
#
# WHY THIS EXISTS. An earlier revision wrote manifest.json into the output
# directory and printed success — while copying NO binary at all. Worse, the
# finally block removed the whole work tree, so esp-miner.bin and www.bin were
# destroyed moments after being verified. The result was a package that
# reported "built" and could not be flashed.
#
# Staging is therefore atomic: everything is assembled in a temporary
# directory beside the requested output, every postcondition is proven against
# the STAGED COPIES, and only a complete package is published. Success is
# printed last, after the PUBLISHED directory has been re-opened and re-proven.
# --------------------------------------------------------------------------

#: The bounded suffix every pilot artifact filename carries.
ARTIFACT_SUFFIX = "weather-pilot-601-BM1370"

ARTIFACT_OTA = "ota-application"
ARTIFACT_WWW = "www-spiffs"

MANIFEST_NAME = "manifest.json"
SHA256SUMS_NAME = "SHA256SUMS.txt"

#: Exactly what a complete package contains — nothing more, nothing less.
def package_filenames(revision: str) -> dict:
    base = f"NeuralAxe-OS-{revision}-{ARTIFACT_SUFFIX}"
    return {ARTIFACT_OTA: f"{base}-ota.bin", ARTIFACT_WWW: f"{base}-www.bin"}


def expected_package_files(revision: str) -> set:
    names = package_filenames(revision)
    return {names[ARTIFACT_OTA], names[ARTIFACT_WWW],
            MANIFEST_NAME, SHA256SUMS_NAME}


#: Deliberately NOT the factory image, the merged image, the bootloader, the
#: partition table, the OTA-data image or any NVS blob: a recommendation-only
#: pilot is delivered over the NVS-preserving OTA route, and shipping a
#: factory image would invite an erase that destroys the owner's settings.
ARTIFACT_SOURCES = ((ARTIFACT_OTA, "esp-miner.bin"), (ARTIFACT_WWW, "www.bin"))


def sha256_file(path: Path) -> str:
    """Hash a file in bounded chunks. Used on the STAGED COPY, never only the
    source, so a truncated or failed copy cannot inherit a correct hash."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage_package(staging: Path, build_dir: Path,
                  identity: canonical_revision.BuildIdentity, cfg: dict,
                  digest: str, pair: dict) -> dict:
    """Assemble the complete package in `staging`. Returns the manifest."""
    staging.mkdir(parents=True, exist_ok=False)
    names = package_filenames(identity.revision)

    artifacts = []
    for kind, source_name in ARTIFACT_SOURCES:
        source = build_dir / source_name
        if not source.is_file():
            fail(f"the build produced no {source_name}; refusing to publish an "
                 f"incomplete package")
        if source.stat().st_size == 0:
            fail(f"the build produced an empty {source_name}")
        target = staging / names[kind]
        shutil.copyfile(source, target)
        if not target.is_file():
            fail(f"staging {kind} failed: the copy does not exist")
        # Hash the COPY. A short read or a full disk shows up here, not later.
        size = target.stat().st_size
        if size != source.stat().st_size:
            fail(f"staging {kind} failed: the copy is a different size")
        artifacts.append({
            "filename": names[kind],
            "type": kind,
            "sizeBytes": size,
            "sha256": sha256_file(target),
        })

    lines = [f"{a['sha256']}  {a['filename']}" for a in artifacts]
    (staging / SHA256SUMS_NAME).write_text("\n".join(lines) + "\n",
                                           encoding="utf-8")

    manifest = build_manifest(identity, cfg, digest, pair, artifacts)
    assert_manifest_private_free(manifest, cfg)
    (staging / MANIFEST_NAME).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def read_sha256sums(path: Path) -> dict:
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        parts = line.split("  ", 1)
        if len(parts) != 2:
            fail("SHA256SUMS.txt is malformed")
        out[parts[1].strip()] = parts[0].strip()
    return out


def verify_package(pkg: Path, identity: canonical_revision.BuildIdentity,
                   cfg: dict | None = None) -> dict:
    """Re-open a package directory and prove it is complete and consistent.

    Everything here is read back FROM DISK. It is deliberately independent of
    whatever the staging step believed it wrote, because the whole point is to
    catch a package that only looks finished.
    """
    if not pkg.is_dir():
        fail("the package directory does not exist")
    present = {p.name for p in pkg.iterdir()}
    expected = expected_package_files(identity.revision)

    missing = sorted(expected - present)
    if missing:
        fail(f"incomplete package: missing {len(missing)} required file(s): "
             f"{', '.join(missing)}")
    unexpected = sorted(present - expected)
    if unexpected:
        fail(f"unexpected file(s) in the package: {', '.join(unexpected)}")
    if len(present) != len(expected):
        fail(f"package file count is {len(present)}, expected {len(expected)}")

    manifest = json.loads((pkg / MANIFEST_NAME).read_text(encoding="utf-8"))
    sums = read_sha256sums(pkg / SHA256SUMS_NAME)
    entries = manifest.get("artifacts") or []
    if len(entries) != len(ARTIFACT_SOURCES):
        fail("the manifest records no complete artifact set")

    names = package_filenames(identity.revision)
    for entry in entries:
        name = entry.get("filename", "")
        blob = pkg / name
        if not blob.is_file():
            fail(f"the manifest names a file that is not in the package: {name}")
        size = blob.stat().st_size
        if size == 0:
            fail(f"{name} is empty")
        if size != entry.get("sizeBytes"):
            fail(f"{name}: size on disk differs from the manifest")
        actual = sha256_file(blob)
        if actual != entry.get("sha256"):
            fail(f"{name}: SHA-256 on disk differs from the manifest")
        if sums.get(name) != actual:
            fail(f"{name}: SHA-256 on disk differs from {SHA256SUMS_NAME}")

    # The published images must still carry the canonical revision.
    fw = read_app_desc_version(pkg / names[ARTIFACT_OTA])
    web = read_embedded_www_revision(pkg / names[ARTIFACT_WWW])
    if fw != identity.revision or web != identity.revision:
        fail("a published image does not carry the canonical revision")
    if manifest.get("firmwareRevision") != fw or \
            manifest.get("webRevision") != web:
        fail("the manifest revisions disagree with the published images")

    # No private value in any retained text file.
    for text_name in (MANIFEST_NAME, SHA256SUMS_NAME):
        blob = (pkg / text_name).read_text(encoding="utf-8")
        assert_text_private_free(blob, cfg)
    return manifest


def assert_text_private_free(text: str, cfg: dict | None) -> None:
    """Fail closed if a private value reached a retained text file.

    The forbidden set mirrors the committed manifest contract exactly: the
    coordinates, the trusted-time source, the provider and the timezone are
    private. `distributionMode` is NOT — it is an approved bounded enum the
    Gate W5 manifest is specified to carry, and treating it as a leak would
    make the guard reject a correct package.
    """
    if not cfg or not cfg.get("ready"):
        return
    forbidden = [str(abs(cfg["latitude_e4"])), str(abs(cfg["longitude_e4"])),
                 str(cfg["latitude_e4"]), str(cfg["longitude_e4"]),
                 cfg["ntp"], cfg["provider"], cfg["timezone"]]
    for value in forbidden:
        if value and str(value) in text:
            fail("a private value reached a retained package file")
    for banned in ("open-meteo", "api.", "http", "://", "latitude", "longitude",
                   "europe/brussels"):
        if banned in text:
            fail("a retained package file names a host, URL or private locale")
    for name in ENV_VARS:
        if name in text:
            fail("a private environment variable name reached a package file")


def publish_package(staging: Path, output: Path) -> None:
    """Atomically move a COMPLETE staged package into place.

    A pre-existing output directory is replaced only when it holds nothing but
    pilot-package files; anything else is the owner's and is never removed.
    """
    if output.exists():
        present = {p.name for p in output.iterdir()}
        foreign = {n for n in present
                   if not (n.endswith(".bin") or n in (MANIFEST_NAME,
                                                       SHA256SUMS_NAME))}
        if foreign:
            fail("the output directory holds files this helper did not create; "
                 "refusing to replace it")
        shutil.rmtree(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    os.replace(staging, output)


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


# --------------------------------------------------------------------------
# Bounded, sanitized build diagnostics
#
# WHY THIS EXISTS. An earlier revision discarded the firmware build log
# entirely — "log withheld: it may contain private values" — on the grounds
# that it may echo the sdkconfig fragment. That is true of a handful of lines
# and false of the rest, and the result was a build failure nobody could
# diagnose: not the owner, who must not paste an unredacted log, and not a
# reviewer, who never sees it at all. Most build failures (a stopped container
# daemon, an unshared mount, a compile error) carry no private value whatever.
#
# The log is therefore captured to an EPHEMERAL file outside the repository,
# reduced to the few lines that establish the failure, sanitized field by
# field, and the raw file is removed in a finally.
# --------------------------------------------------------------------------

#: Every build stage the classifier can name, most specific first.
BUILD_STAGES = (
    ("container-runtime", re.compile(
        r"docker api|cannot connect to the docker|daemon is running|"
        r"error during connect|permission denied while trying to connect|"
        r"is not shared from the host|invalid mount|no such image|"
        r"pull access denied", re.I)),
    ("spiffs-image", re.compile(
        r"spiffs_\w*bin|spiffs_create_partition|given base directory", re.I)),
    ("partition-check", re.compile(
        r"does not fit|exceeds the partition|partition table|binary size .*"
        r"larger", re.I)),
    ("link", re.compile(r"undefined reference|ld returned|multiple definition|"
                        r"region \S+ overflowed", re.I)),
    ("compile", re.compile(r"\berror:|\bfatal error\b", re.I)),
    ("kconfig", re.compile(r"Kconfig[\w.]*:\d+|invalid symbol|"
                           r"unknown config symbol|recursive dependency", re.I)),
    ("cmake-configure", re.compile(r"CMake Error|configure step failed", re.I)),
)

#: Lines worth keeping when reducing a failed log.
DIAGNOSTIC_LINE = re.compile(
    r"\berror\b|FAILED|CMake Error|undefined reference|ninja: |"
    r"RuntimeError|Traceback|fatal|does not exist|no such file|"
    r"cannot connect|denied|overflowed|does not fit", re.I)

MAX_DIAGNOSTIC_LINES = 12
MAX_DIAGNOSTIC_WIDTH = 200


def sanitize_line(line: str, cfg: dict | None = None) -> str:
    """Remove every private value from one build-log line.

    Redacts, in this order: the owner's configured values, ANY sdkconfig
    assignment (so a symbol this function has never heard of still cannot leak
    its value), the private fragment's path, and the temporary work path.
    """
    out = line
    if cfg:
        for key in ("ntp", "provider_input", "timezone_input",
                    "latitude_input", "longitude_input", "distribution_input"):
            value = cfg.get(key)
            if value:
                out = out.replace(str(value), "<redacted>")
        for key in ("latitude_e4", "longitude_e4"):
            value = cfg.get(key)
            if value is not None:
                for form in (str(value), str(abs(value))):
                    out = out.replace(form, "<redacted>")
    # Any CONFIG_* assignment, quoted or bare — value never survives.
    out = re.sub(r"(CONFIG_[A-Z0-9_]+)\s*=\s*(\"[^\"]*\"|[^\s,;)]+)",
                 r"\1=<redacted>", out)
    # The private fragment and the temporary work tree.
    out = re.sub(r"\S*weather\.sdkconfig\.defaults", "<private-fragment>", out)
    out = re.sub(re.escape(CONTAINER_WORK) + r"\S*", "<work>", out)
    out = re.sub(r"[A-Za-z]:[\\/][^\s:]*[\\/]nx-w6-\S*", "<work>", out)
    return out[:MAX_DIAGNOSTIC_WIDTH]


def classify_build_failure(text: str) -> str:
    """Name the stage a failed build reached. Total; never raises."""
    for stage, pattern in BUILD_STAGES:
        if pattern.search(text):
            return stage
    return "unknown"


def summarize_build_failure(log_text: str, cfg: dict | None = None) -> dict:
    """A bounded, sanitized diagnostic. Never the complete raw log."""
    lines = log_text.splitlines()
    hits = [l for l in lines if DIAGNOSTIC_LINE.search(l)]
    if not hits:                      # no recognised marker: keep the tail
        hits = lines[-MAX_DIAGNOSTIC_LINES:]
    kept = [sanitize_line(l.rstrip(), cfg) for l in hits[-MAX_DIAGNOSTIC_LINES:]]
    source = ""
    for line in hits:
        match = re.search(r"([\w./-]+\.(?:c|cpp|h|py|txt|cmake))[:(]", line)
        if match:
            # The basename only: a path could name a private work tree, and
            # the file name is what a reader actually needs.
            source = match.group(1).rsplit("/", 1)[-1].rsplit(chr(92), 1)[-1]
            break
    return {
        # Classified from the kept evidence: scanning the whole log would let
        # an incidental mention outrank the line that actually failed.
        "stage": classify_build_failure(chr(10).join(hits[-MAX_DIAGNOSTIC_LINES:])),
        "sourceFile": source,
        "lines": kept,
        "privateValuesRedacted": True,
    }


def report_build_failure(summary: dict) -> None:
    """Print the bounded diagnostic, then fail with a bounded message."""
    print(f"firmware build failed at stage: {summary['stage']}", file=sys.stderr)
    if summary["sourceFile"]:
        print(f"  source: {summary['sourceFile']}", file=sys.stderr)
    for line in summary["lines"]:
        print(f"  | {line}", file=sys.stderr)
    print(f"  private-values-redacted={str(summary['privateValuesRedacted']).lower()}",
          file=sys.stderr)
    fail(f"firmware build failed during {summary['stage']}")


def run_build(repo: Path, work: Path, *, image: str, revision: str,
              cfg: dict | None = None) -> None:
    """Build the firmware. On failure, report a bounded sanitized diagnostic.

    The complete output goes to an EPHEMERAL file outside the repository so it
    can be reduced and sanitized; that file is removed in the finally, so no
    raw log is ever retained.
    """
    runtime = find_container_runtime()
    cmd = [
        runtime, "run", "--rm",
        "-v", f"{repo}:{CONTAINER_REPO}",
        "-v", f"{work}:{CONTAINER_WORK}",
        "-e", "GITHUB_ACTIONS=true",
        image, "bash", "-lc",
        build_command(CONTAINER_REPO, CONTAINER_WORK, revision=revision),
    ]
    log_path = work / "firmware-build.log"
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
        log_path.write_text((proc.stdout or "") + (proc.stderr or ""),
                            encoding="utf-8", errors="replace")
        if proc.returncode != 0:
            report_build_failure(
                summarize_build_failure(
                    log_path.read_text(encoding="utf-8", errors="replace"), cfg))
    finally:
        # The raw log never outlives the call, on success or failure.
        shred(log_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build a Gate W5 weather recommendation-only pilot pair.")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path,
                        help="artifact directory (required unless --check-config-only)")
    parser.add_argument("--idf-image", default=DEFAULT_IDF_IMAGE)
    parser.add_argument("--check-config-only", action="store_true",
                        help="validate the private configuration and exit; builds nothing")
    parser.add_argument("--npm-install", choices=("ci", "install", "skip"),
                        default="ci",
                        help="how to prepare frontend dependencies before the fresh web build")
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
        identity = resolve_identity(repo)
        digest_before = tracked_tree_digest(repo)

        output = args.output.resolve()
        staging = output.parent / f"{output.name}.staging-{os.getpid()}"
        work: Path | None = None
        built_frontend = False
        published = False
        try:
            # 1-2. Build the web image FRESH and the firmware, both from the
            #      one canonical revision.
            build_frontend(repo, identity.revision, args.npm_install)
            built_frontend = True

            work = Path(tempfile.mkdtemp(prefix="nx-w6-"))
            write_pilot_defaults(work, cfg)
            run_build(repo, work, image=args.idf_image,
                      revision=identity.revision, cfg=cfg)
            build_dir = work / "build"

            # 3-6. Prove the posture BEFORE anything is copied anywhere.
            verify_sdkconfig_h(build_dir)
            verify_symbols(build_dir / "esp-miner.elf", work=work,
                           image=args.idf_image)
            verify_partition_fit(build_dir)
            pair = verify_release_pair(build_dir, identity.revision)
            print(f"release pair verified: firmware == web == "
                  f"{pair['firmwareRevision']}")

            # 7-10. Stage the COMPLETE package beside the requested output:
            #       copy both binaries, hash the copies, write SHA256SUMS.txt
            #       and the manifest with artifact entries.
            shutil.rmtree(staging, ignore_errors=True)
            stage_package(staging, build_dir, identity, cfg, digest_before, pair)

            # 11. Prove the staged package is complete before publishing it.
            verify_package(staging, identity, cfg)

            # 12. Publish atomically.
            publish_package(staging, output)
            published = True
        finally:
            # Never touches a PUBLISHED package. Removes, in order: the private
            # fragment, the helper-owned firmware work tree, an unpublished
            # staging directory, and the helper-owned frontend output.
            if work is not None:
                shred(work / "weather.sdkconfig.defaults")
                shred(work / "sdkconfig")
                shutil.rmtree(work, ignore_errors=True)
            if not published:
                shutil.rmtree(staging, ignore_errors=True)
                if built_frontend:
                    remove_stale_web_output(repo)

        if tracked_tree_digest(repo) != digest_before:
            fail("the build modified the tracked tree")

        # 13. Re-open the PUBLISHED directory and prove it again, then and only
        #     then report success. Verifying the staging copy is not enough:
        #     the publish step itself must be proven to have delivered.
        manifest = verify_package(output, identity, cfg)
        for entry in manifest["artifacts"]:
            print(f"  {entry['filename']}  {entry['sizeBytes']} bytes  "
                  f"sha256={entry['sha256'][:16]}...")
        print(f"PILOT BUILD OK: {len(expected_package_files(identity.revision))} "
              f"files published; no private value was printed")
        return 0
    except PilotError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
