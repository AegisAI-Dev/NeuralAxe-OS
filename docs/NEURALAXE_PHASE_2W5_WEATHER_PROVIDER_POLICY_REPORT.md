# NeuralAxe OS — Gate W5: Weather Provider, Distribution Mode and Private Location Policy — Phase B Report

Phase: Weather-Aware Tuning, Gate W5 · Branch: `neuralaxe-v0.1-weather-provider-policy`
Committed implementation: **a4a0d6b** ("feat: add default-off weather provider, distribution and private location policy") = **v2.14.2-75-ga4a0d6b**, parent `9f6592b` (the committed Gate W4 report). 15 files changed, 2,106 insertions, 2 deletions.
Phase B verification date: 2026-08-04 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gate W5 FULLY GREEN.** The commit closes the provider/distribution/location decision deferred from Gate W0 with a new pure component `components/weather_source_policy/`, behind `CONFIG_NX_WEATHER_SOURCE_POLICY` (**default n**, depending on the already default-off `CONFIG_NX_WEATHER_AWARE_TUNING`).

The gate decides only **whether** a weather source may be used, **which** provider, and **where**. It authorizes nothing further: no frequency, voltage, fan or thermal change; no pool or protocol change; no timed session, lease, B5 ownership or B7 execution; no SNTP start or second trusted-time anchor; no HTTP route; no task, queue or timer; no restart. Gate W4's recommendation-only contract is preserved unchanged, and this is proven at the **binary level** (§5), not only by source inspection.

| Check | Result |
|---|---|
| Committed files | exactly **15** — 11 added + 4 modified |
| Ancestry | `a4a0d6b` → `9f6592b` (W4 report) → `74a6178` (W4); `d256deb` is an ancestor |
| `test-ci/` in the commit | **0 files** |
| `docs/` or frontend in the commit | **0 files** |
| Split QEMU — baseline (B1–B10.2) | **851 Tests, 0 Failures, 0 Ignored** |
| Split QEMU — weather (W1–W4 + W5) | **285 Tests, 0 Failures, 0 Ignored** (254 + **31 W5**) |
| **Deduplicated total** | **1136 / 0 failures / 0 ignored** |
| Frontend `npm run test:gate` | **1270 / 1270, exit 0** |
| Production frontend build | exit 0 |
| Build-helper tests | **20 / 20** |
| Firmware postures A–F | all exit 0 (§5) |
| Strict compile | **0 failures** across 7 translation units |
| Static RAM impact | **+8 bytes**; configuring a location costs **0 additional bytes** |
| W1–W3, W4 runtime, B10.1, B10.2, canonical release identity | **0 lines changed** |
| Privacy / coordinate / URL / hostname scans | clean |
| Working tree after verification | clean; `test-ci/` byte-exact; no build output, `dist`, `sdkconfig`, `report.xml`, `__pycache__` or `.pyc` |
| Access | no hardware, COM/USB, physical NVS, weather service, NTP, DNS, pools, OTA, restart, owner LAN or recovery data |

## 2. What Gate W5 delivers

**`components/weather_source_policy/` (9 new files)**:

- **`nx_weather_source.h/.c`** — the pure private configuration model and its validator. No I/O, no allocation, no clock, no logging.
- **`nx_weather_source_build.c`** — the single point where a build-time private value becomes a runtime value, fed only by Kconfig symbols that all default to *not selected*.
- **`nx_weather_source_boot.c`** — the flag-gated ordered configuration gate and the only logging site.
- **`test/`** — 31 deterministic tests.

**`tools/pilot/build_weather_recommendation_pilot.py` (+ tests)** — the reproducible private-configuration helper.

**Integration (4 modified files)** — `main/Kconfig.projbuild` (+104 lines: the W5 flag plus three `choice` blocks and two range-checked integers), `main/CMakeLists.txt` (+6: unconditional `weather_source_policy` requirement, for the same ESP-IDF reason as `weather_runtime`), `main/main.c` (+11: a guarded include and call nested inside the existing W4 block), `test/CMakeLists.txt` (+3: the component added to the union and the documented split-run list).

## 3. Product policy as implemented

**Default state.** A default build has `CONFIG_NX_WEATHER_AWARE_TUNING=n`, `CONFIG_NX_WEATHER_SOURCE_POLICY=n`, distribution `UNSPECIFIED`, provider `NONE`, timezone `NONE`, latitude `0`, longitude `0`. Enabling the W5 flag *alone* still yields an unconfigured device that issues no request.

**Distribution mode.** `NX_WEATHER_DIST_UNSPECIFIED` (zero default), `NX_WEATHER_DIST_DISABLED` (an explicit, auditable "switched off" decision, distinct from "nobody has said"), and `NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL` — the only mode that permits a provider. It asserts that the owner supplied the configuration, that NeuralAxe inferred neither location nor provider, that the source is not reachable through the unauthenticated timed-session API, and that the owner is responsible for the external service and its terms. It **maps onto** the committed W3 licence gate (`weather_provider_eligible`) rather than duplicating or widening it; every other mode maps to the committed `UNSPECIFIED`, which W3 already refuses.

**Provider policy.** Exactly one supported provider — the committed W3 Open-Meteo adapter — selected by **identity**. There is no host, scheme, path or query field anywhere in the W5 model, so an arbitrary endpoint is not injectable and the adapter keeps its fixed allowlist.

**Location policy.** Explicit only. The model has no notion of an IP address, Wi-Fi SSID, GPS fix, public-IP lookup, reverse geocode, city name or timezone-as-location — none of those inputs exist in any structure or function, so none can be consulted by construction.

**Security boundary.** HTTPS retrieval is an operational transport mechanism, not proof that forecast data is true. Weather remains advisory: a response can never directly authorize a frequency, voltage, fan, thermal, pool, protocol, mining or restart action.

## 4. Private location model

Coordinates are signed scaled integers, **degrees × 1e4**.

**A deliberate deviation from the suggested microdegrees, on a technical ground.** The committed repository already fixes e4 in *both* the W3 request parameters (`WeatherRequestParams.latitude_e4`) and the W2 persisted NVS record (`TuningSourceRecord.latitude_e4`). Introducing microdegrees would add a lossy conversion at two committed boundaries and contradict the W2 wire schema. 1e-4 degrees is ~11 m — far finer than a forecast grid needs — and the owner remains free to supply a deliberately coarse value.

**The unset sentinel.** `0/0` is a real point at sea, but the repository reserves it to mean "no location supplied", so W5 rejects it as `INCOMPLETE`. An unconfigured device therefore cannot request a location nobody chose. Exactly one coordinate being zero is likewise `INCOMPLETE`.

**No label.** The committed W2 record carries a presentation-only `location_label[32]`. W5 deliberately defines **no equivalent**: the configuration contains no character array at all, so a city or site name cannot be stored, projected or printed. This is asserted structurally by a test.

## 5. Binary-level isolation

| Posture | Configuration | Image | W5 syms | W4 syms | EXEC | API | SNTP init | tasks |
|---|---|---|---|---|---|---|---|---|
| **A** | default (all flags off) | 1,658,416 B | **0** | 0 | 0 | 0 | 0 | 0 |
| **B** | W4 only | 1,668,816 B | **0** | 9 | 0 | 0 | 0 | 0 |
| **C** | W5 compiled, unconfigured | 1,670,496 B | 7 | 9 | 0 | 0 | 0 | 0 |
| **D** | W5 + synthetic private config | 1,670,512 B | 7 | 9 | **0** | **0** | **0** | **0** |
| **E** | weather + timed-session foundation | 1,705,568 B | 7 | 9 | **0** | 1* | 1 | 0 |
| **F** | full timed-session stack, weather off | 1,715,392 B | **0** | 0 | 20 | 1* | 1 | 0 |

\* the single API symbol is `nx_pool_session_api_send_conflict` — the known benign conflict-reporting helper (the same one the Gate B10.1 tooling allowlists), **not** an HTTP route, and present only when `CONFIG_NX_TIMED_SESSIONS` is on. Postures C and D, which enable weather without timed sessions, contain **0**.

Three facts worth stating precisely because they were measured rather than assumed:

- **Posture A contains zero W5 symbols**, so the shipped default image gains nothing from this gate.
- **Posture B also contains zero W5 symbols**, so W5 does not ride along on the W4 flag — it is an independent switch.
- **Posture D differs from posture C by exactly 16 bytes**, which are the configured coordinate and enum constants and nothing else. No extra code, no task, no second SNTP provider, no execution or API symbol appears when a private location is supplied.

**Hardware setters were counted in every posture, not only in D:** `ASIC_set_frequency`, `VCORE_set_voltage`, `EMC2101_set_fan` and `Thermal_set` total **4 in all six postures including A**. They are pre-existing miner setters; W5 adds none and reaches none.

**Static RAM.** Posture A 600,784 B → postures C and D 600,792 B: **+8 bytes** (the two static diagnostics in the boot notice). C and D are identical, so configuring a private location costs **zero additional RAM**. No task, queue or timer is created in any posture. Flash A → D: `.flash.text` +10,492 B, `.flash.rodata` +1,600 B, `.iram0.text` unchanged.

## 6. Runtime configuration gate

The ordered gate, with any failure yielding a passive bounded state and no fallback:

1 feature enabled → 2 distribution valid → 3 provider explicitly selected → 4 private location valid → 5 timezone valid → 6 schedule valid → 7 recommendation-only; then the committed W4 runtime owns 8 trusted time → 9 schedule due → 10 fetch allowed → 11 response fresh and valid → 12 W1 recommendation.

There is no fallback to default coordinates, a default provider, an IP-derived location, a raw wall clock, or stale cached data outside W3 policy. When configuration is missing there are zero network requests, zero policy evaluation, zero recommendation, and normal mining is unchanged.

The boot notice injects **no** clock, transport or store, so even a fully valid configuration performs zero network requests at boot and reports a bounded `WAITING_FOR_TRUSTED_TIME` — a structural fact, not a promise.

## 7. Validation outcomes

Ten stable, value-free tokens: `UNCONFIGURED` (zero default), `INVALID_DISTRIBUTION`, `INVALID_PROVIDER`, `INVALID_LATITUDE`, `INVALID_LONGITUDE`, `INVALID_TIMEZONE`, `INVALID_SCHEDULE`, `INCOMPLETE`, `NOT_RECOMMENDATION_ONLY`, `READY_RECOMMENDATION_ONLY`. Checks run in a fixed order so the reported reason is stable, and a test walks every token asserting it contains no digit, no dot, no host, no URL and no query character — a failure can never be reported by echoing the value that failed.

## 8. Build-time private injection

`tools/pilot/build_weather_recommendation_pilot.py` mirrors the committed B10.1 contracts: clean-tree requirement, explicit output directory, private values only from `NX_WEATHER_PROVIDER` / `NX_WEATHER_LATITUDE` / `NX_WEATHER_LONGITUDE` / `NX_WEATHER_TIMEZONE`, validated before any build, **never printed**, written to one temporary sdkconfig fragment outside the repository that is overwritten and deleted in a `finally` block, tracked-tree digest compared before and after, and the canonical release identity reused rather than reinvented. `--check-config-only` validates without building. There is no default provider, location or timezone.

Degrees are parsed by a strict fixed-point reader, deliberately not `float()`: float parsing is locale-sensitive in some runtimes, admits `nan`/`inf`/`1e3`, and would insert a rounding step between the owner's value and the committed e4 unit. The grammar rejects `1,5`, `1e2`, `nan`, `inf`, `+1.0`, `01.5`, `1.23456`, `0x10` and whitespace-padded forms, and its error message never quotes the offending text.

The manifest records bounded booleans and enums only — `weatherConfigured`, `providerConfigured`, `locationConfigured`, `timezoneConfigured`, `distributionMode`, `sourceStatus`, `recommendationOnly`, `executionEnabled`, `timedSessionApiEnabled`, `hardwareTuningEnabled` — and an `assert_manifest_private_free()` guard fails the build closed if a coordinate, host, URL, timezone identifier or environment-variable name ever reaches it. A test proves the guard rejects a deliberately leaking manifest.

## 9. Test inventory

**31 Gate W5 firmware tests** (QEMU weather image 285 = 254 W1–W4 + 31) plus **20 build-helper tests**:

Configuration validation (fixture-validity guard, defaults, zeroed/NULL, build-binder default, distribution matrix, W3 mapping, provider selection, latitude and longitude boundaries, unset sentinel, half-supplied pair, timezone, schedule, recommendation-only, disabled short-circuit); projection onto the W4 runtime (safe posture on every failure, exact projection on success, W4 defaults preserved, determinism); privacy (no coordinate in any token, no host/URL/city/query in any token, no label field structurally, integers not text); source selection (nothing implicit, endpoint owned by the adapter, unconfigured builds no request, redirects rejected); recommendation-only (no field can request execution, a ready configuration still authorizes nothing, no raw wall-clock fallback).

Helper tests cover fixed-point parsing and its rejections, error messages that never quote values, missing/partial/invalid variables, the zero sentinel, fragment content and out-of-repo shredding, manifest contents and the leak guard, `--check-config-only`, and source hygiene (no default or real coordinate baked into the helper).

## 10. Preserved and unchanged

Verified by diff against the parent commit: **0 lines changed** in W1 profile semantics, W2 persistence schema, W3 parser/client, the W4 runtime, B10.1 observation tooling, B10.2 store preflight, and the canonical release identity (`CMakeLists.txt`, `tools/pilot/canonical_revision.py`). The W3 transport hardening is untouched and still in force: `esp_crt_bundle_attach` with default hostname verification, `skip_cert_common_name_check` never set, `disable_auto_redirect = true` with every 3xx rejected, a hard response cap, a bounded timeout, sanitized error categories and no response-body logging.

## 11. Known limits

- **Configuration only.** W5 decides whether, which and where. Producing a recommendation is Gate W4's job; **applying** one to hardware requires a separate future execution gate that does not exist.
- **No physical validation.** No Gamma hardware, pool, live DNS, real NTP, weather service or network was contacted; the client is exercised with fake transports only, and no real weather-pilot artifact was built.
- **One provider, one timezone.** Open-Meteo and Europe/Brussels are the only supported values; anything else fails closed. The public endpoint stays ineligible under any mode except the explicit owner-managed one.
- **Coordinates remain private configuration.** They exist only in the owner's environment, in one shredded out-of-tree fragment, and in the device's own RAM when explicitly configured.

## 12. Phase B verification appendix

All commands ran offline against the committed tree (`a4a0d6b`, clean status), `espressif/idf:v5.5.3` for builds and the `nx-qemu-action` image for QEMU only. Git was used read-only throughout; the owner performed the commit.

1. `git show --name-status HEAD` — 15 files, 11 A + 4 M; `test-ci/`, `docs/` and frontend untouched; `d256deb` confirmed an ancestor.
2. Committed Kconfig defaults inspected directly from HEAD — flag `default n`, distribution UNSPECIFIED, provider NONE, timezone NONE, coordinates 0 with range guards.
3. Two bounded test builds via the documented `TEST_COMPONENTS` override, build directory and sdkconfig outside the repository — both exit 0, both inside the stock 1 MB partition, `SINGLE_APP_LARGE` absent.
4. Split QEMU — 851/0/0 and 285/0/0; deduplicated 1136; 31 W5 tests counted by name.
5. `npm run test:gate` — 1270/1270, exit 0; production frontend build exit 0.
6. `python -m unittest` on the helper — 20/20.
7. Postures A–F from pristine sdkconfig copies (tracked `sdkconfig` untouched) — exit 0 each; symbol counts as tabled; hardware setters counted in all six.
8. Strict compile of the 7 W4/W5-touched translation units — 0 failures.
9. Regression diffs against the parent for W1–W3, W4, B10.1, B10.2 and release identity — 0 lines.
10. Privacy, coordinate, URL, hostname, geolocation and owner-path scans of the committed diff — clean.
11. `test-ci/` restored byte-exact (22-byte stub); all build artifacts, `dist`, `sdkconfig`, `report.xml` and `__pycache__` removed; tree clean.
