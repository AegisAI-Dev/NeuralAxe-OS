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
    IDENTITY = {"describe": "v0.0.0-0-gdeadbee", "commit": "d" * 40,
                "branch": "test"}

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

    IDENT = {"describe": "v0.0.0-0-gdeadbee", "commit": "d" * 40, "branch": "t"}

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

    def test_release_pair_requires_exact_string_equality(self):
        import shutil
        import tempfile
        work = Path(tempfile.mkdtemp(prefix="nx-w6-pair-"))
        try:
            web = work / "web"
            web.mkdir()
            (web / "version.txt").write_text("v2.14.2-76-gf4aee75\n",
                                             encoding="utf-8")
            out = w5.verify_release_pair(work, web, "v2.14.2-76-gf4aee75")
            self.assertTrue(out["releasePairVerified"])
            with self.assertRaises(w5.PilotError):
                w5.verify_release_pair(work, web, "v2.14.2-76-gf4aee76")
            (web / "version.txt").unlink()
            with self.assertRaises(w5.PilotError):
                w5.verify_release_pair(work, web, "v2.14.2-76-gf4aee75")
        finally:
            shutil.rmtree(work, ignore_errors=True)

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
            full = chr(10).join(lines)
            (cfgdir / "sdkconfig.h").write_text(full, encoding="utf-8")
            w5.verify_sdkconfig_h(work)

            # Exactly one option removed: the counters.
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
