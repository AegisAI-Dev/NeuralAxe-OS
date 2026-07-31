# NeuralAxe OS — Gate W3: Weather Provider, Strict HTTPS and Brussels Local Scheduler — Phase B Report

Phase: Weather-Aware Tuning, Gate W3 · Branch: `neuralaxe-v0.1-weather-aware-tuning-weather-client`
Committed implementation: **d3e1869** ("feat: add provider-neutral weather client and Brussels local scheduler") = **v2.14.2-60-gd3e18699**, parent 0bee08b (v2.14.2-59, Gate W2 closed).
Phase B verification date: 2026-07-31 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gate W3 FULLY GREEN.** The committed components `components/weather_client/`
and `components/local_schedule/` deliver the W3 contract — a provider-neutral
weather model, an Open-Meteo adapter behind a fixed host/path allowlist, a
strict bounded parser, a compile-only ESP-IDF HTTPS adapter with mandatory
certificate and hostname validation, a pure Europe/Brussels DST conversion,
daily 05:00/11:00/15:00 scheduling with exactly-once local-slot semantics,
bounded catch-up, bounded retries, a daily request budget, a forecast cache
and the W1 climate-policy bridge — with fake network tests only.

The gate is **isolated and unwired**: no runtime weather task, no production
SNTP startup, no NVS write, no tuning application, no frequency/voltage/fan/
thermal write, no B5 ownership wiring, no API route, no frontend change, no
boot integration, no restart, no OTA and no hardware access. This is proven at
the binary level, not only by source inspection (§5).

Phase A of this gate was a **recovery**: the previous session's working tree
had never compiled. Two genuine defects were repaired and four missing test
files were written before validation (§4).

## 2. Committed diff (verified)

`git diff --stat 0bee08b..d3e1869` — exactly 31 files, 5,429 insertions,
1 deletion; all 31 committed blobs verified LF via `git ls-files --eol`
(`i/lf w/lf` for every path):

| File | Lines |
|---|---|
| `components/local_schedule/CMakeLists.txt` | 8 |
| `components/local_schedule/brussels_time.c` | 261 |
| `components/local_schedule/include/brussels_time.h` | 119 |
| `components/local_schedule/include/local_schedule.h` | 162 |
| `components/local_schedule/local_schedule.c` | 314 |
| `components/local_schedule/test/CMakeLists.txt` | 3 |
| `components/local_schedule/test/test_brussels_time.c` | 238 |
| `components/local_schedule/test/test_local_schedule.c` | 364 |
| `components/weather_client/CMakeLists.txt` | 22 |
| `components/weather_client/include/weather_bridge.h` | 44 |
| `components/weather_client/include/weather_cache.h` | 56 |
| `components/weather_client/include/weather_forecast.h` | 127 |
| `components/weather_client/include/weather_open_meteo.h` | 52 |
| `components/weather_client/include/weather_provider.h` | 156 |
| `components/weather_client/include/weather_retry.h` | 107 |
| `components/weather_client/include/weather_transport.h` | 97 |
| `components/weather_client/test/CMakeLists.txt` | 3 |
| `components/weather_client/test/test_weather_bridge.c` | 488 |
| `components/weather_client/test/test_weather_forecast.c` | 199 |
| `components/weather_client/test/test_weather_open_meteo.c` | 649 |
| `components/weather_client/test/test_weather_provider.c` | 287 |
| `components/weather_client/test/test_weather_retry.c` | 189 |
| `components/weather_client/test/test_weather_transport.c` | 364 |
| `components/weather_client/weather_bridge.c` | 67 |
| `components/weather_client/weather_cache.c` | 76 |
| `components/weather_client/weather_forecast.c` | 103 |
| `components/weather_client/weather_open_meteo.c` | 372 |
| `components/weather_client/weather_open_meteo_http.c` | 184 |
| `components/weather_client/weather_provider.c` | 153 |
| `components/weather_client/weather_retry.c` | 164 |
| `test/CMakeLists.txt` (root) | 1 line: `local_schedule weather_client` added to `TEST_COMPONENTS` |

No file under `main/` changed; no existing component changed; no W1 or W2
production source changed; no report document was included in the code commit
(two-gate workflow). `test-ci/CMakeLists.txt` remains the untouched symlink
blob (mode 120000 → `../test/CMakeLists.txt`).

## 3. Phase B verification results (all against d3e1869, clean tree)

| Check | Result |
|---|---|
| QEMU unit suite (esp32s3, **IDF v5.5.3**, QEMU 9.2.2) | **588 Tests, 0 Failures, 0 Ignored** — 68 new W3 cases + the intact 520-test baseline; 0 `:FAIL` lines |
| Frontend gate (`npm ci` + `npm run test:gate`, Brave headless) | `npm ci` exit 0; **1052 / 1052 SUCCESS**, gate exit 0 |
| Default firmware build | **OK** — `esp-miner.bin` **1,658,208 B**, `www.bin` 3,145,728 B |
| Feature-enabled build (`CONFIG_NX_TIMED_SESSIONS=y`) | **OK** — `#define CONFIG_NX_TIMED_SESSIONS 1` confirmed; `esp-miner.bin` **1,658,208 B**, byte-identical to default |
| Strict warnings (`gcc -std=c11 -Wall -Wextra -Werror` **plus** `-Wshadow -Wconversion -Wsign-conversion -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes`, all 8 pure sources) | **Clean** |
| Real ESP-IDF HTTPS adapter compiles | **Yes** — `weather_open_meteo_http.c.obj` produced by both the test-ci and firmware builds |
| Production runtime caller | **Zero** — no source reference outside the two components; `nm` on the linked ELF finds **0** `weather_*` / `brussels_*` / `om_http_*` symbols |
| Endpoint reachable from the shipped app | **None** — `strings esp-miner.bin \| grep open-meteo` returns **0** matches |
| Real HTTP / DNS / SNTP in tests | **None** — 0 WiFi/netif/DHCP/SNTP lines in the QEMU serial log; the only transport used by tests is the in-file fake |
| Forbidden-symbol scan | **Clean** — no `setenv`/`tzset`/`localtime`/`mktime`/`gettimeofday`/`sntp_*` call sites, no `nvs_*`, `esp_restart`, `esp_ota*`, FreeRTOS task/queue/semaphore, `esp_timer_*`, `ESP_LOG*`, `httpd_*`, `esp_wifi_*`, `socket()`, `connect()`, `getaddrinfo`, `gethostbyname`, `malloc/calloc/realloc` |
| Secret / local-path scan | **Clean** — matches are documentation comments and the deliberate forbidden-substring list inside a privacy test |
| URL / hostname allowlist audit | **Clean** — exactly one endpoint hostname literal (`"api.open-meteo.com"`), one display-attribution URL (`"https://open-meteo.com/"`, never fetched), the scheme-fixed composer `"https://%s%s?%s"`, and the licence text `"Weather data by Open-Meteo.com"`. The single `http://` occurrence is a comment stating that no plain-HTTP URL is representable |
| Line endings | All 31 committed blobs **LF** (`git ls-files --eol` + byte-level `ReadAllBytes` check) |
| Git writes by the assistant | **None** — the commit was made by the owner via GitHub Desktop |
| External systems | **None** — no weather request, no hardware, no owner LAN, no DNS, no NTP, no pools, no OTA, no restart, no tuning application |

Runner mirror of `unittest.yml`: repo mounted read-only into
`espressif/idf:v5.5.3`, tree copied in-container, `test-ci/CMakeLists.txt`
symlink restored, stale generated `test-ci/sdkconfig` dropped, 16 MB merged
flash image, `qemu-system-xtensa -machine esp32s3`.

### Test-count breakdown (68 new cases)

| Suite | Cases |
|---|---|
| `test_brussels_time.c` | 10 |
| `test_local_schedule.c` | 11 |
| `test_weather_forecast.c` | 4 |
| `test_weather_provider.c` | 9 |
| `test_weather_open_meteo.c` | 15 |
| `test_weather_retry.c` | 7 |
| `test_weather_bridge.c` | 7 |
| `test_weather_transport.c` | 5 |
| **W3 total** | **68** |
| Pre-existing baseline (intact) | **520** |

## 4. Phase A recovery record

The interrupted session left 27 files on disk that had **never compiled**.
Nothing was regenerated; the following minimal corrections were applied.

1. **`local_schedule.h`** pinned `WEATHER_SCHED_REASON__COUNT == 16`, but the
   enum has 15 values (and the token table has exactly 15 cases) → the
   `_Static_assert` failed the build. Pin corrected to 15.
2. **`weather_open_meteo.c`** formatted a `uint32_t` with `%u`; on xtensa
   `uint32_t` is `long unsigned int` → `-Werror=format=`. Both parts of the
   coordinate rendering are bounded (≤ 180 and ≤ 9999), so explicit
   `(unsigned)` conversions are exact and the format stays portable.
3. **Missing coverage.** Only `test_weather_forecast.c` and
   `test_weather_retry.c` existed; there were no tests for the provider
   registry/eligibility/attribution, the request builder, the parser, the
   transport rules, the W1 bridge or an end-to-end fake-network path — i.e.
   the gate's core safety proofs were absent. Four files were written:
   `test_weather_provider.c`, `test_weather_open_meteo.c`,
   `test_weather_bridge.c`, `test_weather_transport.c`.

No W1 or W2 production source contained a defect and none was modified. No
forbidden artifact (`.claude/`, `launch.json`, `report.xml`, `dist/`,
`generated/`, `coverage/`, screenshots, `__pycache__`, owner-local paths) was
introduced inside the W3 boundary; the pre-existing gitignored Karma
`report.xml` was removed during Phase A hygiene.

## 5. Isolation proof (binary level)

Source grep alone cannot prove "unwired", because ESP-IDF places every
component archive on the link line. The stronger evidence:

- `build/esp-miner.map` lists `liblocal_schedule.a` and `libweather_client.a`
  as `LOAD` entries — they are offered to the linker.
- `xtensa-esp32s3-elf-nm build/esp-miner.elf | grep -E ' (weather_|brussels_|om_http_)'`
  returns **zero** symbols: no object is extracted from either archive,
  because nothing references them.
- `strings build/esp-miner.bin | grep open-meteo` returns **zero** matches —
  the endpoint host string is not in the shipped application at all.
- The default and `CONFIG_NX_TIMED_SESSIONS=y` binaries are **byte-identical
  in size** (1,658,208 B), consistent with W3 contributing nothing to the app.

The only reference to either component outside their own directories is the
`TEST_COMPONENTS` line in `test/CMakeLists.txt` — a build registration for the
QEMU suite, not a runtime caller.

## 6. Provider architecture and eligibility

- `WeatherProviderId`'s zero value is `UNCONFIGURED`, so a zeroed configuration
  is unconfigured by construction. `weather_provider_get()` returns **NULL**
  for `UNCONFIGURED` and for unknown ids: an unconfigured provider has no
  interface and can never be invoked. Open-Meteo is therefore never an
  implicit operational default.
- `WeatherDistributionMode` models the unresolved Gate W0 product decision.
  `UNSPECIFIED` (the zero default) blocks the public endpoint, as do
  `COMMERCIAL` and `SELF_HOSTED`; only an explicit
  `PERSONAL_NONCOMMERCIAL` yields `WEATHER_PROVIDER_OK`. Out-of-range values
  fail closed. W3 does **not** persist this mode — the committed W2 schema has
  no such field, and wiring it is a W4/W5 prerequisite.
- **No API key exists anywhere** in the component, the request model, the
  transport, the tests or the build.
- Attribution metadata (`"Open-Meteo"`, `"Weather data by Open-Meteo.com"`,
  `https://open-meteo.com/`, `CC BY 4.0`) is static display data required by
  the data licence; the URL is never fetched by this component.

## 7. Request, HTTPS and parser contract

**Request.** One compile-fixed host (`api.open-meteo.com`) and path
(`/v1/forecast`); the `WeatherRequest` struct has no field in which any other
host, path or URL could appear. The query is deterministic and order-fixed:

```
latitude=<lat>&longitude=<lon>&daily=temperature_2m_max&current=temperature_2m
&timezone=Europe%2FBrussels&forecast_days=1&temperature_unit=celsius
```

Coordinates are scaled integers rendered to exactly four decimals by integer
arithmetic — no float formatting, no locale dependency, no scientific
notation, `0.0000` canonical. Rejected by test: NULL arguments, a non-Brussels
timezone id, unset `0/0` coordinates and out-of-range coordinates; the output
is cleared, never half-built. A privacy test asserts the query contains none
of `key, apikey, api_key, token, auth, secret, password, user, worker, wallet,
serial, mac, device, uuid, hostname, session, client, hashrate, asic,
firmware`. There is no request body and no credential field in the model.

**HTTPS (compile-only adapter).** `HTTP_TRANSPORT_OVER_SSL`;
`crt_bundle_attach = esp_crt_bundle_attach` with the default (strict) hostname
check; `skip_cert_common_name_check` never set; no insecure option and no
plaintext fallback; GET only, no body, `Accept: application/json`;
`disable_auto_redirect = true` with `max_redirection_count = 0`; bounded 8 s
timeout; bounded read into a fixed 4 KiB buffer with `Content-Length` overflow
rejected before reading and chunked overflow rejected during reading; cleanup
on every path; no URL or body logging; TLS/DNS/TCP connect failures surface
only as sanitized machine codes. The adapter re-validates the allowlisted
host and path before composing the URL.

**HTTP rules (shared by fake and real paths).** oversized →
`ERR_RESPONSE_TOO_LARGE`; incomplete → `ERR_TRUNCATED`; any 3xx →
`ERR_REDIRECT_REJECTED` (never followed); other non-2xx → `ERR_HTTP_STATUS`;
wrong Content-Type **and a missing Content-Type** → `ERR_CONTENT_TYPE` (the
conservative policy: unavailability degrades toward the safe hot stance);
empty 2xx body → `ERR_TRUNCATED`.

**Parser bounds.** Cap 4096 B, enforced at read and re-checked in the parser.
Rejections proven by test: embedded NUL, malformed JSON, trailing
non-whitespace garbage (trailing whitespace is accepted), non-object root,
`error:true` on a 200, wrong `timezone` string, wrong abbreviation, missing or
non-CET/CEST `utc_offset_seconds`, a numeric-looking **string** offset,
non-Celsius units, missing `daily`/`daily_units`, non-array series, mismatched
array lengths, zero rows, ambiguous multi-row payloads, nine malformed date
shapes, a non-string date entry, wrong-date rows (both yesterday and
tomorrow), null/string/boolean/object/array/out-of-band/absurd-magnitude
values, and a present-but-broken `current` block. Every rejection
re-initializes the output — a partially valid forecast cannot exist. Rounding
is deterministic (half away from zero) and the ±60.0 °C sanity band is
inclusive at the edges.

## 8. Brussels time, scheduling, retries, budget and cache

- **DST rule.** Pure UTC integer arithmetic; **no** `setenv("TZ")`, `tzset()`,
  `localtime()` or `mktime()` anywhere (those tokens appear only inside
  prohibition comments). DST begins on the last Sunday of March at 01:00 UTC
  and ends on the last Sunday of October at 01:00 UTC, verified second-exact
  against the public 2025 anchors and the 2025–2028 last-Sunday calendar.
  Spring gap → the first valid instant after the gap (`dst_adjusted`);
  autumn overlap → the **first** occurrence (`dst_ambiguous`), never the
  second. Supported epoch band 2025-01-01 … 2100-01-01, rejected never wrapped.
- **Exactly-once.** Executed slots live in a bounded bitmask keyed to one local
  date. Duplicate wakeups over the same `(config, progress, now)` produce
  byte-identical plans; applying the proposed progress makes the slot
  permanently non-due for that date; a backward *time* correction never
  re-runs a completed slot; a backward *local date* refuses execution instead
  of resetting progress; the repeated autumn hour cannot double-run a slot.
  With untrusted time no execution decision is ever produced.
- **Catch-up.** Only the single **latest** missed slot is offered, never a
  burst; the window boundary is exact; `catch_up_window_s == 0` (the shipped
  default) disables catch-up entirely pending a committed product decision.
- **Retries.** Two attempts per execution (matching the W2
  `TUNING_MAX_RETRY_COUNT`), linear monotonic backoff `(N+1)×60 s`, never a
  wall clock. Stale execution ids and duplicate/out-of-order results are
  idempotent no-ops; backoff overflow saturates at `UINT64_MAX`.
- **Daily budget.** Saturating `uint8` counters that never wrap, a hard model
  bound of 24, an invalid `daily_max` that fails closed, and exhaustion that
  produces no call intent. Rollover resets only on a **trusted, strictly
  forward** local date — a backward correction or untrusted time never resets.
- **Cache.** In-memory and version-tagged (W3 does not persist it: the
  committed W2 schema has no compatible field). Only a validated forecast with
  a trusted fetch epoch is storable. Empty cache, provider mismatch, a
  previous-date entry, a future fetch epoch, over-age or an untrusted `now`
  all return `ERR_CACHE_STALE`.

## 9. W1 bridge and the two safety proofs

The bridge emits exactly one `TuningForecastInput`: a status plus the daily
maximum. Usable → `TUNING_FORECAST_OK` + `forecast_max_dc`; technical faults →
`INVALID`; date/staleness → `STALE`; everything else → `UNAVAILABLE`. An
untrusted `now` is always `STALE`, and even an `OK` provider result must still
pass the full usability rule (defense in depth).

**Proof 1 — the current outdoor temperature cannot affect any selection.**
`test_weather_bridge.c` drives the *complete committed W1 chain* (bridge →
climate hysteresis → precedence arbitration) and sweeps `current_dc` across
`{-600, -200, 0, 150, 280, 300, 450, 600}` plus `current_valid == false`, in
both directions:

- an **upgrade** control — a cool daily maximum from `emergency-thermal-safe`
  selects `supersink-max` with `is_upgrade` true;
- a **downgrade** control — a hot daily maximum from `supersink-max` selects
  `hot-weather-safe` with `is_downgrade` true.

In every iteration the `TuningForecastInput` is byte-identical and the full
`TuningSelectionIntent` (action, profile id, actor, winning source, both
reasons, override result, direction flags) is unchanged. A 60.0 °C reading
"right now" beside a 20.0 °C daily maximum still follows the maximum.

**Proof 2 — production profiles remain unselectable.** All three
`tuning_registry_gamma601()` descriptors are asserted `UNVALIDATED`,
`payload_present == false`, zero frequency and zero core voltage, with
`tuning_profile_auto_eligible() == ERR_NOT_VALIDATED`. Sweeping the forecast
band `{-600, 0, 200, 279, 280, 299, 300, 350, 600}` through the real chain
against the **production** registry yields no `SELECT_PROFILE` and no upgrade
for any input. Upgrades appear only against a synthetic in-test registry,
which is what makes the negative result meaningful.

**Stale data never upgrades.** Previous-date, over-age, future-epoch,
untrusted-fetch and transport-failure inputs all reach
`TUNING_CLIMATE_REQ_FAIL_SAFE`; from the most aggressive profile the same
failure produces a strict **downgrade** under `TUNING_SOURCE_WEATHER_FAIL_SAFE`,
never an upgrade.

## 10. Engineering notes for later gates

1. **W1 direction semantics.** `HOT` is a *safety* direction: `safety_select`
   only ever selects a strict downgrade (or moves from an unknown current).
   The single upgrade path in the whole policy is
   `TUNING_CLIMATE_REQ_COOL_PROFILE`. Any future test that needs to prove "an
   upgrade was possible here" must use a cool forecast from the safest current
   profile — a hot forecast can never demonstrate one.
2. **Transport error granularity.** The ESP-IDF surface does not separate
   DNS, TCP and TLS connect failures without deeper introspection, so all
   connect-phase failures currently map to `ERR_CONNECT_TIMEOUT`.
   `ERR_TLS_FAILURE` and `ERR_DNS_FAILURE` exist in the code space but are
   produced only by fakes today; finer discrimination is a W4/W6 measurement
   task.
3. **Version-ready, not persisted.** `WeatherScheduleProgress` and
   `WeatherForecastCache` both carry explicit version fields for a later
   reviewed persistence contract; W3 deliberately persists neither.
4. **`WeatherFetchStats`** is pure bookkeeping for a future W4/W6 runtime: all
   fields stay zero until a real runtime fills them, and none of it is ever
   sent to a provider.
5. **Verification gotchas.** The `nx-qemu-action` image ships **IDF v5.5.4**;
   the authoritative suite must be built in `espressif/idf:v5.5.3` with only
   `qemu-system-xtensa` borrowed from the other image — confirm via the
   `ESP-IDF vX.Y.Z 2nd stage bootloader` banner in the serial log. Docker on
   this machine needs `MSYS_NO_PATHCONV=1` and absolute Windows paths for
   `-v`. A firmware build in a container needs only `.git` plus the prebuilt
   `main/http_server/axe-os/dist` when `GITHUB_ACTIONS=true` is set (npm is
   skipped entirely). Copying this Windows checkout into a Linux container
   makes `git status` report ~20 unrelated files as modified — that is the
   known CRLF/`core.symlinks=false` artifact, not a real change; the
   host-side status is authoritative.

## 11. Scope confirmations

No weather network request, no DNS, no NTP/SNTP, no hardware access, no owner
LAN, no real pool, no OTA, no restart, no tuning application, no
frequency/voltage/fan/thermal write, no NVS write, no B5 ownership wiring, no
API route, no frontend change, no boot integration, no edits outside the two
new components plus one test-registry line, and no Git write operations by the
assistant in either phase. The private TCH recovery data was not touched. All
production profiles remain UNVALIDATED and payload-free, and the feature
cannot be enabled against the production registry.

## 12. Owner action (second gate)

Commit this report (suggested: `docs: add Phase W3 weather client and local
scheduler report`). That closes Gate W3.

W4 — the runtime weather task, trusted-time startup, B5 ownership acquisition,
profile application and the API/frontend surface — was not started and remains
blocked on the open items in §10 and on the unresolved distribution-mode
product decision (§6).
