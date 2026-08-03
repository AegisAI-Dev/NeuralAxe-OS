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


def env(provider="open-meteo", lat=SYN_LAT, lon=SYN_LON, tz="europe/brussels"):
    out = {}
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
        for missing in (w5.ENV_PROVIDER, w5.ENV_LATITUDE,
                        w5.ENV_LONGITUDE, w5.ENV_TIMEZONE):
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
                      "longitude", "EUROPE_BRUSSELS", "Kalmthout", "city"):
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
                          "open-meteo", "europe/brussels"):
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
                         ("NX_WEATHER_PROVIDER", "NX_WEATHER_LATITUDE",
                          "NX_WEATHER_LONGITUDE", "NX_WEATHER_TIMEZONE"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
