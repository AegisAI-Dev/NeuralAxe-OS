#!/usr/bin/env python3
"""Deterministic tests for the Gate W5 weather pilot builder.

Nothing here builds firmware, contacts a network or touches hardware. All
coordinates are synthetic. The central property under test is that a private
value can reach the temporary sdkconfig fragment and nothing else.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_weather_recommendation_pilot as w5  # noqa: E402

# Synthetic values: a round 1.2345 / -6.789 degrees. Not a real site.
SYN_LAT = "1.2345"
SYN_LON = "-6.789"
SYN_LAT_E4 = 12345
SYN_LON_E4 = -67890


SYN_NTP = "time.example.org"
REPO = Path(__file__).resolve().parents[2]
HELPER_PATH = (Path(__file__).resolve().parent
               / "build_weather_recommendation_pilot.py")
SYN_DIST = "owner-managed-external"


def env(provider="open-meteo", lat=SYN_LAT, lon=SYN_LON, tz="europe/brussels",
        ntp=SYN_NTP, dist=SYN_DIST):
    out = {}
    if ntp is not None:
        out[w5.ENV_NTP] = ntp
    if dist is not None:
        out[w5.ENV_DISTRIBUTION] = dist
    if provider is not None:
        out[w5.ENV_PROVIDER] = provider
    if lat is not None:
        out[w5.ENV_LATITUDE] = lat
    if lon is not None:
        out[w5.ENV_LONGITUDE] = lon
    if tz is not None:
        out[w5.ENV_TIMEZONE] = tz
    return out



def _write_app_image(path: Path, version: str) -> None:
    """A minimal image whose esp_app_desc_t carries `version`."""
    header = bytearray(0x60)
    header[0x20:0x24] = w5.APP_DESC_MAGIC
    encoded = version.encode()
    header[0x30:0x30 + len(encoded)] = encoded
    path.write_bytes(bytes(header))


class DegreeParsing(unittest.TestCase):
    def test_fixed_point_is_exact_and_locale_independent(self):
        self.assertEqual(w5.parse_degrees_e4("0.0001"), 1)
        self.assertEqual(w5.parse_degrees_e4("1"), 10000)
        self.assertEqual(w5.parse_degrees_e4("1.2345"), 12345)
        self.assertEqual(w5.parse_degrees_e4("-6.789"), -67890)
        self.assertEqual(w5.parse_degrees_e4("-0.0001"), -1)
        self.assertEqual(w5.parse_degrees_e4("90"), 900000)
        self.assertEqual(w5.parse_degrees_e4("-180"), -1800000)

    def test_rejects_locale_and_float_forms(self):
        for bad in ("1,5", "1e2", "nan", "inf", "-inf", "NaN", " 1.0", "1.0 ",
                    "+1.0", "01.5", "1.23456", "", ".", "-", "--1", "1..2",
                    "0x10", "1_0"):
            with self.assertRaises(w5.PilotError, msg=f"accepted {bad!r}"):
                w5.parse_degrees_e4(bad)

    def test_error_never_quotes_the_value(self):
        try:
            w5.parse_degrees_e4("12.3456e0")
        except w5.PilotError as exc:
            self.assertNotIn("12", str(exc))
            self.assertNotIn("3456", str(exc))
        else:
            self.fail("expected rejection")


class ConfigValidation(unittest.TestCase):
    def test_missing_all_is_unconfigured(self):
        cfg = w5.read_private_config({})
        self.assertEqual(cfg["status"], w5.SRC_UNCONFIGURED)
        self.assertFalse(cfg["ready"])

    def test_partial_is_incomplete(self):
        for missing in (w5.ENV_NTP, w5.ENV_DISTRIBUTION, w5.ENV_PROVIDER,
                        w5.ENV_LATITUDE, w5.ENV_LONGITUDE, w5.ENV_TIMEZONE):
            e = env()
            del e[missing]
            cfg = w5.read_private_config(e)
            self.assertEqual(cfg["status"], w5.SRC_INCOMPLETE)
            self.assertFalse(cfg["ready"])

    def test_unsupported_provider_and_timezone(self):
        cfg = w5.read_private_config(env(provider="some-other-service"))
        self.assertEqual(cfg["status"], w5.SRC_INVALID_PROVIDER)
        cfg = w5.read_private_config(env(tz="america/new_york"))
        self.assertEqual(cfg["status"], w5.SRC_INVALID_TIMEZONE)

    def test_out_of_range_coordinates(self):
        self.assertEqual(w5.read_private_config(env(lat="90.0001"))["status"],
                         w5.SRC_INVALID_LATITUDE)
        self.assertEqual(w5.read_private_config(env(lat="-90.0001"))["status"],
                         w5.SRC_INVALID_LATITUDE)
        self.assertEqual(w5.read_private_config(env(lon="180.0001"))["status"],
                         w5.SRC_INVALID_LONGITUDE)
        self.assertEqual(w5.read_private_config(env(lon="-180.0001"))["status"],
                         w5.SRC_INVALID_LONGITUDE)

    def test_zero_sentinel_is_rejected(self):
        self.assertEqual(w5.read_private_config(env(lat="0"))["status"],
                         w5.SRC_INCOMPLETE)
        self.assertEqual(w5.read_private_config(env(lon="0.0"))["status"],
                         w5.SRC_INCOMPLETE)
        self.assertEqual(w5.read_private_config(env(lat="0", lon="0"))["status"],
                         w5.SRC_INCOMPLETE)

    def test_valid_configuration_is_ready(self):
        cfg = w5.read_private_config(env())
        self.assertEqual(cfg["status"], w5.SRC_READY)
        self.assertTrue(cfg["ready"])
        self.assertEqual(cfg["latitude_e4"], SYN_LAT_E4)
        self.assertEqual(cfg["longitude_e4"], SYN_LON_E4)
        self.assertEqual(cfg["provider"], "OPEN_METEO")
        self.assertEqual(cfg["timezone"], "EUROPE_BRUSSELS")

    def test_status_tokens_are_value_free(self):
        for token in (w5.SRC_UNCONFIGURED, w5.SRC_INCOMPLETE,
                      w5.SRC_INVALID_PROVIDER, w5.SRC_INVALID_LATITUDE,
                      w5.SRC_INVALID_LONGITUDE, w5.SRC_INVALID_TIMEZONE,
                      w5.SRC_READY):
            self.assertFalse(any(ch.isdigit() for ch in token))
            self.assertNotIn(".", token)
            self.assertNotIn("http", token)


class TemporaryConfiguration(unittest.TestCase):
    def test_fragment_is_the_only_place_values_appear(self):
        cfg = w5.read_private_config(env())
        text = w5.pilot_defaults_text(cfg)
        # It must carry the values (that is its purpose) ...
        self.assertIn(f"CONFIG_NX_WEATHER_LATITUDE_E4={SYN_LAT_E4}", text)
        self.assertIn(f"CONFIG_NX_WEATHER_LONGITUDE_E4={SYN_LON_E4}", text)
        # ... and it must pin the recommendation-only posture.
        self.assertIn("CONFIG_NX_WEATHER_SOURCE_POLICY=y", text)
        self.assertIn("CONFIG_NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL=y", text)
        self.assertIn("# CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set", text)
        self.assertIn("# CONFIG_NX_TIMED_SESSIONS_API is not set", text)

    def test_fragment_is_written_outside_the_repo_and_shredded(self):
        import tempfile
        cfg = w5.read_private_config(env())
        work = Path(tempfile.mkdtemp(prefix="nx-w5-test-"))
        try:
            path = w5.write_pilot_defaults(work, cfg)
            self.assertTrue(path.is_file())
            repo = Path(__file__).resolve().parents[2]
            self.assertNotIn(str(repo).lower(), str(path).lower())
            w5.shred(path)
            self.assertFalse(path.exists())
            # Shredding a missing file is a no-op, not an error.
            w5.shred(path)
        finally:
            import shutil
            shutil.rmtree(work, ignore_errors=True)


class ManifestPrivacy(unittest.TestCase):
    IDENTITY = w5.canonical_revision.BuildIdentity(
        revision="v0.0.0-0-gdeadbee", commit="d" * 40, dirty=False,
        branch="test")

    def test_manifest_is_booleans_and_enums_only(self):
        cfg = w5.read_private_config(env())
        m = w5.build_manifest(self.IDENTITY, cfg, "digest")
        self.assertTrue(m["weatherConfigured"])
        self.assertTrue(m["providerConfigured"])
        self.assertTrue(m["locationConfigured"])
        self.assertTrue(m["timezoneConfigured"])
        self.assertTrue(m["recommendationOnly"])
        self.assertFalse(m["executionEnabled"])
        self.assertFalse(m["timedSessionApiEnabled"])
        self.assertFalse(m["hardwareTuningEnabled"])
        w5.assert_manifest_private_free(m, cfg)

    def test_manifest_contains_no_coordinate_host_or_env_name(self):
        cfg = w5.read_private_config(env())
        blob = json.dumps(w5.build_manifest(self.IDENTITY, cfg, "digest"))
        for token in (str(SYN_LAT_E4), str(SYN_LON_E4), "1.2345", "6.789",
                      "12345", "67890"):
            self.assertNotIn(token, blob)
        for token in ("open-meteo", "api.", "http", "://", "latitude",
                      "longitude", "EUROPE_BRUSSELS", "Kalmthout", "city",
                      SYN_NTP, "time.example"):
            self.assertNotIn(token, blob)
        for var in w5.ENV_VARS:
            self.assertNotIn(var, blob)

    def test_guard_rejects_a_manifest_that_leaks(self):
        cfg = w5.read_private_config(env())
        bad = w5.build_manifest(self.IDENTITY, cfg, "digest")
        bad["oops"] = cfg["latitude_e4"]
        with self.assertRaises(w5.PilotError):
            w5.assert_manifest_private_free(bad, cfg)

        bad2 = w5.build_manifest(self.IDENTITY, cfg, "digest")
        bad2["endpoint"] = "https://api.open-meteo.com/v1/forecast"
        with self.assertRaises(w5.PilotError):
            w5.assert_manifest_private_free(bad2, cfg)


class CliSurface(unittest.TestCase):
    def test_check_config_only_succeeds_on_valid_synthetic_values(self):
        import io
        import contextlib
        saved = {k: __import__("os").environ.get(k) for k in w5.ENV_VARS}
        try:
            for k, v in env().items():
                __import__("os").environ[k] = v
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = w5.main(["--check-config-only"])
            out = buf.getvalue()
            self.assertEqual(rc, 0)
            self.assertIn(w5.SRC_READY, out)
            # The values must never be echoed.
            for token in (SYN_LAT, SYN_LON, str(SYN_LAT_E4), str(SYN_LON_E4),
                          "open-meteo", "europe/brussels", SYN_NTP):
                self.assertNotIn(token, out)
        finally:
            for k, v in saved.items():
                if v is None:
                    __import__("os").environ.pop(k, None)
                else:
                    __import__("os").environ[k] = v

    def test_missing_variables_fail_without_building(self):
        import io
        import contextlib
        saved = {k: __import__("os").environ.get(k) for k in w5.ENV_VARS}
        try:
            for k in w5.ENV_VARS:
                __import__("os").environ.pop(k, None)
            buf, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
                rc = w5.main(["--check-config-only"])
            self.assertEqual(rc, 2)
            self.assertIn(w5.SRC_UNCONFIGURED, buf.getvalue())
        finally:
            for k, v in saved.items():
                if v is not None:
                    __import__("os").environ[k] = v

    def test_output_required_for_a_real_build(self):
        import io
        import contextlib
        saved = {k: __import__("os").environ.get(k) for k in w5.ENV_VARS}
        try:
            for k, v in env().items():
                __import__("os").environ[k] = v
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(err):
                rc = w5.main([])
            self.assertEqual(rc, 2)
            self.assertIn("--output", err.getvalue())
        finally:
            for k, v in saved.items():
                if v is None:
                    __import__("os").environ.pop(k, None)
                else:
                    __import__("os").environ[k] = v


class SourceHygiene(unittest.TestCase):
    def test_helper_contains_no_default_or_real_coordinate(self):
        text = Path(w5.__file__).read_text(encoding="utf-8")
        # No default provider/location/timezone may be baked in.
        self.assertNotIn("51.", text)
        self.assertNotIn("4.4", text)
        self.assertNotIn("Kalmthout", text)
        # The only endpoint-ish strings are in the manifest *denylist*.
        self.assertNotIn("https://api", text)

    def test_helper_declares_the_expected_environment_names(self):
        self.assertEqual(w5.ENV_VARS,
                         ("NX_PILOT_NTP_SERVER", "NX_WEATHER_DISTRIBUTION",
                          "NX_WEATHER_PROVIDER", "NX_WEATHER_LATITUDE",
                          "NX_WEATHER_LONGITUDE", "NX_WEATHER_TIMEZONE"))


class TrustedTimeSource(unittest.TestCase):
    """Gate W6 adds a private trusted-time source to the pilot inputs."""

    def test_accepts_plausible_hostnames_and_ipv4(self):
        for good in ("time.example.org", "ntp.example.com", "a-b.example.net",
                     "192.0.2.10", "10.1.2.3"):
            self.assertTrue(w5.ntp_source_valid(good), good)

    def test_rejects_schemes_ports_paths_and_odd_forms(self):
        for bad in ("", "localhost", "http://time.example.org",
                    "time.example.org:123", "time.example.org/path",
                    "-lead.example.org", "trail-.example.org",
                    "time..example.org", "2001:db8::1", "192.0.2.010",
                    "256.0.0.1", "1.2.3", "time example org"):
            self.assertFalse(w5.ntp_source_valid(bad), bad)

    def test_missing_or_invalid_ntp_is_reported_without_the_value(self):
        cfg = w5.read_private_config(env(ntp=None))
        self.assertEqual(cfg["status"], w5.SRC_INCOMPLETE)
        cfg = w5.read_private_config(env(ntp="http://time.example.org"))
        self.assertEqual(cfg["status"], w5.SRC_INVALID_NTP)
        self.assertFalse(any(ch.isdigit() for ch in w5.SRC_INVALID_NTP))

    def test_unsupported_distribution_is_rejected(self):
        self.assertEqual(w5.read_private_config(env(dist="commercial"))["status"],
                         w5.SRC_INVALID_DISTRIBUTION)
        self.assertEqual(w5.read_private_config(env(dist="self-hosted"))["status"],
                         w5.SRC_INVALID_DISTRIBUTION)


class PilotPosture(unittest.TestCase):
    """The fragment must pin the exact recommendation-only pilot posture."""

    IDENT = w5.canonical_revision.BuildIdentity(
        revision="v0.0.0-0-gdeadbee", commit="d" * 40, dirty=False, branch="t")

    def test_fragment_enables_pilot_and_disables_execution_surfaces(self):
        text = w5.pilot_defaults_text(w5.read_private_config(env()))
        for required in ("CONFIG_NX_TIMED_SESSIONS=y",
                         "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE=y",
                         "CONFIG_NX_WEATHER_AWARE_TUNING=y",
                         "CONFIG_NX_WEATHER_SOURCE_POLICY=y",
                         "CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS=y",
                         "CONFIG_NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL=y"):
            self.assertIn(required, text)
        for forbidden in ("CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set",
                          "CONFIG_NX_TIMED_SESSIONS_API is not set",
                          "CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT is not set"):
            self.assertIn(forbidden, text)

    def test_fragment_is_the_only_home_of_the_ntp_source(self):
        cfg = w5.read_private_config(env())
        self.assertIn(SYN_NTP, w5.pilot_defaults_text(cfg))
        blob = json.dumps(w5.build_manifest(self.IDENT, cfg, "digest"))
        self.assertNotIn(SYN_NTP, blob)

    def test_manifest_records_the_pilot_flags(self):
        cfg = w5.read_private_config(env())
        m = w5.build_manifest(self.IDENT, cfg, "digest")
        self.assertTrue(m["trustedTimeConfigured"])
        self.assertTrue(m["pilotDiagnosticsEnabled"])
        self.assertTrue(m["recommendationOnly"])
        self.assertFalse(m["executionEnabled"])
        self.assertFalse(m["timedSessionApiEnabled"])
        self.assertFalse(m["storePreflightEnabled"])
        self.assertFalse(m["hardwareTuningEnabled"])
        w5.assert_manifest_private_free(m, cfg)

    def test_manifest_guard_rejects_a_leaked_ntp_source(self):
        cfg = w5.read_private_config(env())
        bad = w5.build_manifest(self.IDENT, cfg, "digest")
        bad["timeSource"] = cfg["ntp"]
        with self.assertRaises(w5.PilotError):
            w5.assert_manifest_private_free(bad, cfg)


class VerificationContracts(unittest.TestCase):
    """The post-build proofs Gate W6 requires."""

    def test_symbol_sets_are_declared_in_both_directions(self):
        for pfx in ("pool_session_execution_", "nx_pool_session_api_",
                    "nx_tps_preflight_", "nx_weather_apply_"):
            self.assertIn(pfx, w5.FORBIDDEN_SYMBOL_PREFIXES)
        for pfx in ("nx_weather_pilot_", "nx_weather_source_",
                    "weather_runtime_", "pool_time_sntp_"):
            self.assertIn(pfx, w5.REQUIRED_SYMBOL_PREFIXES)
        self.assertIn("nx_pool_session_api_send_conflict",
                      w5.ALLOWED_DESPITE_PREFIX)

    def test_sdkconfig_contract_lists_both_directions(self):
        for sym in ("CONFIG_NX_WEATHER_AWARE_TUNING",
                    "CONFIG_NX_WEATHER_SOURCE_POLICY",
                    "CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS",
                    "CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE"):
            self.assertIn(sym, w5.REQUIRED_SDKCONFIG)
        for sym in ("CONFIG_NX_TIMED_SESSIONS_EXECUTION",
                    "CONFIG_NX_TIMED_SESSIONS_API",
                    "CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT"):
            self.assertIn(sym, w5.FORBIDDEN_SDKCONFIG)

    def test_sdkconfig_verification_rejects_a_wrong_posture(self):
        import shutil
        import tempfile
        work = Path(tempfile.mkdtemp(prefix="nx-w6-test-"))
        try:
            cfgdir = work / "config"
            cfgdir.mkdir(parents=True)
            ok = "\n".join("#define %s 1" % s for s in w5.REQUIRED_SDKCONFIG)
            (cfgdir / "sdkconfig.h").write_text(ok, encoding="utf-8")
            w5.verify_sdkconfig_h(work)

            bad = ok + "\n#define CONFIG_NX_TIMED_SESSIONS_EXECUTION 1"
            (cfgdir / "sdkconfig.h").write_text(bad, encoding="utf-8")
            with self.assertRaises(w5.PilotError):
                w5.verify_sdkconfig_h(work)

            (cfgdir / "sdkconfig.h").write_text("#define UNRELATED 1",
                                                encoding="utf-8")
            with self.assertRaises(w5.PilotError):
                w5.verify_sdkconfig_h(work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def test_release_pair_reads_both_identities_from_the_built_images(self):
        """The pair is read from the artefacts, never assumed from the input."""
        import shutil
        import tempfile
        work = Path(tempfile.mkdtemp(prefix="nx-w6-pair-"))
        try:
            rev = "v2.14.2-78-g09b19d8d"
            build = work / "build"
            build.mkdir()
            _write_app_image(build / "esp-miner.bin", rev)
            (build / "www.bin").write_bytes(b"\x00" * 32 + rev.encode() + b"\x00" * 32)

            out = w5.verify_release_pair(build, rev)
            self.assertEqual(out["firmwareRevision"], rev)
            self.assertEqual(out["webRevision"], rev)
            self.assertTrue(out["releasePairVerified"])

            # A firmware/web disagreement is the BOOT PAIR MISMATCH case.
            _write_app_image(build / "esp-miner.bin", "v2.14.2-77-g4fab888f")
            with self.assertRaises(w5.PilotError):
                w5.verify_release_pair(build, rev)

            # Agreement on a revision that is not HEAD is still refused.
            _write_app_image(build / "esp-miner.bin", "v2.14.2-77-g4fab888f")
            (build / "www.bin").write_bytes(b"v2.14.2-77-g4fab888f")
            with self.assertRaises(w5.PilotError):
                w5.verify_release_pair(build, rev)

            # A missing artefact fails closed rather than assuming.
            (build / "www.bin").unlink()
            with self.assertRaises(w5.PilotError):
                w5.verify_release_pair(build, rev)
        finally:
            shutil.rmtree(work, ignore_errors=True)


class MutationObservabilityContract(unittest.TestCase):
    """Gate W6.1: the pilot artifact must carry the mutation counters."""

    # ---------- Gate W6.1: authoritative mutation observability ----------

    def test_fragment_enables_mutation_observability(self):
        """Without it the pilot has no authority to read at the mutation
        boundaries, so every mutation fact is UNAVAILABLE and the monitor can
        never report a healthy pilot."""
        text = w5.pilot_defaults_text(w5.read_private_config(env()))
        self.assertIn("CONFIG_NX_MUTATION_OBSERVABILITY=y", text)

    def test_mutation_observability_is_a_required_build_option(self):
        self.assertIn("CONFIG_NX_MUTATION_OBSERVABILITY", w5.REQUIRED_SDKCONFIG)
        self.assertNotIn("CONFIG_NX_MUTATION_OBSERVABILITY",
                         w5.FORBIDDEN_SDKCONFIG)

    def test_sdkconfig_verification_rejects_a_pilot_without_counters(self):
        import shutil
        import tempfile
        work = Path(tempfile.mkdtemp(prefix="nx-w61-cfg-"))
        try:
            cfgdir = work / "config"
            cfgdir.mkdir()
            lines = [f"#define {s} 1" for s in w5.REQUIRED_SDKCONFIG]
            (cfgdir / "sdkconfig.h").write_text(chr(10).join(lines),
                                                encoding="utf-8")
            w5.verify_sdkconfig_h(work)

            without = chr(10).join(
                x for x in lines
                if "CONFIG_NX_MUTATION_OBSERVABILITY" not in x)
            (cfgdir / "sdkconfig.h").write_text(without, encoding="utf-8")
            with self.assertRaises(w5.PilotError):
                w5.verify_sdkconfig_h(work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def test_mutation_symbols_are_required_in_the_pilot_image(self):
        self.assertIn("nx_mutation_", w5.REQUIRED_SYMBOL_PREFIXES)
        # The observability symbols are REQUIRED, never forbidden: the pilot
        # reads them and the weather component never increments them.
        for pfx in w5.FORBIDDEN_SYMBOL_PREFIXES:
            self.assertFalse("nx_mutation_".startswith(pfx))
            self.assertFalse(pfx.startswith("nx_mutation_"))


class CanonicalIdentityRegression(unittest.TestCase):
    """The KeyError: 'describe' regression and the contract that prevents it.

    canonical_revision.identity() has never had a `describe` key; that name
    belongs to the B10.1/B10.2 helpers' own local repo_state dictionaries. The
    weather helper reached for it while calling the shared contract, so the
    real build failed with a raw KeyError after the private configuration had
    already validated.
    """

    def test_the_shared_contract_has_no_describe_key(self):
        keys = set(w5.canonical_revision.identity(REPO).keys())
        self.assertEqual(keys, {"commit", "revision", "dirty"})
        self.assertNotIn("describe", keys)

    def test_the_typed_accessor_exposes_every_required_value(self):
        ident = w5.canonical_revision.build_identity(REPO)
        self.assertEqual(len(ident.commit), 40)
        self.assertRegex(ident.commit, r"^[0-9a-f]{40}$")
        w5.canonical_revision.validate_canonical(ident.revision)
        self.assertIsInstance(ident.dirty, bool)
        self.assertTrue(ident.branch)
        self.assertTrue(w5.canonical_revision.revision_matches_commit(
            ident.revision, ident.commit))

    def test_the_obsolete_describe_lookup_is_gone_from_the_helper(self):
        src = HELPER_PATH.read_text(encoding="utf-8")
        self.assertNotIn('["describe"]', src)
        self.assertNotIn("['describe']", src)

    def test_the_helper_never_recomputes_git_identity(self):
        src = HELPER_PATH.read_text(encoding="utf-8")
        for flag in ("--tags", "--long", "--always", "--dirty", "--abbrev="):
            self.assertNotIn(flag, src, "helper assembles its own " + flag)
        self.assertIn("canonical_revision.build_identity(", src)

    def test_a_typed_field_that_does_not_exist_fails_loudly(self):
        ident = w5.canonical_revision.build_identity(REPO)
        with self.assertRaises(AttributeError):
            _ = ident.describe

    def test_missing_identity_is_a_controlled_error_not_a_traceback(self):
        import shutil
        import tempfile
        empty = Path(tempfile.mkdtemp(prefix="nx-w6-noidentity-"))
        try:
            with self.assertRaises(w5.PilotError) as ctx:
                w5.resolve_identity(empty)
            self.assertIn("canonical identity", str(ctx.exception).lower())
        finally:
            shutil.rmtree(empty, ignore_errors=True)

    def test_a_changed_contract_shape_is_reported_not_raised(self):
        original = w5.canonical_revision.build_identity
        try:
            def broken(_repo):
                raise KeyError("describe")
            w5.canonical_revision.build_identity = broken
            with self.assertRaises(w5.PilotError) as ctx:
                w5.resolve_identity(REPO)
            self.assertIn("contract", str(ctx.exception).lower())
        finally:
            w5.canonical_revision.build_identity = original

    def test_a_dirty_tree_is_refused_by_the_typed_identity(self):
        ident = w5.canonical_revision.BuildIdentity(
            commit="a" * 40, revision="v0.0.0-0-gdeadbee", dirty=True,
            branch="b")
        with self.assertRaises(w5.canonical_revision.RevisionError):
            ident.require_clean()


class ProductionCallPath(unittest.TestCase):
    """main() must exercise identity, frontend, firmware and the pair."""

    def test_main_calls_every_verification_stage(self):
        import inspect
        src = inspect.getsource(w5.main)
        for call in ("resolve_identity(", "build_frontend(", "run_build(",
                     "verify_sdkconfig_h(", "verify_symbols(",
                     "verify_partition_fit(", "verify_release_pair(",
                     "build_manifest(", "assert_manifest_private_free("):
            self.assertIn(call, src, "main() never calls " + call)

    def test_the_one_canonical_revision_reaches_web_and_firmware(self):
        import inspect
        src = inspect.getsource(w5.main)
        self.assertEqual(src.count("resolve_identity("), 1)
        self.assertIn("build_frontend(repo, identity.revision", src)
        self.assertIn("revision=identity.revision", src)
        self.assertIn("verify_release_pair(build_dir, identity.revision)", src)

    def test_the_frontend_build_hands_over_the_canonical_revision(self):
        import inspect
        src = inspect.getsource(w5.build_frontend)
        self.assertIn("canonical_revision.REVISION_ENV", src)
        self.assertIn("built != revision", src)

    def test_release_pair_equality_is_mandatory_in_the_manifest(self):
        ident = w5.canonical_revision.BuildIdentity(
            commit="d" * 40, revision="v0.0.0-0-gdeadbee", dirty=False,
            branch="b")
        cfg = w5.read_private_config(env())
        pair = {"firmwareRevision": ident.revision,
                "webRevision": ident.revision, "releasePairVerified": True}
        m = w5.build_manifest(ident, cfg, "digest", pair)
        self.assertEqual(m["firmwareRevision"], m["webRevision"])
        self.assertEqual(m["firmwareRevision"], m["canonicalGitDescribe"])
        with self.assertRaises(w5.PilotError):
            w5.build_manifest(ident, cfg, "digest",
                              {"firmwareRevision": ident.revision,
                               "webRevision": "v0.0.0-0-gfeedfac"})


def _synthetic_env(case):
    """Give main() a valid synthetic configuration for the duration."""
    import os
    saved = {k: os.environ.get(k) for k in w5.ENV_VARS}
    for key, value in env().items():
        os.environ[key] = value

    def restore():
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    case.addCleanup(restore)


class FailureCleanup(unittest.TestCase):
    """The finally path must clear private and partial state at every stage."""

    def _drive(self, out_dir, fail_at):
        real = {name: getattr(w5, name) for name in
                ("resolve_identity", "build_frontend", "run_build")}

        def boom(*_a, **_k):
            w5.fail("synthetic " + fail_at + " failure")

        def good_identity(_repo):
            return w5.canonical_revision.BuildIdentity(
                commit="d" * 40, revision="v0.0.0-0-gdeadbee", dirty=False,
                branch="b")
        try:
            w5.resolve_identity = boom if fail_at == "identity" else good_identity
            w5.build_frontend = boom if fail_at == "frontend" \
                else (lambda *a, **k: "v0.0.0-0-gdeadbee")
            if fail_at == "firmware":
                w5.run_build = boom
            return w5.main(["--output", str(out_dir), "--repo", str(REPO)])
        finally:
            for name, fn in real.items():
                setattr(w5, name, fn)

    def _cleanup_case(self, stage):
        import os
        import shutil
        import tempfile
        _synthetic_env(self)
        out = Path(tempfile.mkdtemp(prefix="nx-w6-out-")) / "artifact"
        tmp_root = tempfile.gettempdir()
        before = {d for d in os.listdir(tmp_root) if d.startswith("nx-w6-")}
        try:
            rc = self._drive(out, stage)
            self.assertEqual(rc, 2, stage + " failure must exit 2, not raise")
            self.assertFalse((out / "manifest.json").exists(),
                             "a partial manifest survived")
            after = {d for d in os.listdir(tmp_root) if d.startswith("nx-w6-")}
            leaked = after - before
            self.assertEqual(leaked, set(), "leaked work dirs: %s" % leaked)
        finally:
            shutil.rmtree(out.parent, ignore_errors=True)

    def test_cleanup_after_identity_stage_failure(self):
        self._cleanup_case("identity")

    def test_cleanup_after_frontend_stage_failure(self):
        self._cleanup_case("frontend")

    def test_cleanup_after_firmware_stage_failure(self):
        self._cleanup_case("firmware")

    def test_no_private_value_reaches_stdout_stderr_or_the_error(self):
        import contextlib
        import io
        import shutil
        import tempfile
        _synthetic_env(self)
        out = Path(tempfile.mkdtemp(prefix="nx-w6-out-")) / "artifact"
        stdout, stderr = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(stdout), \
                    contextlib.redirect_stderr(stderr):
                self._drive(out, "firmware")
        finally:
            shutil.rmtree(out.parent, ignore_errors=True)
        blob = stdout.getvalue() + stderr.getvalue()
        for secret in (SYN_NTP, SYN_LAT, SYN_LON, "europe/brussels",
                       str(SYN_LAT_E4), str(abs(SYN_LON_E4))):
            self.assertNotIn(secret, blob, repr(secret) + " reached the console")

    def test_the_tracked_tree_is_unchanged_outside_the_pilot_tooling(self):
        import subprocess
        out = subprocess.run(["git", "status", "--porcelain"], cwd=str(REPO),
                             capture_output=True, text=True).stdout
        changed = [line[3:] for line in out.splitlines()
                   if line[:2].strip() and not line.startswith("??")]
        unexpected = [c for c in changed if not c.startswith("tools/pilot/")]
        self.assertEqual(unexpected, [],
                         "unexpected tracked changes: %s" % unexpected)

if __name__ == "__main__":
    unittest.main(verbosity=2)
