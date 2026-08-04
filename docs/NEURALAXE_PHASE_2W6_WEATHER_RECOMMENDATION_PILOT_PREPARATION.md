# NeuralAxe OS — Phase 2W, Gate W6

## Recommendation-Only Weather Pilot: Artifact Contract, Rollback Readiness, Owner Checklist and Pilot Plan

**Product:** NeuralAxe OS 0.1.0-dev · **Target:** Bitaxe Gamma / board 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-weather-recommendation-pilot` · **Base:** `f4aee75` = v2.14.2-76 (the committed Gate W5 report)

This is the **preparation** document for a supervised, owner-executed physical pilot. It is not a verification report and it does not authorize a flash. Nothing here has been executed against hardware; no artifact has been built; no coordinates, provider or trusted-time source have been selected.

---

## 1. What the pilot does — and what it cannot do

A recommendation-only weather pilot obtains trusted time from the committed B2/B10 provider, evaluates the committed Brussels schedule, retrieves **one** forecast from the explicitly configured Gate W3 provider, evaluates the committed Gate W1 policy, and emits a **recommendation**. It changes nothing.

The safety boundary is structural, not procedural. A weather recommendation is an advisory value with no pointer and no command field, so it cannot reach a frequency, voltage, fan or thermal setter, a pool or protocol writer, a restart, an OTA, a timed-session creation, Restore Now, B5 mutating ownership or B7 execution. The pilot image contains **no timed-session command routes**, and Gate W6 diagnostics observe and report only: an invariant violation is logged and the device is left alone.

Weather is advisory in a second sense too. HTTPS retrieval proves the transport, not the truth of the forecast — so a valid response still authorizes nothing.

**Where the pilot runs.** Gate W6.1 adds a periodic observation, and it creates **no task, timer or queue** of its own: it is called from the already-existing statistics task, which is started only in the normal mining posture (system initialised, ASIC initialised, not self-test). Each call attempts the once-per-boot baseline capture, re-reads the authorities, runs the invariant monitor and emits at most one summary per 60 seconds. If that task does not exist the device is not in the pilot's scenario either, and no observation is emitted — which is reported by the absence of lines, never by a healthy-looking one.

---

## 2. Artifact package contract

A later package contains **exactly** five items and nothing else:

| Item | Purpose |
|---|---|
| `esp-miner.bin` | the pilot OTA application image |
| `www.bin` | the **freshly built** matching web image |
| `manifest.json` | bounded metadata only (§3) |
| `SHA256SUMS` | checksums for every file above |
| `OWNER_INSTRUCTIONS.md` | the checklist and pilot plan |

**Deliberately excluded:** merged image, factory image, bootloader, partition table, NVS dump, the private configuration fragment, coordinates, timezone, provider hostname, request URL, any provider response, credentials, pool identity.

The factory and merged images are excluded because the pilot must install through the **NVS-preserving web OTA route**. A merged or factory image would rewrite the whole flash, discarding the very configuration the pilot is meant to leave untouched.

## 3. Manifest schema

Bounded booleans, enums and identity only:

```
schemaVersion, gate=W6, kind=weather-recommendation-pilot,
revision (canonical describe), commit (full 40 chars), trackedTreeDigest,
weatherConfigured=true, providerConfigured=true, locationConfigured=true,
timezoneConfigured=true, trustedTimeConfigured=true,
distributionMode=OWNER_MANAGED_EXTERNAL, sourceStatus (value-free token),
pilotDiagnosticsEnabled=true, recommendationOnly=true,
executionEnabled=false, timedSessionApiEnabled=false,
storePreflightEnabled=false, hardwareTuningEnabled=false,
releasePairVerified=true, firmwareRevision, webRevision,
artifact sizes, SHA-256 per artifact
```

The helper enforces this with `assert_manifest_private_free()`, which fails the build closed if a coordinate, provider hostname, URL, timezone identifier, trusted-time source or environment-variable name ever reaches the manifest. A test proves the guard rejects a deliberately leaking manifest.

## 4. Canonical release-pair contract

The device compares `esp_app_desc_t.version` with the web image's `version.txt` by **exact string equality** and reports BOOT PAIR MISMATCH on any difference. The helper therefore builds the web image fresh in the same run, passes the identical canonical revision to both sides, and verifies `firmwareRevision == webRevision == canonicalGitDescribe` before packaging. A one-character difference fails the build; a test pins that behaviour.

---

## 5. Rollback readiness

Rollback uses the **committed Gate B10.2 verifier** (`tools/pilot/verify_rollback_readiness.py`); Gate W6 adds no rollback identity logic of its own and duplicates none.

**A weather-pilot artifact is NOT READY while rollback verification is not ready.** Before any flash the owner must hold:

1. the identity of the currently installed firmware, recorded;
2. a coherent current firmware/web pair (no pre-existing mismatch);
3. a known-good rollback OTA + www pair, as a matched set;
4. a factory recovery image available for serial recovery;
5. verified checksums for every artifact above;
6. working USB/serial recovery access;
7. confirmation that no merged image is used for the pilot;
8. the NVS-preserving web OTA route as the only install path.

---

## 6. Owner preflight checklist

Human-executed, before flashing. Every item is a confirmation, not an action.

| # | Confirm |
|---|---|
| 1 | Target is the Gamma 601 / BM1370 |
| 2 | Current firmware/web pair is coherent |
| 3 | Current pool and worker recorded **privately** |
| 4 | Frequency remains 625 MHz |
| 5 | Voltage remains 1150 mV |
| 6 | Fan configuration recorded |
| 7 | ASIC and VRM temperature baseline recorded |
| 8 | Hashrate, power and reject-rate baseline recorded |
| 9 | Timed-session store previously proven EMPTY or CLEARED |
| 10 | No timed-session owner exists |
| 11 | No terminal acknowledgement pending |
| 12 | Rollback pair verified (§5) |
| 13 | Serial/USB recovery available |
| 14 | NTP source explicitly supplied locally |
| 15 | Weather distribution and provider explicitly supplied locally |
| 16 | Private coordinates explicitly supplied locally |
| 17 | Timezone explicitly supplied locally |
| 18 | Manifest shows `executionEnabled=false`, `timedSessionApiEnabled=false` |
| 19 | Artifact hashes match `SHA256SUMS` |
| 20 | No unrelated tuning or hardware change occurs during the pilot |

Items 14–17 are supplied through environment variables at build time and never committed: `NX_PILOT_NTP_SERVER`, `NX_WEATHER_DISTRIBUTION`, `NX_WEATHER_PROVIDER`, `NX_WEATHER_LATITUDE`, `NX_WEATHER_LONGITUDE`, `NX_WEATHER_TIMEZONE`. The helper validates them, never prints them, writes them to one out-of-tree fragment and shreds it in a `finally` block.

---

## 7. Owner-executed pilot plan (P0–P7)

**Not executed. Prepared only.**

**P0 — baseline.** At least 20 minutes of normal mining. Record hashrate, ASIC temperature, VRM temperature, fan, power, rejects, uptime and pool stability, plus **ambient temperature** (without it, a later thermal comparison is meaningless). Alter no tuning.

**P1 — install.** Upload the matching www image, then the matching OTA image. Confirm an exact firmware/web BOOT PAIR MATCH, that pool, tuning and fan configuration are unchanged, and that mining resumes.

**P2 — trusted-time readiness.** Observe the existing trusted-time diagnostics. Require trusted time operational and exactly one SNTP provider. Mining remains normal.

**P3 — pre-window observation.** Verify `WX_WAIT_SCHEDULE`, zero weather fetches before the window, zero recommendations, and no `WX_INVARIANT_VIOLATION`.

**P4 — scheduled recommendation.** Observe the first valid window. Expect exactly one bounded fetch and one of `WX_RECOMMENDATION_READY`, `WX_NO_RECOMMENDATION`, `WX_FETCH_TIMEOUT`, `WX_FETCH_REJECTED` or `WX_FORECAST_STALE`. Every result is advisory and `executed=false` remains visible on every line.

**P5 — duplicate prevention.** Confirm no second fetch or recommendation in the same window, no log flood, and no repeated persistence write.

**P6 — controlled reboot.** Reboot once after a completed window. Confirm duplicate same-window evaluation is still prevented per the committed persistence/schedule semantics, mining returns, and tuning is unchanged.

**P7 — finish or roll back.** Export only sanitized diagnostic lines, verify no private configuration leaked, compare mining and thermal behaviour against P0, roll back on any abort condition, and do not enable hardware execution.

---

## 8. Success criteria

Exact firmware/web pair match · normal source mining resumes · tuning unchanged · trusted time operational · no second SNTP provider · no fetch before the schedule is due · at most one bounded fetch per window · an honest recommendation or a bounded passive failure · `executed=false` · no B5 owner · no B7 execution · no timed-session API · no hardware, pool, protocol or restart mutation · no invariant violation · no private configuration in logs or metadata · no material heap or stack deterioration · no new thermal or power anomaly **relative to the measured ambient baseline**.

The mutation criteria are now **provable rather than assumed**: the pilot baseline must reach `BASELINE_READY`, and from that point every summary must report `mut_hw=0 mut_pool=0 mut_proto=0 mut_restart=0 mut_ota=0 mut_session=0 tuning_same=1 hist_lost=0`. A run whose baseline never becomes READY has proven nothing about mutation and is **not** a successful pilot.

## 9. Abort criteria — roll back immediately

Pool identity changes · frequency changes · voltage changes · fan configuration changes unexpectedly · the timed-session API becomes available · a B5 owner appears · B7 execution appears · any hardware apply counter changes · a pool, protocol or restart counter changes · `WX_INVARIANT_VIOLATION` appears · a repeated fetch occurs inside one window · private coordinates, timezone, hostname or URL appear in logs · a reboot loop or crash occurs · mining cannot resume · the rollback path becomes unavailable.

Also abort on `hist_lost=1` (a counter regressed or saturated) and on `tuning_same=0`. Both mean the pilot can no longer prove what happened between the baseline and now — an ambiguity, not a small number.

**Not an abort:** a weather timeout, a rejected response, a stale forecast or a no-recommendation result, provided every invariant holds. Those are recorded as a **failed recommendation observation** — the pilot's job is to find out whether the chain works, and an honest negative is a valid outcome.

---

## 10. Diagnostic surface

Nineteen bounded tokens (`WX_PILOT_BOOT` … `WX_INVARIANT_VIOLATION`), edge-triggered so a multi-hour wait produces one line, plus a summary limited to once per 60 seconds on monotonic time. Sixteen invariant codes report the first violation in a fixed order.

Every line is machine tokens and scalars: event, sequence, source status, runtime state, the four configuration **booleans**, trusted-time/schedule/policy/recommendation flags, `executed=false`, freshness, provider result, not-executed reason, invariant code, fetch and recommendation counters, free and minimum-free internal heap, stack high-water and monotonic uptime.

Gate W6.1 adds the mutation evidence to the same line: whether mutation observability is compiled in, the baseline lifecycle token, the monotonic uptime at which the observation window opened, whether the boot reached that point with no counted configuration change, whether counter history was lost, the five class deltas since the baseline, the timed-session mutation count and whether the tuning fingerprint still matches. All bounded scalars and one token id.

No coordinate, city, timezone, hostname, URL, forecast body, NTP server, pool identity or session identifier can appear — the diagnostic line type has **no field capable of holding one**, and no character array at all.

---

## 11. AUDITED GAP — found in Gate W6, CLOSED in Gate W6.1

The invariant monitor may only report health from facts an **authority**
produced. Every field it consumes therefore carries an explicit provenance
(`ABSENT` / `OBSERVED` / `STRUCTURAL` / `UNAVAILABLE` / `STALE`), and the
checker evaluates provenance **before** values: a zeroed structure reports
`WX_INV_FACT_ABSENT` rather than passing on convenient zeros, and a required
field stamped `UNAVAILABLE` fails closed no matter how healthy its value looks.

Gate W6 audited the owning subsystems and found that six of the eleven required
facts had **no authority to read**. Gate W6 refused to invent counters inside
the weather component — a counter the observer increments itself is
self-reported evidence, not authority — and returned **NOT READY**.

**Gate W6.1 closes it at the owning boundaries.** The table below is the state
after W6.1.

| Field | Authority | State |
|---|---|---|
| B7 execution availability / action | absence of `CONFIG_NX_TIMED_SESSIONS_EXECUTION` — the symbols are not linked | **STRUCTURAL** |
| Timed-session command API | absence of `CONFIG_NX_TIMED_SESSIONS_API` — no route is registered | **STRUCTURAL** |
| Free heap, minimum free heap, stack high-water, monotonic uptime | ESP-IDF `heap_caps_*`, `uxTaskGetStackHighWaterMark`, `esp_timer_get_time` | **OBSERVED** |
| B5 current owner | `pool_session_runtime_snapshot()` — the published B6 runtime view carries the B5 lease owner | **OBSERVED** (STRUCTURAL when the feature is not linked) |
| Timed-session mutations | the same snapshot's committed proposal count | **OBSERVED** (STRUCTURAL when not linked) |
| Source mining still permitted | the same snapshot's boot mining policy and protocol permission | **OBSERVED** (STRUCTURAL when not linked) |
| Frequency / voltage / fan / thermal mutation counters | `nvs_config_set_*`, the single persistent-configuration writer, incremented after its existing no-op guard | **OBSERVED** |
| Pool / protocol mutation counters | the same writer, classified per key | **OBSERVED** |
| Restart / OTA counters | ALL eight `esp_restart` request sites in the image — the two HTTP routes, the two BAP handlers, the two self-test paths and the two protocol-retry paths — plus the two OTA acceptance points | **OBSERVED** |
| Tuning snapshot vs pilot baseline | a bounded fingerprint of the CONFIGURED tuning, compared against the once-per-boot RAM baseline | **OBSERVED** |

Two properties make this evidence rather than reassurance:

- **The observer never counts.** Every increment lives inside the subsystem
  that performs the mutation. The weather component only reads a coherent
  snapshot; it has no way to increment anything.
- **Absence never reads as calm.** With `CONFIG_NX_MUTATION_OBSERVABILITY`
  absent the snapshot is INVALID, not nine zeroes, and the facts are stamped
  `UNAVAILABLE`. Before the baseline exists they are stamped `ABSENT`. A
  counter that regressed or saturated is reported as lost history, not as a
  small delta. In every one of those states the pilot is unhealthy.

### What the counters do and do not say

A counter proves **that** a class of configuration changed. It attributes
nothing. The overheat-protection path writes a permanently derated frequency,
voltage, fan mode and overheat flag; that is a genuine configuration mutation
and it is counted, so the pilot fails loudly and the owner investigates. The
invariant being proven is therefore **"nothing changed at all"** — the
stronger claim, and the only one these counters can support.

Normal runtime activity is deliberately not counted, and cannot be: fan PWM,
the boot frequency ramp, the initial voltage application, pool reconnects on
unchanged configuration, protocol transitions, share submission and
trusted-time synchronisation never reach the persistent-configuration writer.
Loading the configuration cache at boot reads NVS directly and bypasses the
instrumented writer entirely.

### The observation window is explicit

The baseline is captured once per boot, after the device has been up for 120
seconds, and is never silently replaced. The pilot claims nothing about the
interval before `base_us`; the diagnostic line reports that uptime and whether
the boot reached it with no counted change. The baseline is RAM-only and never
survives a reboot, because after a restart the counters restart at zero and a
carried-over baseline would compare against an unrelated history.

---
## 12. Limits of this preparation

- **No artifact exists.** No pilot image has been built, and none may be built without all explicit owner-supplied private values.
- **No values selected.** No coordinates, provider, distribution mode or trusted-time source have been chosen, recommended or embedded. Repository defaults remain weather disabled, provider unconfigured, location unconfigured, timezone unconfigured, no NTP source.
- **No hardware contact.** Nothing here has touched a device, a network, a pool, DNS, NTP or a weather service.
- **The physical pilot is not authorized by this document.** It becomes executable only when the owner supplies the private values, builds the artifact, completes the §6 checklist and confirms §5 rollback readiness.

---

## 13. Verdicts

**Software: READY.** Every required pilot invariant now has an authoritative
live source at the boundary that owns it, and no required fact is a hardcoded
value, a configuration-derived assumption or a caller default. Absence,
unavailability, a missing baseline and lost counter history are each reported
as their own distinct unhealthy state.

**Artifact: NOT BUILT.** No pilot image exists. Building one requires private
values the owner has not supplied and this work has not chosen.

**Physical pilot: NOT READY.** It stays not ready until the owner supplies the
private values, builds the artifact, completes the §6 checklist and confirms
§5 rollback readiness — including the offline rollback verification and the
read-only `nx_tps` store inspection. Software readiness is a precondition for
the physical pilot, never a substitute for those steps.
