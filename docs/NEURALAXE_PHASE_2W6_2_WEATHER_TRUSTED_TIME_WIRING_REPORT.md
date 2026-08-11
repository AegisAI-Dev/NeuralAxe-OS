# NeuralAxe OS Phase 2W — Gate W6.2
## Weather Trusted-Time Wiring Report

Phase: Weather-Aware Tuning, Gate W6.2 · Branch: `neuralaxe-v0.1-weather-recommendation-pilot`
Committed implementation: **a3c76f6** ("feat: wire live trusted time into weather pilot diagnostics") = **v2.14.2-82-ga3c76f6**, parent `9cfc032` (Gate W6 atomic packaging). 11 files changed, 1,819 insertions, 29 deletions.
Verification date: 2026-08-07 · This report is the only uncommitted file (owner report commit gate).

---

## 1. Verdict

**Gate W6.2 committed-state verification: PASS. Every required property is proven on the committed tree.**

W6.2 answers a defect the previous gate found and could not close: the pilot's `state` and `trusted_time`
were **compile-time constants**. The pilot stepped a throwaway W4 runtime with a NULL clock and cached the
verdict, so `WX_WAITING_FOR_TRUSTED_TIME` / `trusted_time=0` was printed on every device, forever, no matter
what the trusted-time provider did. Thirty minutes of that on real hardware carried exactly zero information.

The commit does three things and nothing else: it **lends** the one committed B2/B10 clock and trust policy to
the weather runtime, it makes the pilot **step that runtime live** instead of replaying a boot verdict, and it
makes the bounded EMPTY-store provider-start budget **reachable** past its first attempt.

It authorizes nothing new. No frequency, voltage, fan or thermal change; no pool or protocol change; no timed
session, lease, B5 ownership or B7 execution; no second SNTP provider, anchor or epoch floor; no HTTP route;
no new task, timer or queue; no persistence; no restart; no weather fetch.

| Check | Result |
|---|---|
| Working tree | **clean** |
| HEAD | `a3c76f6` = **v2.14.2-82-ga3c76f6** |
| Committed files | exactly **11** |
| `test-ci/`, frontend `src/`, OpenAPI in the commit | **0 files** |
| Ancestry | `a3c76f6` → `9cfc032` → `7b1eb99` → `e554306` |
| Split QEMU — baseline (B1–B10.2) | **851 Tests, 0 Failures, 0 Ignored**, 0 panics |
| Split QEMU — weather observation posture | **404 Tests, 0 Failures, 0 Ignored**, 0 panics |
| W6.2 tests inside that posture | **71 PASS / 0 FAIL** (21 projection + 15 wiring, both postures) |
| Frontend gate | **1270 / 1270 SUCCESS** |
| W6 build-helper suite | **85 / 85** — clean-tree guard now **green** |
| Firmware posture build | **Project build complete** |
| Strict compile `-Wall -Wextra -Werror` | **11 / 11** translation units clean |
| Symbol audit | 12/12 required present · 6/6 forbidden absent · exactly **1** SNTP provider |
| Privacy scan | no coordinate in the image; no provider host; no `https://` |

Two accuracy findings surfaced by adversarial verification are recorded in §8. Neither is a safety defect;
both are documentation/scope corrections that a follow-up should make.

---

## 2. What the commit changes

**`components/pool_session_runtime/` (2 files).** Two read-only accessors and one bounded retry.

`pool_session_runtime_clock()` returns `&rt->clock`; `pool_session_runtime_trust_policy()` returns
`&rt->time_policy` — the **address of the live struct members**, not copies. Both return `NULL` for a `NULL`
or uninitialized runtime, so a borrower that ignores the result degrades to an untrusted bounded wait rather
than dereferencing anything.

Handing out a pointer is what makes "no second provider" structural rather than promised: the borrower has no
API that could construct a clock, start SNTP, write an anchor or set an epoch floor. It is also why the
borrower cannot **diverge** — the B4 recovery pass refines `rt->time_policy` *after* `init`, and a by-value
copy taken at bind time would have frozen a stale policy.

`runtime_step_observation_retry()` closes the one-shot defect. Before it, the observation start was reachable
from exactly one place: the owner-task branch that fires on the **first applied** `RUNTIME_EVENT_NETWORK_READY`.
`pool_runtime_control_apply` sets that bit only while `!c->network_ready`, and `outcome.start_time_provider`
requires `RUNTIME_WAITING_FOR_TRUSTED_TIME`, which an EMPTY-store device (FREE) never enters. A device whose
single attempt was refused could therefore never spend attempts 2..`POOL_TIME_SOURCE_ATTEMPTS_MAX`, and — because
`time_observation_active` is only set by a *successful* start — its diagnostics then froze for the whole boot.

**`components/weather_pilot_diag/` (7 files + 2 test files).** `wx_bind_runtime()` borrows the clock and policy
and injects them as `WeatherRuntimeDeps`. Binding is **lazy** because the pilot boot notice runs at `main.c:150`,
before `nx_timed_sessions_boot_init()` at `main.c:161` — at boot there is no runtime to borrow from, so the
notice reports the bounded waiting state honestly instead of constructing a throwaway runtime whose answer
would then be mistaken for a measurement. The pilot now holds **one long-lived** `WeatherRuntime` and steps it
on every observation, so its schedule progress, climate stance and idempotence witness mean what they say.

---

## 3. Required properties — verification results

Ten independent source audits were run, each required to cite `file:line` it had actually read, and each PASS
was then handed to a separate adversary instructed to refute it. Six survived unrefuted; four were partially
refuted, and every refutation is reflected below or in §8.

| # | Required property | Result |
|---|---|---|
| 1 | Clean tree | **PASS** — `git status --porcelain` empty |
| 2 | W6 helper clean-tree guard passes | **PASS** — 85/85, guard green |
| 3 | B10/B10.1 unchanged except the approved retry | **PASS** — §4 |
| 4 | Exactly one SNTP provider | **PASS** — §5 |
| 5 | Weather receives the SAME clock and policy objects | **PASS** — §5 |
| 6 | Live trusted-time diagnostics authoritative | **PASS**, conditional — §8.2 |
| 7 | No raw wall-clock or Stratum time trust | **PASS** — §5 |
| 8 | No second anchor or epoch floor | **PASS** — §5 |
| 9 | No B7 execution | **PASS** — §6 |
| 10 | No timed-session command API routes | **PASS** — §6 |
| 11 | No weather hardware apply | **PASS** — §6 |
| 12 | No pool/protocol/session mutation | **PASS** — §6 |
| 13 | No private NTP value in logs or diagnostics | **PASS**, with a stated residual — §7, §8.1 |

---

## 4. B10/B10.1 behaviour: exactly one approved change

The full diff of `pool_session_runtime.c` contains three additions and nothing else: the two pure accessors,
and `runtime_step_observation_retry()` called once per existing owner-task tick
(`pool_session_runtime.c:1628`). No other behavioural line changed.

The retry is a re-offer of the **existing** path, not a second mechanism. The decision, the bounded budget and
the monotonic backoff all remain in `runtime_start_time_provider()` / `pool_time_source_retry_decide()`. It
returns immediately when the provider is already started, when observation is disabled, when the network is not
ready, when the budget is exhausted, when a session owner exists (`plan.trusted_time_required`,
`plan.restore_required`, `record_present`), or when the source is unconfigured or invalid. Between attempts the
committed backoff (15 s doubling to a 120 s ceiling) refuses, so a tick that is too early costs one comparison.
It performs no restart, no session mutation, no store write and no lease operation, and EMPTY-store mining stays
`ALLOW_SOURCE` throughout — asserted in five QEMU tests.

**Honest limit, verified.** `pool_time_sntp_init()` refuses re-initialization once the provider's lifecycle has
left `UNINITIALIZED` and returns **without calling the platform op**. After a platform init or start failure the
retries therefore make attempts 2–5 *reachable and observable* but cannot revive that provider; only a
deinit/reinit could, which the committed B10 contract deliberately declines. The correct observable is the
runtime's own `pool_session_runtime_time_start_attempts()`, not the platform call count — the tests assert on
the former for exactly this reason.

---

## 5. One provider, one anchor, one floor, no wall clock

`PoolTimeSntpProvider` exists as **one object**: a member of the single static `s_runtime`. There is one
non-test `pool_time_sntp_init` call site, one `esp_netif_sntp_init` binding, and a module-global single-owner
guard that fails a second binder closed. The ELF confirms it: `pool_time_sntp_init` is defined **once**.

The weather side only reads. `weather_time_view_read()` funnels into the same committed B2 entry point the pool
runtime itself uses — `pool_time_snapshot(clock, policy, &snap)` — and returns an untrusted view when either
pointer is `NULL`; it never falls back to a wall clock. `runtime_clock_read_anchor` returns false before the
provider is initialized, and `provider_clock_read_anchor` copies the anchor under the `pool_time` spinlock, so a
cross-task borrower cannot observe a torn value. `pool_time_snapshot` writes only into the caller's output.

No `time()`, `gettimeofday()`, `settimeofday()`, `clock_gettime()`, `localtime()`, `mktime()` or Stratum `ntime`
participates in any trust decision on the weather path. `WeatherTimeView` carries derived values for the
duration of one step; it is written only inside the single trusted branch of `weather_time_view_from_snapshot`
and is never persisted, so it is a derived read, not a second anchor.

**Proven implication:** `trusted(weather) ⇒ trusted(B2/B10)` — untrusted before the anchor, trusted after, and
the epoch weather evaluates at is the one B2 accepted.

---

## 6. No execution, no routes, no mutation

Symbol audit of the built pilot posture:

| Required present | Forbidden absent |
|---|---|
| `pool_time_sntp_init` / `_start` / `_handle_sync` | `pool_session_execution_` |
| `pool_session_runtime_clock` / `_trust_policy` | `pool_session_executor_` |
| `weather_time_view_read`, `weather_runtime_step` | `nx_tps_preflight_` |
| `tuning_policy_evaluate` | `nx_weather_apply_` |
| `nx_weather_pilot_time_project` / `_gather` / `_observe` | `pool_pilot_` |
| `nx_mutation_counter_note` | `weather_open_meteo_http_ops` |

`nx_pool_session_api_send_conflict` is present and is the documented `ALLOWED_DESPITE_PREFIX` exemption in the
build helper — a shared HTTP 409 helper, not a route.

`weather_runtime.c` contains **zero** `malloc`/`calloc`/`strdup`/`vTaskDelay`/`xQueue`/`xSemaphore`; the step is
allocation-free and non-blocking, which is why hosting it on the 1 s statistics task is safe. The pilot component
opens no NVS namespace. Twenty consecutive steps leave `lease_owner = OP_OWNER_NONE`, no session, no restore
obligation, no target mining, no pool-mutation permission, `PROTOCOL_ALLOW_SOURCE` and `proposal_commits = 0`;
the test's store fakes fail the run on any write or commit.

**The W3 transport is still not production-reachable**, as required. `weather_open_meteo_http_ops`,
`weather_runtime_fetch` and `weather_transport_validate` are garbage-collected out of the image, and the OTA
binary contains no `api.open-meteo`, no `open-meteo.com` and not even the substring `https://`.

---

## 7. Privacy

`NxWeatherPilotLine` has **no pointer and no character-array field**, so a string cannot be carried through it
even by mistake. The three non-test `weather_pilot_diag` sources contain exactly two `ESP_LOG` calls; the large
one has 46 format specifiers and 46 arguments matched by position and type, and every `%s` resolves to a total
literal-returning switch. No `printf`, `puts`, `fwrite`, `vprintf` or `esp_log_write` exists in any of the six
non-test files.

`PoolTimeSourceDiagnostics` — the model the new `time_*` fields project — is string-free and epoch-free.
Coordinates are compiled as integers and are **absent** from the OTA image as strings.

**Stated residual (§8.1).** The configured trusted-time host *is* present in the image as a compile-time string,
because SNTP must read it; the contract is that it never reaches a log line or a diagnostic field, and that
holds. Separately, `provider_cfg=1` and `tz_cfg=1` are bijective with the single supported provider and the
single supported timezone, so they disclose *which* provider and *which* timezone (country granularity) rather
than merely *that* one was chosen.

---

## 8. Findings from adversarial verification

Both are accuracy issues in material this gate itself wrote. Neither weakens a safety property. Neither is
fixed here, because this gate is verification-only.

### 8.1 The `provider_cfg` / `tz_cfg` booleans are not a reduction

`WeatherProviderId` and `WeatherTimezoneId` each have exactly two enumerators — an unset sentinel and the one
supported value — pinned by `_Static_assert`. The W5 validator rejects anything out of domain before a line can
report READY. Therefore `provider_configured = (provider != UNCONFIGURED)` is logically equivalent to
`provider == OPEN_METEO`, and the same for `EUROPE_BRUSSELS`. The booleans are honest about the build, and the
manifest contract already treats these as approved bounded enums, but the report should not claim they hide
*which* provider or timezone was selected. They do not. Coordinates and the NTP host remain unexpressible.

### 8.2 The committed header contract is stale, and the "live" claim is conditional

`components/weather_pilot_diag/include/nx_weather_pilot_diag.h:21-28`, added by this same commit, still asserts
in the present tense that the pilot "steps the W4 runtime with NO injected clock", that its state is "fixed at
boot", and that `trusted_time_available` is "a CONSTANT false, not a measurement". That was true of the
diagnostic half of W6.2 and was made **false** by the wiring half in the same commit. It should be corrected.

Relatedly, `CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS` is documented as **independent** of every
timed-session flag and does not depend on, select or imply `NX_TIMED_SESSIONS`. In that legal posture
`wx_bind_runtime()` is compiled out, the bind never succeeds, and `s_state`/`s_rec` retain their boot-notice
values for the whole boot. The pilot is not dishonest there — the projection correctly stamps
`time_fact=STRUCTURAL`, meaning "no provider can exist in this image" — but the live-refresh guarantee holds
**only** when the timed-session runtime is linked and bound, and should be stated with that qualifier.

### 8.3 Scope correction on "no live network in tests"

The C/QEMU suites and the Python tooling perform no network operation of any kind. The Angular spec suite is
different: it runs in a real browser against the local Karma server, and specs such as `logs.component.spec.ts`
subscribe to the root `WebsocketService`, which lazily opens `ws://<karma-origin>/api/ws`; `web-version.service.ts`
issues a real `GET /version.txt`. These are **loopback** connections to Karma's own server on `localhost:9876`,
not to the owner LAN, a pool, an NTP host or a weather provider. The no-network claim should be scoped to the
firmware and tooling suites rather than stated for the whole tree.

---

## 9. Validation environment notes

Three environment issues occurred during verification. None involved the committed tree.

1. **Docker Desktop was not running** when verification began (`com.docker.service` stopped, no processes).
   The Docker Desktop application was started so the container validations could run; no settings were changed.
2. **The first frontend run aborted** with a browser `ECONNRESET` while Docker was initialising concurrently.
   It was re-run cleanly to 1270/1270 rather than reported as a flaky pass.
3. **The firmware posture failed twice on the SPIFFS web image.** The first failure was
   `given base directory does not exist` — a previous cleanup had removed `main/http_server/axe-os/dist`, and
   with `GITHUB_ACTIONS=true` the ESP-IDF build consumes a prebuilt web UI rather than building one. The second
   was `SpiffsFullError` — a raw `ng build` produced a 3.0 MB image against the 3 MB partition. The repository's
   own `npm run build` produces **922 KB**, and the posture then built clean. Both are verification-harness
   errors; the committed build integration is correct, and the W6 helper classifies the first signature
   correctly as `spiffs-image`.

---

## 10. Physical-pilot readiness

**Unchanged from the previous gate: NOT READY**, and W6.2 deliberately does not change it.

What a flashed W6.2 artifact now does that it could not before: the WX summary's `state` and `trusted_time`
become live readings; the eight `time_*` fields expose the real B10 lifecycle, distinguishing unconfigured,
configured, syncing, trusted, rejected, timeout and error; and a device whose first provider start was refused
can spend attempts 2–5.

What it still does not do: no weather fetch, and no recommendation. Four blockers remain, each needing its own
gate, and all four are confirmed again here by the linker discarding the corresponding symbols:

1. **No asynchronous W3 execution context.** The only committed transport is synchronous and bounded per socket
   operation at 8000 ms; the only permitted host is the 1 s statistics task.
2. **No W2 persistence seam** in `WeatherRuntimeDeps`; `weather_runtime` references zero NVS/store APIs.
3. **No schedule-window field** in the committed `nx_wtp` record family.
4. **All three production profiles ship UNVALIDATED** and are never auto-selectable, so `RECOMMENDATION_READY`
   is unreachable on a production artifact regardless of transport.

---

## 11. Confirmation

No owner-private value was read, printed or altered — synthetic fixtures throughout, and only shape booleans
were reported. No hardware, COM/USB, LAN, NTP, weather-provider or pool contact. No artifact was built or
staged for flashing, and the real W3 transport was **not** wired. **No Git write** — HEAD remains
`a3c76f6` = `v2.14.2-82-ga3c76f6`, and this report is the only uncommitted file.
