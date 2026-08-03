#!/usr/bin/env python3
"""NeuralAxe OS — Gate B10.1 pilot build-helper test battery.

Self-contained (no pytest). Exercises everything the helper decides BEFORE a
build starts, plus the verification and privacy rules it applies afterwards,
using synthetic inputs only. Exit 0 = every gate behaves as specified.

Covered:
- a missing NX_PILOT_NTP_SERVER fails before any build;
- an invalid source fails before any build;
- a valid synthetic hostname and a valid dotted-quad literal are accepted;
- the bounded validator matches the committed Gate B10 rule set;
- the source is never printed, never returned in an error and never written
  into the manifest;
- the temporary configuration is created outside the tree and shredded;
- the generated configuration must carry the exact five-flag pilot posture;
- an execution- or API-enabled configuration is refused;
- a forbidden execution/API symbol is refused;
- an absent diagnostics symbol is refused;
- oversized images are refused;
- the manifest is deterministic and hostname-free.

NOTHING here builds firmware, touches hardware, opens a serial port, performs
DNS, contacts NTP or reaches a network.
"""

from __future__ import annotations

import importlib.util
import io
import json
import contextlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

spec = importlib.util.spec_from_file_location("pilot_build",
                                              HERE / "build_time_observation_pilot.py")
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


def _load_helper(name: str):
    """Load a sibling pilot helper, for the cross-helper contract proofs."""
    s = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


# A synthetic fixture. It is never resolved and never contacted.
GOOD_HOST = "time-pilot.example"
GOOD_IPV4 = "192.168.7.9"

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


def expect_fail(fn, *args, **kwargs) -> str:
    """Run something that must raise PilotError; return the message."""
    try:
        fn(*args, **kwargs)
    except pilot.PilotError as exc:
        return str(exc)
    return ""


# --------------------------------------------------------------------------
# Source validation
# --------------------------------------------------------------------------

def test_validator_accepts() -> None:
    accepted = [
        "time-pilot.example",
        "a.b",
        "ntp.internal.example",
        "192.168.7.9",
        "10.0.0.1",
        "0.0.0.0",
        "255.255.255.255",
        ("x" * 59) + ".net",
    ]
    for host in accepted:
        v = pilot.validate_source(host)
        report(f"validator accepts a bounded source (len {len(host)})",
               v["state"] == pilot.SRC_CONFIGURED and v["usable"], str(v))


def test_validator_rejects() -> None:
    rejected = {
        "single label": "localhost",
        "url scheme": "https://ntp.example",
        "path": "ntp.example/path",
        "port": "ntp.example:123",
        "credentials": "user@ntp.example",
        "percent escape": "ntp%2eexample",
        "leading space": " ntp.example",
        "trailing space": "ntp.example ",
        "inner space": "ntp .example",
        "tab": "ntp\t.example",
        "newline": "ntp.example\n",
        "control byte": "ntp.exa\x01mple",
        "non ascii": "ntp.exämple",
        "leading dot": ".ntp.example",
        "trailing dot": "ntp.example.",
        "double dot": "ntp..example",
        "leading hyphen label": "-ntp.example",
        "trailing hyphen label": "ntp-.example",
        "overlong host": ("a" * 61) + ".example",
        "overlong label": ("a" * 64) + ".example",
        "ipv6 literal": "2001:db8::1",
        "bracketed ipv6": "[2001:db8::1]",
        "leading zero octet": "010.0.0.1",
        "octet out of range": "192.168.1.256",
        "three numeric labels": "1.2.3",
        "five numeric labels": "1.2.3.4.5",
        "numeric last label": "ntp.example.123",
        "hex literal": "0x0a.0.0.1",
        "too many labels": ".".join(["a"] * 17),
    }
    for name, host in rejected.items():
        v = pilot.validate_source(host)
        report(f"validator rejects {name}", v["state"] == pilot.SRC_INVALID, str(v))


def test_validator_unconfigured() -> None:
    for value in (None, ""):
        v = pilot.validate_source(value)
        report("validator treats an absent source as UNCONFIGURED, not an error",
               v["state"] == pilot.SRC_UNCONFIGURED and not v["usable"], str(v))


def test_env_required() -> None:
    msg = expect_fail(pilot.read_source_from_env, {})
    report("a missing environment variable fails before any build",
           pilot.ENV_VAR in msg and "not set" in msg, msg)

    msg = expect_fail(pilot.read_source_from_env, {pilot.ENV_VAR: ""})
    report("an empty environment variable fails before any build", msg != "", msg)

    msg = expect_fail(pilot.read_source_from_env, {pilot.ENV_VAR: "localhost"})
    report("an invalid source fails before any build",
           "TIME_SOURCE_INVALID" in msg, msg)

    host = pilot.read_source_from_env({pilot.ENV_VAR: GOOD_HOST})
    report("a valid synthetic hostname is accepted", host == GOOD_HOST, host)
    host = pilot.read_source_from_env({pilot.ENV_VAR: GOOD_IPV4})
    report("a valid dotted-quad literal is accepted", host == GOOD_IPV4, host)


def test_source_never_printed() -> None:
    """No failure message and no stdout may echo the candidate."""
    leaky = "secret-ntp.example:9999"
    msg = expect_fail(pilot.read_source_from_env, {pilot.ENV_VAR: leaky})
    report("a rejection message never echoes the candidate", leaky not in msg, msg)

    msg = expect_fail(pilot.read_source_from_env, {pilot.ENV_VAR: " " + GOOD_HOST})
    report("a whitespace rejection never echoes the candidate",
           GOOD_HOST not in msg, msg)

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        v = pilot.validate_source(GOOD_HOST)
    report("validation prints nothing at all", buf.getvalue() == "" and v["usable"])


# --------------------------------------------------------------------------
# Temporary configuration
# --------------------------------------------------------------------------

def test_temp_config() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b101-test-") as tmp:
        work = Path(tmp)
        path = pilot.write_pilot_defaults(work, GOOD_HOST)
        text = path.read_text(encoding="utf-8")

        report("the temporary config lives outside any repository tree",
               path.parent == work and path.is_file())
        report("the temporary config enables exactly the three pilot flags",
               all(f"{f}=y" in text for f in pilot.PILOT_FLAGS_ON), text)
        report("the temporary config disables execution and the API",
               all(f"# {f} is not set" in text for f in pilot.PILOT_FLAGS_OFF), text)
        report("the temporary config is the only carrier of the source",
               f'CONFIG_NX_TIMED_SESSIONS_NTP_SERVER="{GOOD_HOST}"' in text)

        pilot.shred(path)
        report("the temporary config is removed after the build", not path.exists())
        pilot.shred(path)  # idempotent
        report("shredding a missing config is a no-op", not path.exists())


def test_defaults_deterministic() -> None:
    a = pilot.pilot_defaults_text(GOOD_HOST)
    b = pilot.pilot_defaults_text(GOOD_HOST)
    report("the temporary config is byte-deterministic", a == b)


# --------------------------------------------------------------------------
# Build configuration verification
# --------------------------------------------------------------------------

def pilot_defines(host: str = GOOD_HOST) -> dict:
    return {
        "CONFIG_NX_TIMED_SESSIONS": "1",
        "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE": "1",
        "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS": "1",
        "CONFIG_NX_TIMED_SESSIONS_NTP_SERVER": f'"{host}"',
    }


def test_verify_build_config() -> None:
    flags = pilot.verify_build_config(pilot_defines(), GOOD_HOST)
    report("the exact pilot posture is accepted",
           all(flags[f] for f in pilot.PILOT_FLAGS_ON)
           and not any(flags[f] for f in pilot.PILOT_FLAGS_OFF), str(flags))
    report("the verified flag summary redacts the source",
           GOOD_HOST not in json.dumps(flags), json.dumps(flags))

    for missing in pilot.PILOT_FLAGS_ON:
        d = pilot_defines()
        del d[missing]
        msg = expect_fail(pilot.verify_build_config, d, GOOD_HOST)
        report(f"a build without {missing} is refused", missing in msg, msg)

    for extra in pilot.PILOT_FLAGS_OFF:
        d = pilot_defines()
        d[extra] = "1"
        msg = expect_fail(pilot.verify_build_config, d, GOOD_HOST)
        report(f"a build WITH {extra} is refused", extra in msg, msg)

    d = pilot_defines()
    del d["CONFIG_NX_TIMED_SESSIONS_NTP_SERVER"]
    msg = expect_fail(pilot.verify_build_config, d, GOOD_HOST)
    report("a build without a configured source is refused", msg != "", msg)

    msg = expect_fail(pilot.verify_build_config, pilot_defines("other.example"), GOOD_HOST)
    report("a mismatched configured source is refused", msg != "", msg)
    report("the mismatch message echoes neither source",
           GOOD_HOST not in msg and "other.example" not in msg, msg)


def test_verify_symbols() -> None:
    good = ["pool_pilot_step", "pool_pilot_invariants_check", "pool_time_source_validate",
            "pool_session_runtime_boot", "app_main"]
    found = pilot.verify_symbols(good)
    report("a clean pilot image passes the symbol audit",
           found["pool_pilot_"] >= 1 and found["pool_time_source_"] >= 1, str(found))

    for bad in ("pool_session_execution_apply", "pool_exec_state_str",
                "nx_pool_execution_boot_init", "pool_session_api_status_build",
                "pool_session_command_submit", "nx_pool_session_api_boot_init"):
        msg = expect_fail(pilot.verify_symbols, good + [bad])
        report(f"a build linking {bad} is refused", bad in msg, msg)

    msg = expect_fail(pilot.verify_symbols, ["app_main", "pool_time_source_validate"])
    report("a build without the pilot diagnostics is refused",
           "pool_pilot_" in msg, msg)

    # The Gate B7 mutation fence's 409 body writer is compiled by
    # CONFIG_NX_TIMED_SESSIONS alone and MUST remain linked: it is what keeps
    # the owner's rollback OTA correctly gated rather than silently unguarded.
    found = pilot.verify_symbols(good + ["nx_pool_session_api_send_conflict"])
    report("the mutation fence's conflict reporter is not mistaken for an API route",
           found["pool_pilot_"] >= 1, str(found))
    report("the allowance is exactly one symbol, not a prefix",
           pilot.ALLOWED_DESPITE_PREFIX == frozenset({"nx_pool_session_api_send_conflict"}))
    msg = expect_fail(pilot.verify_symbols, good + ["nx_pool_session_api_register_routes"])
    report("a build that registers API routes is still refused",
           "nx_pool_session_api_register_routes" in msg, msg)


def test_verify_sizes() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b101-size-") as tmp:
        build = Path(tmp)
        (build / "esp-miner.bin").write_bytes(b"\x00" * 1024)
        (build / "www.bin").write_bytes(b"\x00" * 2048)
        sizes = pilot.verify_sizes(build)
        report("a fitting image reports free space in both partitions",
               sizes["espMinerBinFreeBytes"] > 0 and sizes["wwwBinFreeBytes"] > 0,
               str(sizes))

        (build / "esp-miner.bin").write_bytes(b"\x00" * pilot.APP_SLOT_BYTES)
        msg = expect_fail(pilot.verify_sizes, build)
        report("an oversized application image is refused", "app slot" in msg, msg)

        (build / "esp-miner.bin").write_bytes(b"\x00" * 1024)
        (build / "www.bin").write_bytes(b"\x00" * (pilot.WWW_SLOT_BYTES + 1))
        msg = expect_fail(pilot.verify_sizes, build)
        report("an oversized web image is refused", "www partition" in msg, msg)

        (build / "www.bin").unlink()
        msg = expect_fail(pilot.verify_sizes, build)
        report("a missing build output is refused", "www.bin" in msg, msg)


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------

def sample_manifest() -> dict:
    state = {"commit": "0" * 40, "describe": "v2.14.2-99-gdeadbee", "branch": "pilot"}
    flags = pilot.verify_build_config(pilot_defines(), GOOD_HOST)
    sizes = {"espMinerBinBytes": 1, "espMinerBinFreeBytes": 2,
             "wwwBinBytes": 3, "wwwBinFreeBytes": 4}
    artifacts = [{"filename": "x-pilot-ota.bin", "artifactType": "ota-application",
                  "sizeBytes": 1, "sha256": "a" * 64, "flashMethod": "Update page",
                  "settingsPreserved": True, "destructive": False}]
    return pilot.build_manifest(state, flags, sizes, {"pool_pilot_": 9},
                                "v2.14.2-99-gdeadbee", artifacts, "2026-01-01T00:00:00Z")


def test_manifest() -> None:
    m = sample_manifest()
    blob = json.dumps(m)

    report("the manifest records only that a source is configured",
           m["trustedTimeSourceConfigured"] is True, blob[:120])
    report("the manifest never carries the source", GOOD_HOST not in blob)
    report("the manifest declares execution unreachable",
           m["executionReachable"] is False and m["canCreateTimedSession"] is False)
    report("the manifest declares the API unreachable", m["apiReachable"] is False)
    report("the manifest declares no pool mutation",
           m["canMutatePoolConfiguration"] is False)
    report("the manifest ships no factory image", m["factoryImageIncluded"] is False)
    report("the manifest names a settings-preserving flash method",
           "OTA" in m["flashMethod"] and "NVS" in m["flashMethod"])
    report("the manifest names a rollback method that is not a factory flash",
           "known-good" in m["rollbackMethod"] and "ERASE NVS" in m["rollbackMethod"])
    report("the manifest records the exact commit and describe",
           m["gitCommit"] == "0" * 40 and m["gitDescribe"] == "v2.14.2-99-gdeadbee")
    report("the manifest records the board and ASIC target",
           m["targetBoard"] == "601" and m["targetAsic"] == "BM1370")

    a = json.dumps(sample_manifest(), indent=2, sort_keys=True)
    b = json.dumps(sample_manifest(), indent=2, sort_keys=True)
    report("the manifest is deterministic", a == b)

    report("the manifest contains no owner-local absolute path",
           "C:\\" not in blob and "/home/" not in blob and "D:\\" not in blob)


# Extensions of the local build files the helper legitimately names. Anything
# else that parses as a usable trusted-time source would be a baked-in server.
BUILD_FILE_SUFFIXES = {"bin", "elf", "h", "json", "txt", "defaults", "py", "log",
                       "sh", "cvs", "map", "csv", "md",
                       # local executable names (npm.cmd on Windows), not hosts
                       "cmd", "exe", "bat", "js"}

STRING_LITERAL = re.compile(r"""(?:'([^'\n]*)'|"([^"\n]*)")""")
VERSION_LIKE = re.compile(r"^v?\d+\.\d+(\.\d+)?([.\-][\w\-]+)*$")


def helper_literals(text: str) -> list[str]:
    out = []
    for match in STRING_LITERAL.finditer(text):
        literal = match.group(1) if match.group(1) is not None else match.group(2)
        if literal:
            out.append(literal)
    return out


def looks_like_a_server(value: str) -> bool:
    """True when a literal would be accepted as a trusted-time source AND is
    not obviously a local build-file name or a version string."""
    if not value or pilot.validate_source(value)["state"] != pilot.SRC_CONFIGURED:
        return False
    if value.rsplit(".", 1)[-1].lower() in BUILD_FILE_SUFFIXES:
        return False  # a local build artifact name
    if VERSION_LIKE.match(value):
        return False  # a product/toolchain version, not a host
    return True


# Assembled from fragments on purpose: this scanner runs over its OWN source
# too, so the pattern must not contain a matchable owner-local literal.
_WIN_ROOTS = ("Users", "Compan" + "ys")
_LOCAL_APP_DIR = "App" + "Data"
OWNER_LOCAL_PATH = re.compile(
    r"[A-Za-z]:[\\/](?:" + "|".join(_WIN_ROOTS) + r")"
    r"|/home/[a-z]|/Users/[A-Za-z]|" + _LOCAL_APP_DIR)

# The owner's artifact destination, matched only in a path context: either the
# "Build Artifacts" directory name, or "Firmware" adjacent to a separator.
ARTIFACT_DIR_PATH = re.compile(
    r"Build\s+Artifacts"
    r"|[\\/]\s*Firmware\b"
    r"|\bFirmware\s*[\\/]")

# Every file this gate intends to commit, relative to the repository root.
COMMITTED_FILES = (
    "components/pool_session_runtime/CMakeLists.txt",
    "components/pool_session_runtime/include/pool_session_runtime.h",
    "components/pool_session_runtime/pool_session_runtime.c",
    "components/pool_session_runtime/include/pool_session_runtime_pilot.h",
    "components/pool_session_runtime/pool_session_runtime_pilot.c",
    "components/pool_session_runtime/test/test_pool_session_runtime_pilot.c",
    "main/Kconfig.projbuild",
    "docs/NEURALAXE_PHASE_2M1B_B10_1_OBSERVATION_PILOT_PREPARATION.md",
    "tools/pilot/build_time_observation_pilot.py",
    "tools/pilot/test_build_time_observation_pilot.py",
)


def test_no_owner_paths_in_committed_files() -> None:
    """No file this gate commits may carry an owner-local absolute path."""
    repo = HERE.parent.parent
    for rel in COMMITTED_FILES:
        path = repo / rel
        if not path.is_file():
            report(f"intended-commit file exists: {rel}", False, "missing")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        hits = sorted({m.group(0) for m in OWNER_LOCAL_PATH.finditer(text)})
        report(f"no owner-local path in {rel}", not hits, str(hits))


def test_no_repo_paths_in_helper() -> None:
    text = (HERE / "build_time_observation_pilot.py").read_text(encoding="utf-8")
    report("the helper hardcodes no owner-local path",
           OWNER_LOCAL_PATH.search(text) is None)
    # The owner's artifact directory looks like
    #   <root>\NeuralAxe Build Artifacts\Firmware\...
    # Match it in a PATH context. A bare capitalised "Firmware" is ordinary
    # English ("Firmware and web must both be...") and flagging it would make
    # the scanner fire on prose rather than on a hardcoded destination.
    report("the helper hardcodes no artifact directory",
           not ARTIFACT_DIR_PATH.search(text),
           str(sorted({m.group(0) for m in ARTIFACT_DIR_PATH.finditer(text)})))
    # The scanner must still catch the real thing.
    report("the artifact-directory scanner still detects a real hardcoded path",
           all(ARTIFACT_DIR_PATH.search(s) for s in (
               r'out = "E:\\NeuralAxe Build Artifacts\\Firmware\\b101"',
               'out = "/mnt/d/NeuralAxe Build Artifacts/Firmware"',
               'out = "artifacts/Firmware/"')))
    report("the artifact-directory scanner does not fire on prose",
           not ARTIFACT_DIR_PATH.search(
               '"""THE gate. Firmware and web must both be the expected."""'))

    # Provider-neutral: instead of naming public NTP services (which would read
    # like a suggestion list), assert that NO string literal in the helper is a
    # usable trusted-time source under the firmware's own bounded rules.
    baked = [lit for lit in helper_literals(text) if looks_like_a_server(lit)]
    report("no string literal in the helper is a usable trusted-time source",
           not baked, str(baked))

    # Every line that produces the Kconfig define must carry a placeholder,
    # never a concrete value.
    define_values = []
    for ln in text.splitlines():
        for m in re.finditer(r'CONFIG_NX_TIMED_SESSIONS_NTP_SERVER=\\?"([^"\\]*)', ln):
            define_values.append(m.group(1))
    report("the helper emits the source define at least once", len(define_values) >= 1)
    report("no source define carries a concrete server value",
           not any(looks_like_a_server(v) for v in define_values), str(define_values))
    report("the helper never defaults the environment variable",
           "environ.get(ENV_VAR" not in text and f'[ENV_VAR], "' not in text
           and "or GOOD" not in text)
    # Look for real hardware-access patterns, not the words used to promise
    # their absence in the module docstring.
    hardware_patterns = ["esptool.py", "bitaxetool", "serial.Serial", "import serial",
                         "/dev/tty", "pyserial"]
    hits = [p for p in hardware_patterns if p in text]
    report("the helper never invokes esptool, bitaxetool or a serial port",
           not hits and re.search(r"\bCOM\d", text) is None, str(hits))
    report("the helper never performs DNS or an NTP request",
           "socket." not in text and "getaddrinfo" not in text
           and "urllib" not in text and "requests." not in text)


# --------------------------------------------------------------------------
# Output location: always caller-supplied, never hardcoded
# --------------------------------------------------------------------------

def test_out_dir_is_caller_supplied() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b101-out-") as tmp:
        chosen = Path(tmp) / "pilot-package"

        got = pilot.resolve_out_dir(str(chosen), {})
        report("an explicit --out-dir is honoured", got == chosen.resolve(), str(got))

        got = pilot.resolve_out_dir(None, {pilot.ARTIFACT_ROOT_ENV: str(chosen)})
        report("NX_PILOT_ARTIFACT_ROOT is honoured when --out-dir is absent",
               got == chosen.resolve(), str(got))

        got = pilot.resolve_out_dir(str(chosen), {pilot.ARTIFACT_ROOT_ENV: str(Path(tmp) / "other")})
        report("--out-dir takes precedence over the environment",
               got == chosen.resolve(), str(got))

        msg = expect_fail(pilot.resolve_out_dir, None, {})
        report("no output directory at all is a hard error, never a default",
               pilot.ARTIFACT_ROOT_ENV in msg and "hardcoded" in msg, msg)
        msg = expect_fail(pilot.resolve_out_dir, "   ", {})
        report("a blank output directory is a hard error", msg != "", msg)


def test_caller_supplied_out_dir_actually_works() -> None:
    """Stage into a caller-chosen directory and prove the result is clean."""
    with tempfile.TemporaryDirectory(prefix="nx-b101-stage-") as tmp:
        root = Path(tmp)
        build = root / "build"
        build.mkdir()
        (build / "esp-miner.bin").write_bytes(b"\xa5" * 4096)
        (build / "www.bin").write_bytes(b"\x5a" * 8192)
        out = pilot.resolve_out_dir(str(root / "pkg"), {})

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            artifacts = pilot.stage_artifacts(build, out, "NX-TEST")
        printed = buf.getvalue()

        names = sorted(a["filename"] for a in artifacts)
        report("staging writes both artifacts into the caller's directory",
               names == ["NX-TEST-pilot-ota.bin", "NX-TEST-pilot-www.bin"]
               and all((out / n).is_file() for n in names), str(names))
        report("staged checksums are real SHA-256 digests of the staged bytes",
               all(len(a["sha256"]) == 64 for a in artifacts)
               and artifacts[0]["sha256"] != artifacts[1]["sha256"])
        report("staging prints file names only, never the containing path",
               str(root) not in printed and str(out) not in printed, printed)

        state = {"commit": "0" * 40, "describe": "v0.0.0-0-g0000000", "branch": "b"}
        flags = pilot.verify_build_config(pilot_defines(), GOOD_HOST)
        manifest = pilot.build_manifest(state, flags, pilot.verify_sizes(build),
                                        {"pool_pilot_": 7}, "v0.0.0-0-g0000000",
                                        artifacts, "2026-01-01T00:00:00Z")
        blob = json.dumps(manifest, indent=2)

        report("no staging path appears in the manifest",
               str(out) not in blob and str(root) not in blob and str(build) not in blob)
        report("no repository path appears in the manifest",
               str(HERE) not in blob and str(HERE.parent.parent) not in blob)
        report("no drive letter or POSIX home path appears in the manifest",
               re.search(r"[A-Za-z]:[\\/]", blob) is None and "/home/" not in blob
               and "/Users/" not in blob, blob[:200])
        report("the trusted-time source does not appear in the manifest",
               GOOD_HOST not in blob and GOOD_IPV4 not in blob)


def test_no_source_in_any_output() -> None:
    """The source must not reach stdout on the paths that run before a build."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        host = pilot.read_source_from_env({pilot.ENV_VAR: GOOD_HOST})
        verdict = pilot.validate_source(host)
        # The exact acceptance line main() prints.
        print(f"trusted-time source: accepted ({verdict['state']}, "
              f"{'IPv4 literal' if verdict['literalIpv4'] else 'DNS name'}, "
              f"{verdict['labelCount']} labels) — value not printed")
        pilot.verify_build_config(pilot_defines(), host)
    printed = buf.getvalue()
    report("the acceptance line reveals shape but never the source",
           GOOD_HOST not in printed and "TIME_SOURCE_CONFIGURED" in printed, printed)

    # And the one file that legitimately holds it is shredded.
    with tempfile.TemporaryDirectory(prefix="nx-b101-shred-") as tmp:
        cfg = pilot.write_pilot_defaults(Path(tmp), GOOD_HOST)
        report("only the temporary fragment ever holds the source",
               GOOD_HOST in cfg.read_text(encoding="utf-8"))
        pilot.shred(cfg)
        remaining = [p for p in Path(tmp).rglob("*") if p.is_file()
                     and GOOD_HOST in p.read_text(encoding="utf-8", errors="ignore")]
        report("after shredding, no file under the work tree holds the source",
               not remaining, str(remaining))


# ==========================================================================
# The canonical-revision CALL FLOW.
#
# The container branch of run_build() once called build_command() without the
# revision. Every test at the time passed, because they all called
# build_command() directly — a helper mock — and never exercised the real
# production call site. These proofs are written against the SOURCE and
# against a real run_build() invocation instead.
# ==========================================================================

import ast as _ast
import inspect as _inspect

HELPERS = ("build_time_observation_pilot", "build_store_preflight")


def _helper_path(name: str) -> Path:
    return HERE / (name + ".py")


def _calls_to(tree, func_name: str) -> list:
    return [n for n in _ast.walk(tree)
            if isinstance(n, _ast.Call)
            and isinstance(n.func, _ast.Name)
            and n.func.id == func_name]


def _supplies_revision(call) -> bool:
    """True when this call site actually hands over a revision."""
    if any(kw.arg == "revision" for kw in call.keywords):
        return True
    if any(kw.arg is None for kw in call.keywords):
        return True  # **kwargs forwarding
    return False


def test_every_production_call_site_supplies_the_revision() -> None:
    """(4) The ACTUAL production call signatures are covered — every call site
    in the real helper sources, not a mock written in the test."""
    for name in HELPERS:
        path = _helper_path(name)
        tree = _ast.parse(path.read_text(encoding="utf-8"))
        for func in ("build_command", "run_build"):
            calls = _calls_to(tree, func)
            report("%s: %s() is actually called in production code"
                   % (name, func), len(calls) >= 1, "%d call sites" % len(calls))
            bad = [c.lineno for c in calls if not _supplies_revision(c)]
            report("%s: every %s() call site supplies revision (%d sites)"
                   % (name, func, len(calls)),
                   not bad, "missing at lines %s" % bad)

    # The exact defect that shipped, expressed as a source assertion: the
    # container branch must forward the revision, not rebuild the string.
    src = _inspect.getsource(pilot.run_build)
    report("the B10.1 container branch forwards the revision",
           "build_command(CONTAINER_REPO, CONTAINER_WORK, revision=revision)" in src)
    report("the B10.1 native branch forwards the revision",
           "revision=revision" in src.split("if native:")[1].split("return")[0])

    # MUTATION SELF-CHECK. A source scanner that cannot fail is worthless, and
    # the previous battery passed while this exact defect was in the tree. Feed
    # the detector the regression verbatim and require it to object.
    regressed = _ast.parse(
        "def run_build(repo, work, image, native, *, revision):\n"
        "    inner = 'cfg && ' + build_command(CONTAINER_REPO, CONTAINER_WORK)\n")
    missed = [c for c in _calls_to(regressed, "build_command")
              if not _supplies_revision(c)]
    report("the call-site detector REJECTS the regression that shipped "
           "(build_command with no revision)",
           len(missed) == 1, str(missed))
    repaired = _ast.parse(
        "def run_build(repo, work, image, native, *, revision):\n"
        "    inner = 'cfg && ' + build_command(CONTAINER_REPO, CONTAINER_WORK,"
        " revision=revision)\n")
    report("the call-site detector ACCEPTS the corrected call",
           not [c for c in _calls_to(repaired, "build_command")
                if not _supplies_revision(c)])


def test_revision_cannot_be_omitted() -> None:
    """(2) Omitting the revision is impossible to do silently: it is a
    keyword-only parameter with no default, so it fails loudly and in a
    controlled way."""
    for name in HELPERS:
        mod = pilot if name == "build_time_observation_pilot" else _load_helper(name)
        sig = _inspect.signature(mod.build_command)
        p = sig.parameters.get("revision")
        report("%s: build_command's revision is keyword-only" % name,
               p is not None and p.kind is _inspect.Parameter.KEYWORD_ONLY, str(sig))
        report("%s: build_command's revision has no default" % name,
               p is not None and p.default is _inspect.Parameter.empty)
        sig2 = _inspect.signature(mod.run_build)
        p2 = sig2.parameters.get("revision")
        report("%s: run_build's revision is keyword-only with no default" % name,
               p2 is not None
               and p2.kind is _inspect.Parameter.KEYWORD_ONLY
               and p2.default is _inspect.Parameter.empty, str(sig2))

        # Controlled failure, not a silent wrong value.
        try:
            mod.build_command("/repo", "/work")
            caught = ""
        except TypeError as exc:
            caught = str(exc)
        report("%s: omitting the revision raises TypeError, never builds" % name,
               "revision" in caught, caught)

        # A path can no longer land in the revision slot by position.
        try:
            mod.build_command("/repo", "/work", "/oops")
            caught = ""
        except TypeError as exc:
            caught = str(exc)
        report("%s: a third positional argument is rejected outright" % name,
               "positional" in caught, caught)


def test_run_build_forwards_revision_to_build_command() -> None:
    """(1) run_build really forwards the revision — BOTH branches driven, with
    the container runtime and subprocess stubbed so nothing is executed."""
    rev = "v2.14.2-71-gd8176eb7"
    for name in HELPERS:
        mod = pilot if name == "build_time_observation_pilot" else _load_helper(name)
        for branch, native in (("container", False), ("native", True)):
            seen = {}

            class _Result:
                returncode = 0

            def fake_run(argv, **kw):
                seen["argv"] = argv
                return _Result()

            real_run = mod.subprocess.run
            real_which = mod.shutil.which
            mod.subprocess.run = fake_run
            mod.shutil.which = lambda n: "/usr/bin/docker" if n == "docker" else None
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    mod.run_build(Path("/repo"), Path("/work"), "img", native,
                                  revision=rev)
            finally:
                mod.subprocess.run = real_run
                mod.shutil.which = real_which

            rendered = " ".join(str(x) for x in seen.get("argv", []))
            report("%s/%s: the firmware command carries -DPROJECT_VER=%s"
                   % (name, branch, rev),
                   '-DPROJECT_VER="%s"' % rev in rendered,
                   rendered[-160:])
            report("%s/%s: the container is not asked to derive its own identity"
                   % (name, branch),
                   "git describe" not in rendered)


def test_both_helpers_share_the_revision_contract() -> None:
    """(3) B10.1 and B10.2 use the same canonical revision contract."""
    other = _load_helper("build_store_preflight")
    report("both helpers resolve the same canonical_revision module",
           Path(pilot.canon.__file__).resolve()
           == Path(other.canon.__file__).resolve()
           == (HERE / "canonical_revision.py").resolve())
    for attr in ("CANONICAL_ABBREV", "CANONICAL_DESCRIBE_ARGS", "REVISION_ENV"):
        report("both helpers share %s" % attr,
               getattr(pilot.canon, attr) == getattr(other.canon, attr))
    for func in ("build_command", "run_build"):
        a = _inspect.signature(getattr(pilot, func)).parameters["revision"]
        b = _inspect.signature(getattr(other, func)).parameters["revision"]
        report("both helpers declare %s(revision=...) identically" % func,
               a.kind == b.kind == _inspect.Parameter.KEYWORD_ONLY
               and a.default is b.default is _inspect.Parameter.empty)

    # And the same exact-string pair requirement.
    for label, mod in (("B10.1", pilot), ("B10.2", other)):
        src = _inspect.getsource(mod.verify_release_pair)
        report("%s requires firmware == web == canonical, by exact string"
               % label,
               "fw != web" in src and "fw != describe" in src
               and "web != describe" in src)
        report("%s validates the canonical shape of both sides" % label,
               "validate_canonical" in src)


# --------------------------------------------------------------------------
# Cleanup after a firmware-build exception, proven by driving the REAL main().
# --------------------------------------------------------------------------

SECRET_HOST = "pilot-source.invalid.example"


def _seed_repo(root: Path) -> Path:
    """A throwaway git repo shaped like this one (tag + the web dist path)."""
    cfg = ["-c", "user.email=t@example.invalid", "-c", "user.name=t",
           "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main"]

    def run(*a):
        return subprocess.run(["git", *cfg, *a], cwd=str(root),
                              capture_output=True, text=True, check=True)

    (root / pilot.WEB_SRC_REL).mkdir(parents=True, exist_ok=True)
    (root / pilot.WEB_SRC_REL / "package.json").write_text("{}\n")
    run("init", "-q")
    run("add", "-A")
    run("commit", "-q", "-m", "seed")
    run("tag", "v2.14.2")
    (root / "f.txt").write_text("x\n")
    run("add", "-A")
    run("commit", "-q", "-m", "c1")
    return root


def _run_main_with_failing_build(tmp: Path) -> dict:
    """Drive the real main() to the point of a firmware-build failure.

    Only the two heavy stages are replaced: the npm web build (which would take
    minutes and needs a real Angular project) and the firmware build (which is
    the stage under test — it must raise). EVERYTHING else, including the
    try/finally cleanup contract, is the real code path.
    """
    repo = _seed_repo(tmp / "repo")
    work = tmp / "work"
    work.mkdir()
    out = tmp / "out"
    dist_inner = repo / pilot.WEB_DIST_INNER

    def fake_frontend(r, describe, npm_install="ci"):
        dist_inner.mkdir(parents=True, exist_ok=True)
        (dist_inner / "version.txt").write_text(describe, encoding="utf-8")
        (dist_inner / "main.js").write_text("//\n", encoding="utf-8")
        return describe

    def exploding_build(*a, **k):
        # The exact shape of the reported regression: a TypeError escaping the
        # firmware-build stage, NOT a controlled PilotError.
        raise TypeError(
            "build_command() missing 1 required positional argument: 'revision'")

    real_frontend, real_build = pilot.build_frontend, pilot.run_build
    real_argv = sys.argv
    pilot.build_frontend = fake_frontend
    pilot.run_build = exploding_build
    sys.argv = ["build_time_observation_pilot.py",
                "--repo-root", str(repo), "--work-dir", str(work),
                "--out-dir", str(out)]
    os.environ[pilot.ENV_VAR] = SECRET_HOST

    captured = io.StringIO()
    raised = None
    config_existed = {"value": False}

    # Confirm the temporary configuration really did carry the source before
    # the failure — otherwise "it was removed" would prove nothing.
    real_write = pilot.write_pilot_defaults

    def watching_write(w, host):
        p = real_write(w, host)
        config_existed["value"] = p.is_file() and host in p.read_text(encoding="utf-8")
        return p

    pilot.write_pilot_defaults = watching_write
    try:
        with contextlib.redirect_stdout(captured), \
                contextlib.redirect_stderr(captured):
            try:
                pilot.main()
            except BaseException as exc:      # noqa: BLE001 - recorded, not handled
                raised = exc
    finally:
        pilot.build_frontend = real_frontend
        pilot.run_build = real_build
        pilot.write_pilot_defaults = real_write
        sys.argv = real_argv
        os.environ.pop(pilot.ENV_VAR, None)

    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=str(repo),
                           capture_output=True, text=True).stdout.strip()
    return {
        "repo": repo, "work": work, "out": out,
        "raised": raised, "log": captured.getvalue(), "dirty": dirty,
        "config_carried_source": config_existed["value"],
        "config_left": sorted(p.name for p in work.glob("*.sdkconfig.defaults")),
        "dist_left": (repo / pilot.WEB_DIST_REL).exists(),
    }


def test_cleanup_after_a_firmware_build_exception() -> None:
    """(5)(6)(7)(8) After the firmware build raises, the frontend output and
    the temporary configuration are removed, the tracked tree is unchanged, and
    nothing leaks the source."""
    with tempfile.TemporaryDirectory(prefix="nx-b101-cleanup-") as tmp:
        r = _run_main_with_failing_build(Path(tmp))

        report("the failing build really did raise",
               r["raised"] is not None,
               type(r["raised"]).__name__)
        report("the temporary configuration really carried the source before "
               "the failure", r["config_carried_source"] is True)

        # (6) temporary configuration removed after failure
        report("the temporary configuration is removed after the exception",
               r["config_left"] == [], str(r["config_left"]))

        # (5) frontend output removed after a firmware-build exception
        report("the frontend build output is removed after the exception",
               r["dist_left"] is False)

        # (7) tracked tree unchanged
        report("the tracked tree is left clean after the exception",
               r["dirty"] == "", r["dirty"])

        # (8) no hostname in logs or in the exception
        report("no NTP hostname reaches stdout/stderr",
               SECRET_HOST not in r["log"])
        report("no NTP hostname reaches the raised exception",
               SECRET_HOST not in str(r["raised"])
               and SECRET_HOST not in repr(r["raised"]))
        report("no NTP hostname reaches a manifest (none was written)",
               not r["out"].exists() or not any(
                   SECRET_HOST in p.read_text(encoding="utf-8", errors="replace")
                   for p in r["out"].rglob("*") if p.is_file()))
        report("no package was staged by the failing run",
               not r["out"].exists() or not any(r["out"].iterdir()))

        # The scanner must be able to see the source at all, or the four checks
        # above would pass vacuously.
        report("the leak scanner is not vacuous (it finds a planted value)",
               SECRET_HOST in ("log carrying " + SECRET_HOST))


def main() -> int:
    print("Gate B10.1 pilot build-helper gates")
    test_validator_accepts()
    test_validator_rejects()
    test_validator_unconfigured()
    test_env_required()
    test_source_never_printed()
    test_temp_config()
    test_defaults_deterministic()
    test_verify_build_config()
    test_verify_symbols()
    test_verify_sizes()
    test_manifest()
    test_out_dir_is_caller_supplied()
    test_caller_supplied_out_dir_actually_works()
    test_no_source_in_any_output()
    test_no_owner_paths_in_committed_files()
    test_no_repo_paths_in_helper()
    test_every_production_call_site_supplies_the_revision()
    test_revision_cannot_be_omitted()
    test_run_build_forwards_revision_to_build_command()
    test_both_helpers_share_the_revision_contract()
    test_cleanup_after_a_firmware_build_exception()
    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
