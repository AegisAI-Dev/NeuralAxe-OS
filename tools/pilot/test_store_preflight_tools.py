#!/usr/bin/env python3
"""NeuralAxe OS — Gate B10.2 tooling test battery.

Self-contained (no pytest). Covers the preflight build helper's pre-build and
post-build decisions, and the offline rollback verifier's full outcome set,
using synthetic fixtures only.

NOTHING here builds firmware, touches hardware, opens a serial port, reads a
real NVS partition, performs DNS, contacts NTP or reaches a network.
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import re
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pre = _load("build_store_preflight")
rb = _load("verify_rollback_readiness")

PASS = 0
FAIL = 0

# Assembled from fragments: this scanner runs over its own source too.
_WIN_ROOTS = ("Users", "Compan" + "ys")
_LOCAL_APP_DIR = "App" + "Data"
OWNER_LOCAL_PATH = re.compile(
    r"[A-Za-z]:[\\/](?:" + "|".join(_WIN_ROOTS) + r")"
    r"|/home/[a-z]|/Users/[A-Za-z]|" + _LOCAL_APP_DIR)

COMMITTED_FILES = (
    "components/pool_session_preflight/include/pool_session_preflight.h",
    "components/pool_session_preflight/pool_session_preflight.c",
    "components/pool_session_preflight/pool_session_preflight_nvs.c",
    "components/pool_session_preflight/pool_session_preflight_boot.c",
    "components/pool_session_preflight/CMakeLists.txt",
    "components/pool_session_preflight/test/CMakeLists.txt",
    "components/pool_session_preflight/test/test_pool_session_preflight.c",
    "tools/pilot/build_store_preflight.py",
    "tools/pilot/verify_rollback_readiness.py",
    "tools/pilot/test_store_preflight_tools.py",
    "main/Kconfig.projbuild",
    "main/main.c",
)


NETWORK_MODULES = {"socket", "urllib", "requests", "http", "ftplib", "telnetlib",
                   "asyncio", "ssl", "smtplib", "xmlrpc"}
DEVICE_MODULES = {"serial", "usb", "esptool", "bitaxetool"}
HARDWARE_LITERAL = re.compile(r"esptool|bitaxetool|/dev/tty|\bCOM\d|read_flash|nvs_tool",
                              re.IGNORECASE)


def module_imports(path: Path) -> set[str]:
    """Top-level module names this file actually imports (AST, not text)."""
    import ast
    names: set[str] = set()
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Import):
            for a in node.names:
                names.add(a.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom) and node.module:
            names.add(node.module.split(".")[0])
    return names


def code_string_literals(path: Path) -> list[str]:
    """Every string literal EXCEPT docstrings.

    Docstrings are excluded deliberately: these tools document the hardware and
    network access they do not perform, so scanning them would flag the very
    sentence promising the absence.
    """
    import ast
    tree = ast.parse(path.read_text(encoding="utf-8"))
    doc_nodes = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            body = getattr(node, "body", None)
            if body and isinstance(body[0], ast.Expr) and \
               isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
                doc_nodes.add(id(body[0].value))
    return [n.value for n in ast.walk(tree)
            if isinstance(n, ast.Constant) and isinstance(n.value, str)
            and id(n) not in doc_nodes]


def report(name: str, ok: bool, detail: str = "") -> None:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name} {detail}")


def expect_fail(fn, *a, **k) -> str:
    try:
        fn(*a, **k)
    except pre.PreflightError as exc:
        return str(exc)
    return ""


# ==========================================================================
# D. Preflight build helper
# ==========================================================================

def preflight_defines(extra: dict | None = None) -> dict:
    d = {"CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT": "1"}
    if extra:
        d.update(extra)
    return d


def test_out_dir() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b102-out-") as tmp:
        chosen = Path(tmp) / "pkg"
        report("an explicit --out-dir is honoured",
               pre.resolve_out_dir(str(chosen), {}) == chosen.resolve())
        report("the environment root is honoured",
               pre.resolve_out_dir(None, {pre.ARTIFACT_ROOT_ENV: str(chosen)}) == chosen.resolve())
        report("--out-dir wins over the environment",
               pre.resolve_out_dir(str(chosen),
                                   {pre.ARTIFACT_ROOT_ENV: str(Path(tmp) / "other")})
               == chosen.resolve())
        msg = expect_fail(pre.resolve_out_dir, None, {})
        report("a missing output path fails, never defaults",
               pre.ARTIFACT_ROOT_ENV in msg and "hardcoded" in msg, msg)
        report("a blank output path fails", expect_fail(pre.resolve_out_dir, "  ", {}) != "")


def test_verify_build_config() -> None:
    flags = pre.verify_build_config(preflight_defines())
    report("the exact preflight posture is accepted",
           flags["CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT"] is True
           and not any(flags[f] for f in pre.PREFLIGHT_FLAGS_OFF), str(flags))

    d = preflight_defines()
    del d["CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT"]
    report("a build without the preflight flag is refused",
           "STORE_PREFLIGHT" in expect_fail(pre.verify_build_config, d))

    for extra in pre.PREFLIGHT_FLAGS_OFF:
        msg = expect_fail(pre.verify_build_config, preflight_defines({extra: "1"}))
        report(f"a build WITH {extra} is refused", extra in msg, msg)

    msg = expect_fail(pre.verify_build_config,
                      preflight_defines({"CONFIG_NX_TIMED_SESSIONS_NTP_SERVER": '"x.example"'}))
    report("a preflight build carrying an NTP source is refused",
           "trusted-time source" in msg, msg)
    report("an empty NTP source is accepted",
           pre.verify_build_config(
               preflight_defines({"CONFIG_NX_TIMED_SESSIONS_NTP_SERVER": '""'})) is not None)


def test_verify_symbols() -> None:
    good = ["nx_tps_preflight_classify", "nx_tps_preflight_run_once",
            "pool_session_store_load", "app_main"]
    found = pre.verify_symbols(good)
    report("a clean preflight image passes the symbol audit",
           found["nx_tps_preflight_"] >= 1, str(found))

    for bad in ("pool_session_execution_apply", "pool_exec_state_str",
                "nx_pool_execution_boot_init", "pool_session_api_status_build",
                "nx_pool_session_api_register_routes", "pool_pilot_step",
                "pool_time_sntp_start", "pool_time_source_validate"):
        msg = expect_fail(pre.verify_symbols, good + [bad])
        report(f"a preflight image linking {bad} is refused", bad in msg, msg)

    for writer in pre.FORBIDDEN_STORE_WRITE_SYMBOLS:
        msg = expect_fail(pre.verify_symbols, good + [writer])
        report(f"a preflight image linking {writer} is refused",
               "MUTATION" in msg and writer in msg, msg)

    msg = expect_fail(pre.verify_symbols, ["app_main", "pool_session_store_load"])
    report("a build without the read-only inspector is refused",
           "nx_tps_preflight_" in msg, msg)


def test_verify_sizes_and_config_file() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b102-size-") as tmp:
        build = Path(tmp)
        (build / "esp-miner.bin").write_bytes(b"\0" * 2048)
        (build / "www.bin").write_bytes(b"\0" * 4096)
        sizes = pre.verify_sizes(build)
        report("a fitting image reports free space in both partitions",
               sizes["espMinerBinFreeBytes"] > 0 and sizes["wwwBinFreeBytes"] > 0)

        (build / "esp-miner.bin").write_bytes(b"\0" * pre.APP_SLOT_BYTES)
        report("an oversized application image is refused",
               "app slot" in expect_fail(pre.verify_sizes, build))

        (build / "esp-miner.bin").write_bytes(b"\0" * 2048)
        (build / "www.bin").unlink()
        report("a missing build output is refused", "www.bin" in expect_fail(pre.verify_sizes, build))

    # The temporary configuration is created outside the tree and removed.
    with tempfile.TemporaryDirectory(prefix="nx-b102-cfg-") as tmp:
        work = Path(tmp)
        cfg = pre.write_preflight_defaults(work)
        text = cfg.read_text(encoding="utf-8")
        report("the temporary config enables only the preflight flag",
               "CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT=y" in text
               and all(f"# {f} is not set" in text for f in pre.PREFLIGHT_FLAGS_OFF), text)
        report("the temporary config carries no NTP source", "NTP_SERVER" not in text)
        report("the temporary config lives outside any repository", cfg.parent == work)
        report("the temporary config is byte-deterministic",
               pre.preflight_defaults_text() == pre.preflight_defaults_text())
        cfg.unlink()
        report("the temporary config is removed after the build", not cfg.exists())


def test_preflight_manifest() -> None:
    state = {"commit": "0" * 40, "describe": "v2.14.2-99-gdeadbee", "branch": "b"}
    artifacts = [{"filename": "x-preflight-ota.bin", "artifactType": "ota-application",
                  "sizeBytes": 1, "sha256": "a" * 64, "flashMethod": "Update page",
                  "settingsPreserved": True, "destructive": False}]
    m = pre.build_manifest(state, pre.verify_build_config(preflight_defines()),
                           {"espMinerBinBytes": 1}, {"nx_tps_preflight_": 8},
                           "v2.14.2-99-gdeadbee", artifacts, "2026-01-01T00:00:00Z")
    blob = json.dumps(m, indent=2)

    report("the manifest records the preflight flag enabled", m["storePreflightEnabled"] is True)
    report("the manifest records execution/API/observation disabled",
           m["executionEnabled"] is False and m["apiEnabled"] is False
           and m["timeObservationEnabled"] is False and m["pilotDiagnosticsEnabled"] is False)
    report("the manifest records no NTP source", m["trustedTimeSourceConfigured"] is False)
    report("the manifest records read-only access to the timed-session store",
           m["readOnlyStoreAccess"] is True
           and m["timedSessionStoreWritesPossible"] is False)
    # The claim must be scoped: an earlier draft said the image "provably cannot
    # write", which is false — nvs_config_init writes the "main" namespace and in
    # its recovery branch erases the whole partition.
    report("the manifest does NOT claim the whole image is NVS write-free",
           m["wholeImageNvsWriteFree"] is False)
    report("the manifest records that destructive NVS recovery is disabled",
           m["destructiveNvsRecoveryDisabled"] is True)
    report("the manifest lists the NVS-init blocking token",
           "TPS_PREFLIGHT_BLOCKED_NVS_INIT" in m["expectedSerialTokens"])
    report("the manifest explains what else may write NVS",
           "nvs_config_init" in m["wholeImageNvsWriteNote"]
           and "COMPILED OUT" in m["wholeImageNvsWriteNote"]
           and "BEFORE any write-capable" in m["wholeImageNvsWriteNote"],
           m["wholeImageNvsWriteNote"])
    report("the manifest lists every expected serial token",
           m["expectedSerialTokens"] == pre.EXPECTED_TOKENS and len(pre.EXPECTED_TOKENS) == 12)
    report("the manifest lists exactly two acceptable results",
           m["acceptableResults"] == ["TPS_PREFLIGHT_EMPTY", "TPS_PREFLIGHT_CLEARED"])
    report("every non-acceptable outcome token is listed as blocking",
           set(m["blockingResults"]) == {
               "TPS_PREFLIGHT_BLOCKED_RECORD", "TPS_PREFLIGHT_BLOCKED_TERMINAL",
               "TPS_PREFLIGHT_BLOCKED_UNCERTAIN", "TPS_PREFLIGHT_BLOCKED_CORRUPT",
               "TPS_PREFLIGHT_BLOCKED_SCHEMA", "TPS_PREFLIGHT_BLOCKED_IO",
               "TPS_PREFLIGHT_BLOCKED_NVS_INIT",
               "TPS_PREFLIGHT_INTERNAL_ERROR"}, str(m["blockingResults"]))
    report("the manifest names an NVS-preserving OTA method",
           "OTA" in m["flashMethod"] and "preserved" in m["flashMethod"])
    report("the manifest ships no factory image", m["factoryImageIncluded"] is False)
    report("the manifest records the exact commit and board",
           m["gitCommit"] == "0" * 40 and m["targetBoard"] == "601")
    report("the manifest is deterministic",
           json.dumps(m, sort_keys=True) == json.dumps(
               pre.build_manifest(state, pre.verify_build_config(preflight_defines()),
                                  {"espMinerBinBytes": 1}, {"nx_tps_preflight_": 8},
                                  "v2.14.2-99-gdeadbee", artifacts, "2026-01-01T00:00:00Z"),
               sort_keys=True))
    report("the manifest carries no local filesystem path",
           OWNER_LOCAL_PATH.search(blob) is None and re.search(r"[A-Za-z]:[\\/]", blob) is None)


def test_preflight_staging() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b102-stage-") as tmp:
        root = Path(tmp)
        build = root / "build"
        build.mkdir()
        (build / "esp-miner.bin").write_bytes(b"\xa5" * 4096)
        (build / "www.bin").write_bytes(b"\x5a" * 8192)
        out = pre.resolve_out_dir(str(root / "pkg"), {})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            artifacts = pre.stage_artifacts(build, out, "NX-TEST")
        names = sorted(a["filename"] for a in artifacts)
        report("staging writes both artifacts into the caller's directory",
               names == ["NX-TEST-preflight-ota.bin", "NX-TEST-preflight-www.bin"]
               and all((out / n).is_file() for n in names), str(names))
        report("staged checksums are real digests of the staged bytes",
               artifacts[0]["sha256"] == hashlib.sha256((out / names[0]).read_bytes()).hexdigest())
        report("staging prints no containing path", str(root) not in buf.getvalue())


# ==========================================================================
# E. Rollback verifier
# ==========================================================================

def make_rollback(root: Path, fw_rev="v2.14.2-43-gd333dc4", web_rev=None,
                  board="601", with_factory=True, break_hash=False,
                  drop_file=None, device="Gamma", asic="BM1370") -> Path:
    web_rev = web_rev or fw_rev
    root.mkdir(parents=True, exist_ok=True)
    entries = [("ota.bin", "ota-application", b"APP" * 100),
               ("www.bin", "www-update", b"WEB" * 100)]
    if with_factory:
        entries.append(("factory.bin", "factory-image", b"FAC" * 100))
    artifacts = []
    for name, kind, data in entries:
        path = root / name
        if drop_file != name:
            path.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        if break_hash and kind == "ota-application":
            digest = "f" * 64
        artifacts.append({"filename": name, "artifactType": kind,
                          "sizeBytes": len(data), "sha256": digest})
    manifest = {"schemaVersion": 1, "targetBoard": board, "targetDevice": device,
                "targetAsic": asic, "supportedBoards": [board],
                "firmwareRevision": fw_rev, "webRevision": web_rev,
                "sourceRevision": fw_rev, "artifacts": artifacts}
    mpath = root / "manifest.json"
    mpath.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return mpath


def test_rollback() -> None:
    with tempfile.TemporaryDirectory(prefix="nx-b102-rb-") as tmp:
        base = Path(tmp)

        m = make_rollback(base / "exact")
        v = rb.verify(m, "v2.14.2-43-gd333dc4", "v2.14.2-43-gd333dc4", "601", False, True)
        report("an exactly matching pair is READY_EXACT_MATCH",
               v.outcome == rb.READY_EXACT and rb.is_ready(v.outcome), str(v.reasons))

        v = rb.verify(m, "v2.14.2-67-g3bbfb07", None, "601", True, True)
        report("an explicitly approved older pair is READY_EXPLICIT_DOWNGRADE",
               v.outcome == rb.READY_DOWNGRADE, str(v.reasons))
        report("a downgrade is NEVER reported as an exact match",
               v.outcome != rb.READY_EXACT)

        v = rb.verify(m, "v2.14.2-67-g3bbfb07", None, "601", False, True)
        report("a downgrade without authorization is blocked",
               v.outcome == rb.BLOCKED_VERSION_UNKNOWN and not rb.is_ready(v.outcome))

        v = rb.verify(m, None, None, "601", False, True)
        report("an unknown installed version is blocked",
               v.outcome == rb.BLOCKED_VERSION_UNKNOWN)
        v = rb.verify(m, "not-a-version", None, "601", False, True)
        report("an unparseable installed version is blocked",
               v.outcome == rb.BLOCKED_VERSION_UNKNOWN)

        v = rb.verify(m, "v2.14.2-43-gd333dc4", "v2.14.2-40-gaaaaaaa", "601", False, True)
        report("an installed application/web disagreement is a pair mismatch",
               v.outcome == rb.BLOCKED_PAIR_MISMATCH)

        m2 = make_rollback(base / "pair", fw_rev="v2.14.2-43-gd333dc4",
                           web_rev="v2.14.2-41-gbbbbbbb")
        v = rb.verify(m2, "v2.14.2-43-gd333dc4", None, "601", False, True)
        report("an incoherent rollback pair is blocked",
               v.outcome == rb.BLOCKED_PAIR_MISMATCH)

        m3 = make_rollback(base / "board", board="702")
        v = rb.verify(m3, "v2.14.2-43-gd333dc4", None, "702", False, True)
        report("a non-601 board is blocked", v.outcome == rb.BLOCKED_BOARD_MISMATCH)
        v = rb.verify(make_rollback(base / "asic", asic="BM1368"),
                      "v2.14.2-43-gd333dc4", None, "601", False, True)
        report("a non-BM1370 ASIC is blocked", v.outcome == rb.BLOCKED_BOARD_MISMATCH)

        m4 = make_rollback(base / "hash", break_hash=True)
        v = rb.verify(m4, "v2.14.2-43-gd333dc4", None, "601", False, True)
        report("a wrong checksum is blocked", v.outcome == rb.BLOCKED_HASH_MISMATCH)

        m5 = make_rollback(base / "missing", drop_file="www.bin")
        v = rb.verify(m5, "v2.14.2-43-gd333dc4", None, "601", False, True)
        report("a missing artifact is blocked", v.outcome == rb.BLOCKED_HASH_MISMATCH)

        m6 = make_rollback(base / "nofactory", with_factory=False)
        v = rb.verify(m6, "v2.14.2-43-gd333dc4", None, "601", False, True)
        report("a missing factory image blocks when serial recovery is claimed",
               v.outcome == rb.BLOCKED_FACTORY_MISSING)
        v = rb.verify(m6, "v2.14.2-43-gd333dc4", None, "601", False, False)
        report("the same package is READY when serial recovery is NOT claimed",
               v.outcome == rb.READY_EXACT)

        bad = base / "bad"
        bad.mkdir()
        (bad / "manifest.json").write_text("{not json", encoding="utf-8")
        v = rb.verify(bad / "manifest.json", "v2.14.2-43-gd333dc4", None, "601", False, False)
        report("an invalid manifest is blocked", v.outcome == rb.BLOCKED_MANIFEST_INVALID)
        v = rb.verify(base / "nope" / "manifest.json", "v2.14.2-43-gd333dc4",
                      None, "601", False, False)
        report("a missing manifest is blocked", v.outcome == rb.BLOCKED_MANIFEST_INVALID)

        empty = base / "empty"
        empty.mkdir()
        (empty / "manifest.json").write_text(json.dumps({"artifacts": []}), encoding="utf-8")
        v = rb.verify(empty / "manifest.json", "v2.14.2-43-gd333dc4", None, "601", False, False)
        report("a manifest with no artifacts is blocked",
               v.outcome == rb.BLOCKED_MANIFEST_INVALID)

        dirty = make_rollback(base / "dirty", fw_rev="v2.14.2-43-gd333dc4-dirty")
        v = rb.verify(dirty, "v2.14.2-43-gd333dc4", None, "601", False, False)
        report("a dirty rollback revision is blocked",
               v.outcome == rb.BLOCKED_MANIFEST_INVALID)


def test_rollback_properties() -> None:
    report("only the two READY outcomes permit proceeding",
           [o for o in rb.ALL_OUTCOMES if rb.is_ready(o)] == [rb.READY_EXACT, rb.READY_DOWNGRADE])
    report("an unknown outcome never permits proceeding", not rb.is_ready("SOMETHING_ELSE"))
    report("all eight outcomes are distinct", len(set(rb.ALL_OUTCOMES)) == 8)

    # AST-based, not substring-based: these files DOCUMENT that they perform no
    # network or hardware access, so a naive text scan would match its own
    # promise. What matters is what the code imports and calls.
    imports = module_imports(HERE / "verify_rollback_readiness.py")
    report("the verifier imports no networking module",
           not (imports & NETWORK_MODULES), str(sorted(imports & NETWORK_MODULES)))
    report("the verifier imports no serial/device module",
           not (imports & DEVICE_MODULES), str(sorted(imports & DEVICE_MODULES)))
    report("the verifier spawns no subprocess (so it cannot run esptool or Git)",
           "subprocess" not in imports and "os" not in imports, str(sorted(imports)))
    lits = code_string_literals(HERE / "verify_rollback_readiness.py")
    report("no code literal in the verifier names a device or flashing tool",
           not [s for s in lits if HARDWARE_LITERAL.search(s)],
           str([s for s in lits if HARDWARE_LITERAL.search(s)]))


# ==========================================================================
# Owner-path and privacy scans
# ==========================================================================

def test_no_owner_paths() -> None:
    for rel in COMMITTED_FILES:
        path = REPO / rel
        if not path.is_file():
            report(f"intended-commit file exists: {rel}", False, "missing")
            continue
        hits = sorted({m.group(0) for m in
                       OWNER_LOCAL_PATH.finditer(path.read_text(encoding="utf-8",
                                                                errors="replace"))})
        report(f"no owner-local path in {rel}", not hits, str(hits))


def test_tools_have_no_hardware_access() -> None:
    for name in ("build_store_preflight.py", "verify_rollback_readiness.py"):
        path = HERE / name
        imports = module_imports(path)
        report(f"{name} imports no serial/device module",
               not (imports & DEVICE_MODULES), str(sorted(imports & DEVICE_MODULES)))
        report(f"{name} imports no networking module",
               not (imports & NETWORK_MODULES), str(sorted(imports & NETWORK_MODULES)))
        hits = [s for s in code_string_literals(path) if HARDWARE_LITERAL.search(s)]
        report(f"{name} names no device or flashing tool in code", not hits, str(hits))
        report(f"{name} hardcodes no artifact directory",
               "NeuralAxe Build Artifacts" not in path.read_text(encoding="utf-8"))


def main() -> int:
    print("Gate B10.2 tooling gates")
    test_out_dir()
    test_verify_build_config()
    test_verify_symbols()
    test_verify_sizes_and_config_file()
    test_preflight_manifest()
    test_preflight_staging()
    test_rollback()
    test_rollback_properties()
    test_no_owner_paths()
    test_tools_have_no_hardware_access()
    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
