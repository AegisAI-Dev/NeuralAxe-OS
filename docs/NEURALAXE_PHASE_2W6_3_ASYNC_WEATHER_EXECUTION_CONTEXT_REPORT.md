# NeuralAxe OS Phase 2W — Gate W6.3 (family)
## Asynchronous Weather Execution Context — Closure Report

Phase: Weather-Aware Tuning, Gates W6.3 / W6.3T-A / W6.3T-B / W6.3.1 / W6.3.2 · Branch: `neuralaxe-v0.1-weather-recommendation-pilot`
Verified closure HEAD: **fb2728d** ("feat: add explicit weather pilot schedule authorization") = **v2.14.2-89-gfb2728d**
Verification date: 2026-08-21 · Working tree clean · This report is the only uncommitted file (owner report commit gate).

---

## 1. Verdict

**Gate W6.3 committed-state closure verification: PASS. The W6.3 family is CLOSED as a software gate.**

**Physical recommendation pilot: NOT READY.** No hardware validation has been performed, and two
substantive gaps remain open by design (§11).

The family's single achievement, stated precisely: a NeuralAxe device built as an explicit,
owner-authorized recommendation pilot can now run the complete weather chain end to end — from an
explicit schedule authorization through trusted time, the committed Brussels schedule, one bounded
asynchronous fetch, one coherent telemetry snapshot and the W1 input projection — and arrive at
`tuning_policy_evaluate()` on real inputs. It arrives there and **changes nothing**: the result is a
bounded recommendation with `actionable=false` and `executed=false`, over a registry in which every
profile is `TUNING_VALIDATION_UNVALIDATED`.

Before this family the chain was not merely unused; it was **structurally unreachable** at three
separate points, each of which had to be removed in its own right.

---

## 2. Implementation lineage

Recovered from Git, not from a task description. Five commits, linear, **zero merges** in
`df46ce2^..HEAD`; every commit verified an ancestor of `HEAD` with `git merge-base --is-ancestor`.

| Commit | describe | Date | Subject | Files | Diff |
|---|---|---|---|---|---|
| `df46ce2` | v2.14.2-85 | 2026-08-12 | feat: add bounded asynchronous weather I/O worker | 16 | +3,131 / −52 |
| `e23440f` | v2.14.2-86 | 2026-08-12 | feat: add authoritative fan and VRM telemetry health | 5 | +246 / −0 |
| `5143fc1` | v2.14.2-87 | 2026-08-12 | feat: add coherent telemetry safety snapshot | 9 | +831 / −1 |
| `792a338` | v2.14.2-88 | 2026-08-21 | feat: wire production W1 policy input projection | 14 | +1,730 / −11 |
| `fb2728d` | v2.14.2-89 | 2026-08-21 | feat: add explicit weather pilot schedule authorization | 10 | +901 / −20 |

Full SHA of the W6.3.1 anchor: `792a3387eff844e3ccb2f90ab411428d1895c8f0`.

Components introduced: `components/weather_io/` (W6.3), `components/telemetry_safety/` (W6.3T-B),
`components/tuning_input_projection/` (W6.3.1). W6.3T-A added no component — it added the missing
classifiers to the committed W1 policy module. W6.3.2 added no component and no dependency: it is one
Kconfig symbol, one production assignment, one diagnostic field and one test file.

---

## 3. What each gate closed

### 3.1 W6.3 — bounded asynchronous W3 execution context (`df46ce2`)

The committed Gate W3 transport is **synchronous**, and its `WEATHER_HTTP_TIMEOUT_MS = 8000` is an
`esp_http_client` *socket* timeout applying to each blocking operation — connect/TLS, header fetch,
every body read — not to the call as a whole. One fetch can therefore occupy tens of seconds. The only
host the pilot offers is the ~1 s statistics task.

W6.3 supplied the missing execution context and nothing else:

- **One worker.** `components/weather_io/nx_weather_io_worker.c:240` holds the single
  `xTaskCreateStatic` in the entire weather path, guarded module-wide by `s_worker_count` under a
  spinlock, with a static TCB and a static stack. Repeated start calls cannot duplicate it.
- **The statistics task cannot execute blocking HTTP.** The runtime's own transport seam is held
  `NULL` (`nx_weather_pilot_diag_log.c:184`), which makes `weather_runtime_fetch()` — the synchronous
  entry point reachable from that task — structurally incapable of a network call. The production
  transport is wired only into the worker (`:207`).
- **The existing W3 transport is reused.** `components/weather_client/weather_open_meteo_http.c` is
  the sole `esp_http_client` user in the tree; no second transport was written.
- **One physical in-flight fetch.** One request slot, one result slot, one in-flight generation.
- **Stale generations and timeouts.** The 120 s deadline (`NX_WX_IO_DEADLINE_US`) is enforced by the
  *consumer*, so a wedged worker cannot latch the pilot busy; a late publish for a superseded
  generation is discarded and counted, never applied.
- **No synchronous fallback.** There is no path that falls back to a blocking fetch on the host task.

### 3.2 W6.3T-A — authoritative fan and VRM telemetry health (`e23440f`)

`TUNING_SENSOR_OK` is the **zero** of the W1 sensor enum, so a zero-initialised `TuningSensorHealth`
silently asserts healthy hardware. W6.3.1 could not honestly be built until two facts had an authority:

- **Fan tach.** `tuning_classify_fan_tach()` invents no RPM threshold. It restates two
  already-committed contracts: the acquisition layer returns 0 for "no tach signal" (failed register
  read, the `0xFFFF` idle encoding, and a stopped fan alike), and W1 already defines
  `TUNING_SENSOR_INVALID` as "read error / −1 sentinel / ≤ 0". A board declaring no fan yields
  `MISSING` — reported *before* the error and value checks, so a non-existent fan cannot be turned
  into a measurement. The all-zero argument set returns `MISSING`, never `OK`.
- **TPS546 read validity.** `TPS546_get_temperature()` returns a **cached last value** on SMBus
  failure. `tuning_classify_vrm_temp_dc()` therefore treats the successful acquisition itself as the
  freshness authority: a read error returns `INVALID` regardless of how plausible the cached number
  is. A perfectly in-band 61.0 °C carried over from a failed acquisition is `INVALID`, not healthy.
- **No repeated-value staleness heuristic.** The classifier's unchanged-streak and stale-streak-limit
  parameters exist, and the projection passes `0, 0` — deliberately disabling them. Freshness is
  decided by the acquisition result, never by guessing from a frozen reading.

### 3.3 W6.3T-B — coherent telemetry safety snapshot (`5143fc1`)

The safety-relevant telemetry is written by two independent tasks with no shared synchronisation
(`power_management_task`, `fan_controller_task`). A consumer reading field by field can observe a set
that never existed as a whole. A safety decision assembled from a torn read is not defensible.

- **One bounded publication per producer, one bounded read.** Each producer publishes its own facts as
  a single update under one spinlock; a reader takes one struct copy and holds no lock afterwards.
- **Generations are the commit point.** The generation counter advances **last**, after every field is
  stored, so a reader that observes generation N has observed all of publication N. Generation `0`
  means "never published" — the load-bearing property, because it prevents an unpublished fact from
  being mistaken for a confirmed-healthy one.
- **Saturating, never wrapping.** A counter that reaches `NX_TELEMETRY_GENERATION_MAX` stops there, so
  a stale snapshot cannot impersonate a fresh one and a long-running device cannot re-enter
  "never published".
- **Facts, not policy.** The type holds no profile, recommendation, actionable flag, mining policy,
  weather state, persistence or wall clock. It samples no hardware and decides nothing.
- **What is deliberately NOT claimed.** Equal generations are a coincidence of counting, **not**
  evidence that the ASIC, VRM and fan were sampled at the same instant — the two producers run on
  independent ~100 ms cycles. The snapshot removes torn reads; it does not manufacture a common clock.

### 3.4 W6.3.1 — production W1 policy input projection (`792a338`)

Until this commit the production pilot stepped the W4 runtime with `env = NULL`, so
`tuning_policy_evaluate()` could not run however healthy the device was.

- **Exactly one snapshot read per evaluation.** `nx_weather_pilot_diag_log.c:838` is the sole
  production reader; the projection receives a `const` pointer and never re-reads the store,
  `PowerManagementModule`, `SystemModule`, `DeviceConfig` or the TPS546 globals.
- **Canonical declared identity.** `TUNING_BOARD_GAMMA_601` / `TUNING_ASIC_BM1370` come from the
  declared build target, never from a runtime board string, and are chosen in exactly one place.
- **The real registry is linked.** Production passes `tuning_registry_gamma601()`. Supplying a
  registry is candidate *selection*, never authorization: `tuning_profile_auto_eligible()`
  independently requires `VALIDATED`.
- **Honest fail-closed sentinels.** Installation facts are `TUNING_COOLING_UNSPECIFIED` /
  `TUNING_PSU_UNSPECIFIED` — the firmware never guesses which cooler or PSU the owner fitted. Mining
  health is `TUNING_MINING_UNKNOWN` (no classifier exists, and UNKNOWN fails closed for upgrades).
  The current profile is unknown and is never inferred from a running frequency or voltage. Roles are
  the committed "not configured" empty sentinels. No field is silently safe because it is zero.
- **Fail-closed refusal.** A partial or absent snapshot is refused outright rather than handed to W1
  with placeholders, so "not yet observable" never masquerades as "observed and safe".
- **The evaluator is genuinely reached.** `weather_runtime.c:577` is the sole production call site, and
  `weather_runtime_step()` copies the projected input and overwrites only the three weather-owned
  fields (climate request, trusted-time validity, current epoch).

### 3.5 W6.3.2 — explicit schedule authorization (`fb2728d`) — owner-ratified

W6.3.1 wired the environment, but the chain still could not start. The committed W3 schedule ships
disabled, with exactly one non-test writer (`local_schedule.c:15`, inside
`weather_schedule_config_defaults()`), and **no Kconfig symbol and no production assignment** could set
it true. `weather_schedule_evaluate()` short-circuits to `WEATHER_SCHEDULE_DISABLED`
(`local_schedule.c:167`) *before* the trusted-time check and the slot loop, so DUE was unreachable, the
worker was never fed, and the step always returned at its no-observation guard.

`CONFIG_NX_WEATHER_PILOT_SCHEDULE` closes exactly that gap:

- `default n`, `depends on NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS`, itself inside
  `if NX_WEATHER_SOURCE_POLICY` — so it cannot exist in a baseline image.
- **Exactly one production assignment**, at
  `components/weather_source_policy/nx_weather_source_build.c:90`, double-gated behind both guards.
- **Only the enable flag moves.** Slot count, the three committed local slots (05:00 / 11:00 / 15:00)
  and the catch-up window keep the values `weather_schedule_config_defaults()` gave them. This
  authorizes the committed Brussels schedule; it does not redefine it.
- **No second scheduler**, timer, task or queue; **no runtime toggle**, REST route, NVS setting or
  remote command. The only code reference to the symbol in the tree is that single guard.

**The owner has explicitly ratified the capability name `CONFIG_NX_WEATHER_PILOT_SCHEDULE`.** It is
final and is not to be renamed.

---

## 4. Load-bearing architectural properties

**1. Weather configuration is not schedule authorization.** A device with distribution, provider,
location and timezone configured reports `WX_SRC_READY_RECOMMENDATION_ONLY` — and still asks for
nothing. Readiness says *where* weather would come from; authorization is the separate answer to
*whether this device may go and ask*. Test A proves this at a genuinely due instant, with trusted time
and a usable projection present: the only missing element is the authorization, and the result is zero
submissions and zero fetches.

**2. Schedule authorization is not hardware execution authorization.** The capability authorizes a
weather **observation** and nothing else. It grants no profile application, no frequency, voltage, fan
or thermal change, no pool or protocol change, no lease, no timed-session execution, no restart and no
OTA. Those surfaces are absent from the pilot image by ELF audit (§6.4).

**3. The statistics task remains non-blocking.** The synchronous W3 HTTP/TLS implementation runs only
on the dedicated W6.3 worker. The runtime transport seam stays `NULL` in the adapter.

**4. Physical concurrency remains one.** The deadline is a *logical* release performed by the consumer;
it does not signal, cancel or restart the worker, and it therefore cannot create a second simultaneous
physical W3 execution. A late publish from the released generation is discarded on its generation.

**5. Trusted time remains unique.** Weather **borrows** the B2/B10 clock and trust policy through
`pool_session_runtime_clock()` and `pool_session_runtime_trust_policy()`. SNTP is implemented only in
`components/pool_time/pool_time_sntp.c`. No weather component constructs a clock, starts a provider,
writes an anchor or establishes an accepted-epoch floor. Authorization does not outrank trusted time:
with the schedule authorized and no anchor, the runtime refuses at `WAITING_FOR_TRUSTED_TIME` before
the schedule is consulted at all.

**6. Telemetry authority remains outside weather.** Weather consumes one coherent snapshot published by
the two owning producer tasks. It performs no I2C/SMBus transaction and acquires no hardware telemetry
itself.

**7. UNVALIDATED means non-actionable.** All three Gamma 601 profiles remain
`TUNING_VALIDATION_UNVALIDATED`, payload-free and evidence-free, and none can become auto-eligible. No
validation evidence, payload or fingerprint is fabricated anywhere in the family.

---

## 5. The production chain, now reachable

```
explicit pilot schedule authorization   nx_weather_source_build.c:90 (CONFIG_NX_WEATHER_PILOT_SCHEDULE)
  -> trusted B2/B10 time                weather_runtime.c:458  (refuses first; borrowed clock)
    -> Brussels schedule DUE            weather_runtime.c:488 -> local_schedule.c:147
      -> ONE async W3 submission        nx_weather_io_worker_submit (one request slot)
        -> existing W3 transport        weather_open_meteo_http.c (sole esp_http_client user)
          -> WeatherObservation         nx_weather_io_worker_consume
            -> ONE telemetry snapshot   nx_weather_pilot_diag_log.c:838
              -> W1 input projection    nx_weather_pilot_diag_log.c:839
                -> tuning_policy_evaluate()          weather_runtime.c:577
                  -> safe recommendation / no-profile result
                     actionable = false
                     executed   = false
                     zero authoritative mutations
```

Every link above has **exactly one** call site in non-test code (comments excluded), verified by
source audit on the committed tree.

The two refusal postures are equally load-bearing:

- **Authorization OFF, time genuinely DUE** — plan `WEATHER_SCHEDULE_DISABLED`, reason
  `WEATHER_SCHED_REASON_DISABLED`, slot index −1; zero submissions, zero fetches, no recommendation,
  `executed = false`.
- **Authorization ON, outside every window** — plan `WEATHER_SCHEDULE_NOT_DUE` (13:00 local is two
  hours past the 11:00 slot and catch-up is disabled by the committed default); zero submissions, zero
  fetches. Authorization is not a licence to fetch whenever it likes.

---

## 6. Validation evidence — committed state

All figures below are from the final closure run against the clean committed tree at `fb2728d`, in a
fresh workspace. Builds used `espressif/idf:v5.5.3`; QEMU used the `nx-qemu-action` image for
emulation only. No earlier or partial log was used as evidence.

### 6.1 Test suites

| Suite | Result |
|---|---|
| Baseline QEMU (B1–B10.2) | **851 Tests · 0 Failures · 0 Ignored** |
| Weather QEMU (W1–W6.3.2) | **476 Tests · 0 Failures · 0 Ignored** |
| FAIL lines, either image | **0** |
| In-test panics, either image | **0** |
| W6 pilot helper suite | **93 / 93 OK — including the clean-tree guard** |

The single `abort()` in each log lies **after** the Unity summary (baseline line 2901 vs summary 2898;
weather 1066 vs 1063) and is the deliberate end-of-run halt, not a fault.

The clean-tree guard (`test_the_tracked_tree_is_unchanged_outside_the_pilot_tooling`) can only pass on
a committed tree; its green result is itself part of the closure evidence.

Weather suite composition (committed `TEST_CASE` counts): `test_nx_weather_io.c` 30 ·
`test_nx_weather_io_stress.c` 3 · `test_nx_telemetry_safety.c` 13 · `test_nx_tuning_input.c` 16 ·
`test_nx_weather_pilot_w631_flow.c` 6 · `test_nx_weather_pilot_w632_schedule.c` 7 ·
`test_nx_weather_pilot_diag.c` 31 · `test_nx_weather_pilot_time.c` 21 ·
`test_nx_weather_pilot_wiring.c` 15.

### 6.2 Compile and build

- **Strict compile: 7 / 7 clean, 0 failures** — host `gcc`, `-std=gnu11 -Wall -Wextra -Werror` plus
  `-Wshadow -Wpointer-arith -Wcast-qual -Wmissing-prototypes -Wstrict-prototypes -Wundef
  -Wsign-compare -Wvla`, over the pure translation units of the family plus header self-containment
  for `nx_tuning_input.h` and `nx_weather_pilot_diag.h`.
  (`-Wswitch-enum` is deliberately excluded: the committed token maps use a default arm rather than
  listing the count enumerator, so enabling it would fail committed code and prove nothing about this
  family.)
- **Baseline firmware posture (A): `Project build complete`**, exit 0. Application image 1,658,720 B.
- **Pilot firmware posture (P): `Project build complete`**, exit 0. Application image 1,737,360 B,
  ELF 19,265,924 B.

### 6.3 Posture proof, from the generated headers

| | Posture A (baseline) | Posture P (pilot) |
|---|---|---|
| `CONFIG_NX_*` defines in `sdkconfig.h` | **0** | 14 |
| `CONFIG_NX_WEATHER_PILOT_SCHEDULE` | **absent** | defined as 1 |

The tracked `sdkconfig.defaults` and `sdkconfig.ci` never mention the capability, so a normal build
cannot acquire it. The pilot helper verifies the capability from the **generated** `sdkconfig.h`, not
from its own fragment — the fragment states intent, only the header proves the compiler agreed.

### 6.4 Real ELF required / forbidden audit

| Check | Result |
|---|---|
| Required in posture P | **20 / 20 present** |
| Forbidden in every posture (never written: apply/execute entry points) | **8 / 8 absent** |
| Forbidden in posture A (family carries no code into the default image) | **6 / 6 absent** |
| Forbidden in posture P (`pool_session_execution_`, `pool_session_executor_`, `nx_pool_session_api_`, `nx_tps_preflight_`, `tuning_store_`) | **5 / 5 absent** |
| Audit failures | **0** |

Symbol counts: posture A 10,150 · posture P 10,599.

**Exactly one trusted-time / SNTP authority:** `pool_time` SNTP entry points in P = **2**
(`pool_time_sntp_init`, `pool_time_sntp_start`); other NeuralAxe SNTP symbols = **0**.
**Exactly one weather transport:** `weather_open_meteo_http_ops`, backed by a single static
`WeatherTransportOps`. **Worker singleton symbol set** present and complete
(start / submit / consume / tick / observe / count / stack high-water).

### 6.5 Structural audits

- **Worker singleton and boundedness.** One `xTaskCreateStatic` in the whole weather path
  (`nx_weather_io_worker.c:240`), guarded by `s_worker_count` under a spinlock, static TCB and static
  stack; one request slot, one result slot, one in-flight generation; consumer-enforced 120 s deadline;
  saturating counters that never wrap. Verified green by the committed worker and stress suites inside
  the 476/0/0 run.
- **Telemetry snapshot coherence.** One store, **one** production reader
  (`nx_weather_pilot_diag_log.c:838`), two publishers — one per owning producer task. The generation
  advances last under the lock.
- **W1 production reachability.** One `tuning_policy_evaluate()` call site (`weather_runtime.c:577`),
  one `nx_tuning_input_project()` call site (`nx_weather_pilot_diag_log.c:839`), one
  `weather_schedule_evaluate()` call site (`weather_runtime.c:488`), one schedule-enable assignment
  (`nx_weather_source_build.c:90`).
- **Schedule OFF / ON-not-due / ON-due.** All three postures pass, plus the trusted-time-precedence
  case and the pilot-line separation case — 7 W6.3.2 cases, 0 failures.

### 6.6 Mutation invariants

The W6.1 authoritative mutation model is intact. Across **all four** W6.3.2 postures (OFF-at-due,
ON-not-due, ON-due, and authorized-without-trusted-time) and the W6.3.1 end-to-end flow,
`nx_mutation_delta_compute()` followed by `nx_mutation_delta_clean()` over the full nine-counter set
returns clean:

```
mut_hw      = 0        (frequency + voltage + fan + thermal)
mut_pool    = 0
mut_proto   = 0
mut_restart = 0
mut_ota     = 0        (firmware + web)
executed    = 0
```

`out->executed = false` is a hard constant at `nx_weather_pilot_diag.c:453`; no production code
anywhere sets it true.

**Scope note, stated precisely.** `NxMutationDelta` covers the nine configuration-mutation counters
above. `mut_session = 0` and `tuning_same = 1` are pilot-line facts gathered from the B5/B6 posture and
the tuning snapshot rather than from that delta; they are supported here by the absence of every
session and execution surface from posture P (§6.4), by the committed W6.2 wiring test's session
invariants, and by the fact that the family adds no tuning writer and no counter call site.
**No hardware execution was tested, because none exists to test.**

### 6.7 Privacy, network and persistence

- **Privacy.** Diff-scoped scan over the added lines of the family: no coordinate, hostname, URL, query
  string, raw timezone, credential or owner path. `NxTuningInputDiag` and `NxWeatherIoDiag` contain no
  pointer and no character array, so a private value is not expressible through them. The pilot line
  and the `WX_W1_INPUT` line print bounded enum tokens and counters only.
- **Live network.** Exactly one file calls `esp_http_client_*`
  (`components/weather_client/weather_open_meteo_http.c`), reachable only from the worker. Every test
  in the family uses a fake transport and a synthetic clock; nothing contacts DNS, NTP, a weather
  provider, a pool or OTA.
- **Persistence.** Zero NVS writes anywhere in the weather path. The family opens no namespace and
  changes no schema. The runtime's store-write-request field is an in-RAM counter, not a write.

---

## 7. Verification posture

Offline throughout. Builds in `espressif/idf:v5.5.3`; QEMU emulation from the `nx-qemu-action` image
only. Firmware postures were built as **synthetic symbol-audit images**: the prebuilt-web branch of
`main/CMakeLists.txt` was taken (the container has no npm and installing it would require the network),
and a placeholder web `dist` was staged outside the tracked tree and removed afterwards. The web
partition of those images is therefore a placeholder — they are valid for **symbol reachability only**
and are **not** release artifacts. No artifact was flashed.

No Git write operation was performed at any point: `HEAD` and the reflog are unchanged, and nothing was
staged, stashed or discarded.

---

## 8. ELF audit process note

During the closure run an ELF audit was launched while the pilot image was still being linked. The path
`esp-miner.elf` already existed but the file was **0 bytes**, so `nm` reported 0 symbols and the audit
printed 0/20 required present and 20 failures.

**This was a validation-harness race, not a firmware failure.** Nothing about the committed tree was
wrong. The authoritative audit was re-run only after the build reported `Project build complete` and
the ELF was present at its full 19,265,924 bytes; that run produced the green result recorded in §6.4,
and it is the only ELF result this report relies on.

Lesson recorded for future gates: **artifact existence alone is insufficient evidence that an artifact
may be audited; artifact completeness must be verified first** — gate on the build's own completion
signal *and* a plausible size, never on the presence of a path.

The out-of-tree verification script used for this run was hardened accordingly. **No repository tooling
change was committed for this** — the working tree is clean and `fb2728d` contains no such change; the
hardening lives only in the ephemeral verification workspace.

---

## 9. Safety invariants (summary)

| Invariant | Status |
|---|---|
| Baseline image contains no W6.3-family code | Proven — 6/6 forbidden symbols absent from posture A; 0 `CONFIG_NX_*` defines |
| Schedule disabled unless explicitly authorized | Proven — one double-gated assignment; tracked defaults silent |
| Configured is not authorized | Proven — Test A: READY source, due instant, 0 submissions |
| Authorized is not due | Proven — Test B: 0 submissions outside the window |
| Authorization is not execution | Proven — 5/5 execution surfaces absent from posture P |
| Trusted time cannot be substituted | Proven — untrusted anchor refuses before the schedule |
| One SNTP authority | Proven — `pool_time` only; 0 other NeuralAxe SNTP symbols |
| One scheduler, one worker, one transport, one snapshot reader, one evaluator | Proven — single call site each |
| All Gamma 601 profiles UNVALIDATED and non-eligible | Proven — asserted per profile in the flow tests |
| `executed = false` | Proven — hard constant; no production writer |
| Zero authoritative mutations | Proven — clean delta across all four postures |

---

## 10. Privacy posture

No coordinate, provider hostname, request URL, query string, forecast body, NTP source, pool identity,
session identifier or credential can appear in any diagnostic this family adds — not because the code
avoids printing them, but because the line types have no field capable of holding one. Private values
exist only in the owner's environment, in one shredded out-of-tree fragment during a pilot build, and
in device RAM when explicitly configured. The pilot manifest carries bounded booleans and value-free
tokens only, including the new `scheduleEnabled`.

---

## 11. Known limits — physical pilot is NOT READY

1. **Reboot / window deduplication is unresolved (owned by W6.4).** The served-window mask
   (`WeatherScheduleProgress`) is **RAM-only**. A reboot inside an already-served window therefore
   allows that window to be served again. This is stated in the committed Kconfig help text and is a
   deliberate scope boundary, not an oversight.
2. **All Gamma 601 profiles remain UNVALIDATED (owned by W6.5).** No recommendation can ever be
   actionable today. The pilot observes and reports; it does not propose an applicable change.
3. **No physical hardware validation has occurred.** No Gamma 601 board, pool, live weather provider,
   real NTP server, LAN or OTA path was contacted at any point in this family. The worker's 12,288-byte
   stack remains a conservative estimate, not a measured value — a supervised pilot must read the
   reported high-water mark before that size is treated as proven.
4. **No release artifact exists.** The images built for this closure are synthetic symbol-audit builds
   with a placeholder web partition and no valid release pair.

---

## 12. Next gates

**W6.4 — Reboot / Window Deduplication.** Owns: persisted window identity and completion state;
crash-safe semantics; reboot duplicate prevention; bounded write behaviour; `nx_wtp` integration and
schema changes if required. Not started.

**W6.5 — Gamma 601 Profile Validation.** Owns: physical profile-validation evidence; hardware, cooling
and PSU qualification; evidence fingerprint and governed validation state; any future transition toward
actionable recommendations. Not started.

Neither is begun by this report.

---

## 13. Confirmation

- Gate W6.3 family (W6.3, W6.3T-A, W6.3T-B, W6.3.1, W6.3.2) — **software closure: CLOSED**.
- Physical recommendation pilot — **NOT READY**.
- Verified closure HEAD **fb2728d** = **v2.14.2-89-gfb2728d**, working tree clean, lineage linear and
  fully ancestral.
- **`CONFIG_NX_WEATHER_PILOT_SCHEDULE` is owner-ratified.** The name is final and is not to be renamed.
- No firmware source, test or build helper was modified in the production of this report.
- No Git write operation, no hardware access, no COM/USB, no owner LAN, no live weather provider, no
  NTP server, no pool and no OTA path was contacted. No owner-private value was read or printed.
- This report is the only uncommitted file (owner report commit gate).
