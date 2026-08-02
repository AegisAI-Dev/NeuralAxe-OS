# NeuralAxe OS — Phase 2M.1B, Gate B10.1
## Trusted-Time Observation Pilot: Preparation, Instrumentation and Owner-Executed Plan

**Scope:** NeuralAxe OS 0.1.0-dev, Bitaxe Gamma / board 601 / BM1370 / ESP32-S3 N16R8 only.
**Base:** committed Gate B10 (`d256deb`) and its report; branch
`neuralaxe-v0.1-timed-pool-sessions-time-pilot`.
**Nature:** software preparation only. **No physical pilot has been performed.** No hardware,
serial port, USB device, LAN, DNS resolver, NTP server, mining pool or OTA endpoint was contacted
while preparing this gate.

---

## 1. What this gate is, and what it is not

Gate B10 wired the trusted-time (SNTP) provider and added an observation-only mode. It ended with a
verdict of *ready for a separate observation pilot*, conditional on three things the owner controls.
This gate makes that pilot **executable**: it audits what a real device actually reveals, adds the
bounded diagnostics that were missing, gives the owner a reproducible way to build a pilot image
without ever putting a trusted-time hostname in Git, and writes the flash / rollback / pre-flight /
staged-observation plan the owner will follow.

It is **not** an execution gate. The pilot artifact this gate prepares:

- **cannot create a timed session** — the Gate B8 API is compiled out, so no route, no command
  mailbox and no session-creation path exists;
- **cannot apply a target pool configuration or restore a source** — the Gate B7 executor is
  compiled out, so no controlled apply, no protocol stop/start, no verification connection and no
  restore exist;
- **cannot grant target mining** — the ASIC gate override lives entirely in Gate B7;
- **cannot take a mutating Gate B5 lease, write a Gate B3 record or commit a heartbeat** on the
  empty store it runs against;
- **cannot call `esp_restart()`** from anything this gate added;
- **changes no pool, protocol, TLS, account, worker, password, frequency, voltage, fan or thermal
  setting.**

Everything added here observes and reports. A detected violation produces a log line and nothing
else: during an observation-only pilot, the honest response to an unexpected fact is to tell the
owner, not to intervene automatically.

---

## 2. Pre-edit observability audit

### 2.1 Method

Read the committed Gate B10 implementation and report in full, then traced every fact a supervised
pilot needs back to something a real device can actually show through: bounded serial logs, the
already-published sanitized snapshots, the existing dashboard/status API, and standard ESP-IDF
runtime diagnostics.

### 2.2 What the committed build already reveals

| Fact | Already visible? | Through what |
|---|---|---|
| Runtime state, protocol permission, store outcome, lease phase, time state | **yes** | one bounded `ESP_LOGI` per publish in `runtime_publish` (`state=… protocol=… status=… decision=… store=… phase=… time=…`) |
| Provider started, observation vs recovery, attempt index, bounded wait | **yes** | `"trusted-time provider started (%s, attempt %u, bounded wait %us)"` |
| Attempt budget exhausted | **yes** | `"trusted-time start budget exhausted after %u attempts"` |
| Provider init / start rejected | **yes** | two bounded `ESP_LOGW` lines carrying only a `PoolTimeError` token |
| Owner task started / stopping | **yes** | two bounded `ESP_LOGI` lines |
| Sanitized trusted-time diagnostics model | **built, but internal** | `pool_session_runtime_time_diagnostics()` — deliberately **not** on the public API (Gate B10 §9) |
| Free heap (total / internal / SPIRAM), uptime | **yes, but system-wide** | `/api/system/info` (`freeHeap`, `freeHeapInternal`, `freeHeapSpiram`, `uptimeSeconds`) |

### 2.3 What was missing

| Required pilot fact | Missing because |
|---|---|
| Observation mode compiled **and active** | never logged; only inferable from a provider-start line that may never appear |
| Source configured / unconfigured / invalid | the diagnostics model knows it; nothing prints it |
| Source state stream (`START_PENDING` → `SYNCING` → `TRUSTED`/`REJECTED`/`TIMEOUT`/`ERROR`) | `runtime_publish` prints the state only when the *runtime* re-publishes, which an observation device rarely does — the whole point of observation is that nothing changes |
| Trusted-time available / operational / sync age | internal only |
| Network-ready received | not logged |
| Link loss and recovery | not observable at all |
| Protocol still `ALLOW_SOURCE`, runtime still `FREE`, owner still `NONE`, `restore_required` false | published in the snapshot, never periodically logged |
| **B3 session write count**, **heartbeat count** | `proposal_commits` exists in the snapshot but is never logged; there was no heartbeat call counter at all |
| Executor / API inactive | compile-time truth with no runtime evidence |
| Free internal heap **trend**, **minimum** free internal heap | `/api/system/info` gives an instantaneous total; no minimum, and no owner-task correlation |
| Owner-task stack high-water | `uxTaskGetStackHighWaterMark` is used nowhere in the repository |
| Uptime correlated with the above | separate surface, different timebase |
| Sanitized reset class | classified at boot, deliberately never published raw; not logged |
| A bounded diagnostic sequence number | did not exist |

### 2.4 Could the owner get these without new instrumentation?

No — not without changing the public surface, which this gate must not do:

- The Gate B9 operator dashboard reads the Gate B8 API. **The API flag is off in a pilot build**, so
  no dashboard field is reachable. Turning it on to watch a pilot would enable the session-creation
  route, which is exactly what the pilot must prove is unreachable.
- Adding the diagnostics to `/api/system/info` would be an OpenAPI schema change and a public
  privacy decision — Gate B10 §13.4 explicitly reserved that as a deliberate future decision, not a
  side effect.
- A serial-only, default-disabled path therefore both preserves the public surface and keeps the
  data on a cable the owner physically controls.

### 2.5 Privacy scan of existing logs (pre-edit)

Searched every non-test production line in `pool_time`, `pool_session_runtime`,
`pool_session_store`, `pool_session_recovery`, `pool_operation_coordinator`,
`pool_session_execution` and `pool_session_api`:

- `components/pool_time/**` emits **no log line at all** — the entire trusted-time domain is silent,
  so the configured hostname has no logging path even in principle;
- every other line formats only `%s` from a committed token function or `%u` from a bounded counter;
- **no** line carries a hostname, a resolved address, a raw epoch, a pool identity, an account, a
  worker, a password, a session identifier or a record generation.

The pre-existing logging convention was therefore already safe, and the new instrumentation matches
it exactly.

### 2.6 Selected instrumentation and expected files

| Selected | File |
|---|---|
| Pure diagnostic domain: event vocabulary, invariant checker, bounded step, bounded formatters | `components/pool_session_runtime/include/pool_session_runtime_pilot.h` (new) `components/pool_session_runtime/pool_session_runtime_pilot.c` (new) |
| Default-off compile flag | `main/Kconfig.projbuild` |
| Bounded emission on the existing owner task, optional link fact, heartbeat audit counter | `components/pool_session_runtime/pool_session_runtime.c` |
| Optional link-fact dependency (documented, `NULL` by default) | `components/pool_session_runtime/include/pool_session_runtime.h` |
| Conditional `esp_wifi` dependency, new source file | `components/pool_session_runtime/CMakeLists.txt` |
| Tests | `components/pool_session_runtime/test/test_pool_session_runtime_pilot.c` (new) |
| Reproducible build helper + its tests | `tools/pilot/build_time_observation_pilot.py`, `tools/pilot/test_build_time_observation_pilot.py` (new) |
| This document | `docs/NEURALAXE_PHASE_2M1B_B10_1_OBSERVATION_PILOT_PREPARATION.md` (new) |

**No public API change is required or made.** `main/http_server/**`, `openapi.yaml`, the Angular
frontend, the Gate B9 dashboard, the Gate B1 FSM, the B3 schema, the B4 recovery policy, the B5
ownership policy, B7 execution semantics and B8 API semantics are untouched.

---

## 3. What was added

### 3.1 The pure diagnostic domain (`pool_session_runtime_pilot.h/.c`)

No ESP-IDF, no IO, no NVS, no SNTP, no FreeRTOS, no networking, no heap, no logging, no global
mutable state and no clock read. Every function is total and deterministic, so the whole domain is
exhaustively testable off-device — and it is: **54 new QEMU cases** (49 pure, 3 adapter-level, 2
rollback-availability).

**Privacy by construction.** No model has a string field and no function takes a string input, so a
hostname, a resolved address, a raw wall-clock epoch, a pool identity, an account, a worker, a
password, a session identifier, a lease token or a record generation cannot be carried, formatted or
logged. Every age and uptime is **monotonic seconds**, never wall-clock time.

**Event vocabulary** (exactly the required tokens):

```
PILOT_TIME_OBSERVE_BOOT          PILOT_SNTP_TRUSTED
PILOT_TIME_SOURCE_UNCONFIGURED   PILOT_SNTP_REJECTED
PILOT_TIME_SOURCE_INVALID        PILOT_SNTP_TIMEOUT
PILOT_NETWORK_READY              PILOT_SNTP_ERROR
PILOT_SNTP_START_ATTEMPT         PILOT_WIFI_LOST
PILOT_SNTP_SYNCING               PILOT_WIFI_READY
PILOT_OBSERVATION_SUMMARY        PILOT_INVARIANT_VIOLATION
```

`TIME_SOURCE_CONFIGURED` and `TIME_SOURCE_STOPPED` map to **no** event on purpose: they are not
pilot-significant transitions on their own, and the current source state appears on every summary
line, so no posture goes unreported.

**Bounded emission, by construction:**

- lifecycle events are **edge-triggered** — a steady state emits nothing (proved by a 59-tick test
  that asserts zero events);
- the summary is emitted at most **once per 60 monotonic seconds** (proved by a 600-tick test:
  exactly 10 summaries in 10 minutes);
- a **monotonic regression never emits early** — it re-anchors the window and waits the full period
  again;
- a start attempt is edge-triggered on the counter, which the committed Gate B10 budget already
  hard-bounds to 5;
- an invariant violation is emitted when the violation **set changes** (hard bounded by 15 codes) or
  alongside a due summary — never once per tick (proved: 2 lines across 120 ticks, not 120);
- one step can emit at most `POOL_PILOT_EVENTS_MAX` (8) events; a 200-tick adversarial sweep that
  flips the link and cycles every source state asserts the bound holds.

**Bounded formatting.** A line that would not fit its buffer is discarded entirely and reported as
length 0 — never half-written, because a truncated log line is an ambiguous log line. The
worst-case summary (every counter at `UINT32_MAX`, longest token in every vocabulary) is measured by
test against `POOL_PILOT_SUMMARY_MAX` (448 B).

The summary line:

```
PILOT_OBSERVATION_SUMMARY seq=<n> up=<monotonic s> src=<TIME_SOURCE_*> avail=<0|1> oper=<0|1>
  att=<n> agev=<0|1> age=<monotonic s> proto=<protocol_*> rt=<runtime_*> owner=<OWNER_*>
  restore=<0|1> b3w=<n> hbw=<n> exec=<0|1> api=<0|1> heap=<B> heapmin=<B> hwm=<B>
  inv=<PILOT_INV_*> invn=<n>
```

(one physical line; wrapped here for readability)

### 3.2 The invariant checker

A bounded, read-only, fail-closed checker for the healthy observation posture. It reads snapshots
only, performs **no** mutation of any kind, and returns stable machine codes with a deterministic
ordering (`first` is always the lowest-valued violated code).

Healthy posture — store `EMPTY` or `CLEARED`, no session record, owner `NONE`,
`restore_required=false`, B3 writes `0`, heartbeat writes `0`, execution unavailable, API
unavailable, no target-mining grant, no pool-mutation permission, protocol `ALLOW_SOURCE`, runtime
`FREE`, every enum in range and the snapshot structurally valid.

Fifteen stable codes:

```
PILOT_INV_OK                       PILOT_INV_API_REACHABLE
PILOT_INV_STORE_NOT_EMPTY          PILOT_INV_MINING_GRANT
PILOT_INV_SESSION_RECORD_PRESENT   PILOT_INV_POOL_MUTATION
PILOT_INV_OWNER_PRESENT            PILOT_INV_PROTOCOL_HELD
PILOT_INV_RESTORE_REQUIRED         PILOT_INV_RUNTIME_NOT_FREE
PILOT_INV_SESSION_WRITE            PILOT_INV_ENUM_OUT_OF_RANGE
PILOT_INV_HEARTBEAT_WRITE          PILOT_INV_SNAPSHOT_INVALID
PILOT_INV_EXECUTION_REACHABLE
```

An invalid or unknown snapshot field **fails closed in diagnostics** and produces an
invariant-violation line. It does **not** interrupt source mining: this is an observation-only
pilot, and the committed B4/B5/B6 machinery — not a diagnostic — remains the only thing that may
ever hold the protocol.

The checker never restarts the device, never clears a record, never changes the pool and never
alters mining. It has no field through which it could.

### 3.3 The adapter hook

`runtime_step_pilot()` runs on the **existing** Gate B6 owner task, immediately after the committed
`runtime_step_observation()`, inside the same `CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE` block. It:

1. copies the already-published sanitized snapshot and trusted-time diagnostics through their
   existing accessors (each takes and releases the runtime lock internally);
2. **only when a summary is due** (≤ once per minute), reads the bounded platform counters —
   `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`,
   `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`,
   `uxTaskGetStackHighWaterMark(NULL)` (the task measures **itself**, so no foreign handle is
   dereferenced and the stack walk runs once a minute, not once a second);
3. runs the pure checker and the pure step;
4. logs whatever the step returned.

**No lock or critical section is held across any log call or external call.** No new task is
created, no event handler is registered, no socket is opened, no NVS is written and nothing runs
from interrupt context.

`runtime_step_observation()` itself is byte-for-byte the committed Gate B10 function.

A pilot build also raises the owner task's static stack from 4 KiB to 8 KiB — the same bound the
Gate B7 execution flag already uses. The diagnostics add an `snprintf` and an extra `ESP_LOG` frame
to this loop, and an unattended hardware pilot is the worst place to discover a stack overflow. The
cost is 4 KiB of static RAM **in a pilot artifact only**; the shipped default is unchanged.

### 3.4 The optional link fact

`PoolSessionRuntimeDeps.link_up` is an **optional** function pointer, `NULL` in every build without
the pilot flag. `NULL` means "no link fact is available", and the pure step then emits **no**
Wi-Fi event rather than a guessed one. In a pilot build it is bound to one bounded
`esp_wifi_sta_get_ap_info()` call whose record is zeroed immediately and used only for its return
code — no SSID, BSSID, RSSI, channel or address is retained, published or logged.

It feeds **no decision anywhere in B1–B10**: it cannot start or stop the time provider, change a
plan, take a lease, write the store, touch the pool/protocol/ASIC or restart the device.

### 3.5 The heartbeat audit counter

`pool_session_runtime_commit_epoch_heartbeat()` gains a flag-gated counter increment **on entry**
(so even a refused heartbeat is counted). It changes no behaviour and exists for one reason: an
observation pilot must be able to prove that this call had **no caller**, and a compile-time
argument alone is weaker evidence than a runtime zero.

---

## 4. The diagnostics flag

```
config NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    bool "Timed pool session observation-pilot diagnostics (EXPERIMENTAL, Gate B10.1)"
    depends on NX_TIMED_SESSIONS_TIME_OBSERVE
    default n
```

- **default n**, and never auto-enabled by any other flag;
- depends on `NX_TIMED_SESSIONS_TIME_OBSERVE`, which in turn depends only on `NX_TIMED_SESSIONS` —
  so it can never pull in Gate B7 execution or the Gate B8 API;
- a C-level `#error` enforces the same dependency, so a hand-written `sdkconfig` cannot produce a
  half-configured build;
- **no HTTP endpoint, no public schema change, no persistent storage, no new task**;
- with it disabled the adapter is byte-for-byte the committed Gate B10 adapter and the linker pulls
  in **no** `pool_pilot_*` symbol at all.

---

## 5. Proof that no public API or schema changed

| Surface | Status |
|---|---|
| `main/http_server/**` | untouched (`git diff` empty) |
| `main/http_server/openapi.yaml` | untouched |
| `main/http_server/axe-os/**` (Angular frontend) | untouched |
| Gate B9 operator dashboard | untouched; frontend suite identical at 1270 |
| `/api/system/info` response | unchanged; no new field |
| Gate B8 routes | not registered in a pilot build (flag off) |
| Gate B1 FSM, B3 schema, B4 recovery, B5 ownership, B7 execution, B8 API semantics | untouched |
| Weather-Aware Tuning | not present on this branch |

The only header change outside the new pilot header is one **optional, documented, `NULL`-by-default
diagnostic field** in the internal `PoolSessionRuntimeDeps` platform seam, which no HTTP surface can
see.

---

## 6. Source injection: how the hostname stays out of Git

- `CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` **remains an empty string in the repository** and is never
  edited by this gate or by the build helper.
- The owner supplies the source through the required environment variable **`NX_PILOT_NTP_SERVER`**.
- The helper validates it with a faithful port of the committed Gate B10 bounded validator, writes
  it into a temporary `sdkconfig` fragment in a directory **outside the repository** (mode `0600`),
  and **shreds then deletes** that file when the build ends — including on failure.
- The build runs with the build directory and the generated `sdkconfig` **outside the repository**,
  so no hostname-bearing artifact is ever produced inside the working tree. (This also side-steps a
  latent hazard: `.gitignore` contains `!build/config/sdkconfig.json` re-include lines that only fail
  to apply because `build/` itself is excluded. Building elsewhere makes the point moot.)
- The helper never prints the hostname: acceptance reports only the verdict, whether it is a DNS
  name or an IPv4 literal, and the label count. Every rejection message is written to be
  value-free, and this is **asserted by test** — including for a deliberately leaky candidate.
- The build manifest records **`trustedTimeSourceConfigured: true`** and
  `trustedTimeSourceValue: "<never recorded>"`. The helper re-reads the manifest it just wrote and
  **refuses to keep it** if the hostname appears anywhere in it.
- No NTP source is selected by tooling, and none is named anywhere — not even as an example. This is
  asserted **provider-neutrally**: a test runs every string literal in the helper through the
  firmware's own bounded validator and fails if any of them (other than a local build-file name) is a
  usable trusted-time source. Two further tests assert that the
  `CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` define is produced by interpolating the validated
  environment value and nothing else, and that the environment variable is never defaulted.

**If the build system echoes the Kconfig string** (ESP-IDF prints changed config values during
`set-target`), that output exists only in the owner-local, outside-the-repository work directory and
is deleted with it. It must be redacted from any report or transcript that is shared.

---

## 7. The exact pilot flag posture

```
CONFIG_NX_TIMED_SESSIONS=y
CONFIG_NX_TIMED_SESSIONS_NTP_SERVER="<owner-supplied via NX_PILOT_NTP_SERVER>"
CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE=y
CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS=y
# CONFIG_NX_TIMED_SESSIONS_EXECUTION is not set
# CONFIG_NX_TIMED_SESSIONS_API is not set
```

The helper **verifies this from the generated `build/config/sdkconfig.h`** after the build and
refuses to stage anything if execution or the API turned out to be enabled, if any required flag is
missing, or if the configured source does not match the one supplied (compared, never printed).

---

## 8. Reproducible pilot build

```bash
NX_PILOT_NTP_SERVER='<the source you chose>' python tools/pilot/build_time_observation_pilot.py --out-dir '<external-artifact-directory>'
```

`--out-dir` may instead come from `$NX_PILOT_ARTIFACT_ROOT`. **No artifact location is hardcoded
anywhere** — neither in the helper nor in this document. A missing directory is a hard error, never
a silent default, so the package can never land somewhere the owner did not choose.

What it does, in order:

1. reads and validates `NX_PILOT_NTP_SERVER` **before anything else** — a missing or invalid source
   fails before a single build step runs;
2. refuses a dirty working tree and a `-dirty` describe: a pilot artifact must come from an exact
   committed HEAD;
3. records the tracked-tree digest (`git ls-files -s`) so the "nothing was modified" claim is
   checked, not asserted;
4. creates a temporary work directory **outside** the repository (and refuses to run if it would be
   inside one) and writes the temporary config fragment there;
5. builds in the ESP-IDF **v5.5.3** container (or natively when `IDF_PATH` is set), with the repo
   mounted at a fixed container path so no owner-local path leaks into the build;
6. verifies the five flags and the configured source from the generated `sdkconfig.h`;
7. audits the ELF: **refuses** any `pool_session_execution*`, `pool_exec_*`, `nx_pool_execution_*`,
   `pool_session_api_*`, `pool_session_command*`, `nx_pool_session_api_*` or `pool_api_command_*`
   symbol, and **requires** `pool_pilot_*` and `pool_time_source_*` symbols to be present;
8. checks partition fit — `esp-miner.bin` against the 4 MiB app slot, `www.bin` against the 3 MiB
   `www` partition — and records the free bytes;
9. reads the `esp_app_desc_t` version from the built image and refuses a `-dirty` identity;
10. stages the artifacts, computes SHA-256, writes the hostname-free manifest and `SHA256SUMS`;
11. shreds and deletes the temporary config and removes the work directory;
12. re-checks the tracked-tree digest and `git status` and **fails** if the build modified anything.

`--check-source-only` validates the environment variable and exits. `--print-flags` prints the
posture and exits. Neither builds anything and neither prints the hostname.

The helper performs **no** hardware access: no serial port, no COM enumeration, no `esptool`, no
`bitaxetool`, no OTA upload, no device restart, no DNS and no NTP request — asserted by test against
the helper's own source.

---

## 9. Flash-method audit (traced from source, not assumed)

| Route | Code anchor | Writes | Preserves NVS |
|---|---|---|---|
| `POST /api/system/OTA` (Update page) | `esp_ota_get_next_update_partition(NULL)` → `esp_ota_begin/write/end` → `esp_ota_set_boot_partition` → `esp_restart()` | the **inactive** OTA slot + `otadata` | **yes** |
| `POST /api/system/OTAWWW` (Update page) | `esp_partition_find_first(..., SPIFFS, "www")`, erase + write | the **`www`** partition only | **yes** |
| Merged/factory image at `0x0` | `merge_bin.sh` spans `0x0`–`0xf12000` with `0xFF` gap-fill | bootloader, partition table, **`nvs`**, `phy_init`, factory app, `www`, both OTA slots, `otadata` | **NO — erases NVS** |

**Therefore the pilot flash method is the AxeOS Update page (OTA), not a factory flash.** The
required file is `esp-miner.bin`. `www.bin` is *not* written by the application OTA and is therefore
not strictly required — but the release-pair rule (firmware revision must equal the revision embedded
in `www.bin`) means uploading only the application would leave the device reporting a mismatched
`axeOSVersion`. The package therefore ships **both**, built from the same commit, and the owner
uploads `www.bin` first, then `esp-miner.bin`.

The bootloader, the partition table, `config-601.cvs` and any merged/factory image are **excluded**
from the pilot package. They are not written by the audited route, and including a factory image next
to a pilot image invites exactly the mistake that would erase the owner's pool configuration,
Wi-Fi credentials and tuning.

> **Note on the web UI.** The pilot `www.bin` is newer than the image currently on the device: it
> contains the Gate B9 operator-dashboard views added since `v2.14.2-43-gd333dc4`. With the Gate B8
> API compiled out those views have no backend and will report the feature as unavailable. That is
> expected and is not a pilot failure.

---

## 10. Rollback readiness

### 10.1 The known-good rollback artifact

| Item | Value |
|---|---|
| Location | `<known-good-rollback-artifact>` — the owner's export directory for `pool-strategy-polish-v0.1.0-dev-board601` (outside this repository; the exact path is owner-local and deliberately not recorded here) |
| Revision | `v2.14.2-43-gd333dc4` (Phase 2M.0, the last device-facing gate) |
| Application | `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` — SHA-256 `83999e5a6de6280a47412f36a5a74fb8c90f8a6008cc4f35cb90a91562d71f18` |
| Web UI | `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` — SHA-256 `00c1c5aa1b2f4f0bd93366f3555e3171f731fa4935eb948283df5bd7f40ff03a` |
| Factory (last resort only) | `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` — SHA-256 `e39cb5d592965cb3be4f5b8839014b4630f6b6e7e2a68f1d0b151dc90c89c03c` |

**Verified during this gate**, read-only: all three files were re-hashed on disk and match the
recorded `SHA256SUMS` exactly, and `tools/release/export_release.py --check-pair` confirms the
application and web images are a coherent pair, both at `v2.14.2-43-gd333dc4`, neither `-dirty`.
Nothing in the rollback directory was written, renamed or overwritten.

### 10.2 Rollback procedure (least destructive first)

1. **Preferred — OTA re-upload.** Upload the known-good `www.bin`, then the known-good
   `esp-miner.bin`, through the AxeOS Update page. Preserves NVS: Wi-Fi, pools, tuning, fan settings
   all survive. Writes the inactive OTA slot and `otadata` only.
2. **If the dashboard is unreachable** — the application (factory or OTA) embeds `recovery_page.html`
   and `/recovery` always serves it; if the `www` partition content is unusable the default route
   serves the recovery page too, and a new `www.bin` can be uploaded from there.
3. **If the device does not boot far enough to serve HTTP** — serial/USB recovery: power the board
   through the barrel connector, attach USB, and flash the **factory** image at offset `0x0` with
   esptool / esptool-js. **This erases NVS** and the device must be re-provisioned (pool, Wi-Fi,
   tuning, fan). Only after paths 1 and 2 have failed.

### 10.3 Rollback caveat found during this gate

A pilot build compiles `CONFIG_NX_TIMED_SESSIONS` in, which places the Gate B7 mutation fence in
front of `POST /api/system/OTA` and `POST /api/system/OTAWWW`.

- On the empty store the pilot runs against, the fence **admits**: proved by a new test that boots
  the real runtime on an empty store and asserts `NX_ADMIT_ALLOW` for OTA, device restart **and**
  pool patch (and by the committed Gate B5 matrix tests). The web-UI rollback path stays open.
- **However**, if `nx_timed_sessions_boot_init()` ever fails to produce a coherent ownership view,
  the fence fails **closed** (`NX_ADMIT_DENY_UNBOOTSTRAPPED`, HTTP 409) and the OTA route becomes
  unavailable — by design, since a session record might exist. That is also proved by test.

**Consequence for the pilot:** serial/USB recovery (path 3) is a *prerequisite*, not a nicety.
The owner must have the USB cable, a working esptool/esptool-js, and the verified factory image in
hand **before** flashing the pilot image. Pre-flight item 11 exists for exactly this.

### 10.4 Does the rollback artifact match the device?

**Unverified — and unverifiable from here.** No device was contacted. The rollback pair is a
verified, coherent, checksum-matched build, but whether it is the build currently installed on the
owner's Gamma is a fact only the owner can confirm (pre-flight item 10). If the device reports a
different firmware version, the owner must locate or export the matching known-good pair **before**
flashing the pilot image.

---

## 11. Store `EMPTY` / `CLEARED` verification — audited finding

An earlier draft of this document claimed the empty store was "provable without any NVS read". That
claim was audited against the repository and **it does not hold**. It is withdrawn.

### 11.1 The finding

> **Pre-flash EMPTY/CLEARED cannot currently be independently proven using the existing repository
> tooling.**

### 11.2 Why — every candidate method, checked

| Candidate | Verdict |
|---|---|
| Gate B8 status route (`/api/system/timed-session…`) | **Unavailable.** Registered only under `CONFIG_NX_TIMED_SESSIONS_API`, which no committed build sets and which a pilot build must not set. Enabling it to read the store would expose the session-creation route the pilot exists to prove unreachable. |
| Gate B9 operator dashboard | **Unavailable.** It is a client of that same API. |
| Any other HTTP route | **None exists.** Every `#ifdef CONFIG_NX_TIMED_SESSIONS` block in `main/http_server/http_server.c` is a *write fence* (pool PATCH, restart, OTA) — a refusal path, not a read surface. |
| BAP serial interface | **None exists.** Its only timed-session code is `nx_bap_restart_allowed()`, a restart gate. No timed-session read parameter. |
| Repository tooling | **None exists.** `tools/upload2device.py` only POSTs OTA images; `tools/release/*` not only cannot read NVS, it actively *rejects* NVS/flash dumps as artifacts (`FORBIDDEN_NAME_PATTERNS`). |
| `esptool read_flash` of the `nvs` partition | **Disqualified.** It is physical hardware access, and the resulting blob contains the Wi-Fi PSK and pool credentials — it fails the "without exposing pool credentials" requirement no matter which tool parses it afterwards. |
| ESP-IDF `nvs_tool.py` namespace filter | **Disqualified for the same reason** — it filters a dump that must first exist, and producing that dump is the disqualifying step. |

### 11.3 What the two available signals actually are

1. **A deductive argument, not an inspection.** The `nx_tps` namespace
   (`POOL_STORE_NVS_NAMESPACE`) is opened only through the runtime instance, which exists only under
   `CONFIG_NX_TIMED_SESSIONS`; no committed `sdkconfig.defaults`, `sdkconfig.ci`,
   `test/sdkconfig.defaults` or `test-ci/sdkconfig.defaults` sets that flag. So a device that has only
   ever run committed builds *cannot* hold a session record. This is sound reasoning about build
   provenance — it is **not** a measurement of the device in front of you, and it silently fails if
   the device ever ran an unrecorded local build.
2. **Post-flash serial evidence.** On the first boot of the pilot image, `runtime_publish` prints
   `store=STORE_EMPTY` (or `STORE_CLEARED`) and the new summary line confirms the whole healthy
   posture. This is **real evidence, obtained after flashing** — a first-boot abort gate, not a
   pre-flash gate.

### 11.4 Consequence

Because (1) is inference and (2) is post-flash, the pre-flight checklist can only ask the owner to
*accept the residual risk* on the strength of build provenance, plus commit to rolling back
immediately if the first boot does not show an empty store.

**Closing this properly requires a new prerequisite** — see §18, prerequisite 9: a read-only
store-inspection preflight tool/gate that can report the `nx_tps` posture alone, without modifying
NVS, erasing configuration, flashing firmware or exposing pool credentials. Designing it is a
separate gate; this gate deliberately implements no physical-access tooling and executed none.

Until that exists, if the device's build provenance is not certain, **do not flash.**

---

## 12. Pilot artifact package

**External root (outside Git), always supplied by the owner:**

```
<external-artifact-directory>/<git-describe>/
```

Supplied as `--out-dir` or `$NX_PILOT_ARTIFACT_ROOT`. This document and the helper record **no**
concrete location: an artifact directory is an owner-local fact, and hardcoding one would leak a
single machine's layout into every checkout and every future clone.

**Contents, once built:**

| File | Purpose |
|---|---|
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-<describe>-pilot-ota.bin` | the application image (required) |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-<describe>-pilot-www.bin` | the matching web image (pair coherence) |
| `…-pilot-manifest.json` | commit, describe, firmware version, build timestamp, board/ASIC target, sizes, flag summary, `trustedTimeSourceConfigured: true`, flash method, rollback method, SHA-256 of every file |
| `…-pilot-SHA256SUMS.txt` | checksums |
| `PILOT-README.md` | already written at the package root: the exact build command, the flag posture, the flash steps, the rollback paths with verified checksums, and the serial tokens to watch. It points here for the full checklist, plan and criteria rather than duplicating them, so there is one authoritative copy. |

**Never included:** the trusted-time hostname, any credential, any NVS dump, any pool identity, any
owner-local repository path, build caches, the temporary `sdkconfig`, the factory/merged image,
`config-601.cvs`, or any log containing private information.

**Status: the binaries have NOT been produced.** No owner-approved NTP hostname is available, and
this gate does not invent, recommend or silently default one. See §18.

---

## 13. Owner pre-flight checklist

Each item is confirmed by the owner, on the bench, before anything is flashed.

| # | Item | Done |
|---|---|---|
| 1 | The correct physical device: Bitaxe **Gamma v601 / BM1370**, copper SuperSink, two fans on the supplied splitter, standard PSU. Rear VRM heatsink still absent — unchanged for this pilot. | ☐ |
| 2 | Current firmware is stable and the device is mining normally right now. | ☐ |
| 3 | Existing pool identity (host, port, worker/account) recorded **privately**, off this repository, for before/after comparison. | ☐ |
| 4 | Existing frequency recorded and equal to **625 MHz**. | ☐ |
| 5 | Existing configured core voltage recorded and equal to **1150 mV**. | ☐ |
| 6 | Current fan configuration recorded (mode, curve or fixed percent, minimum-fan floor). | ☐ |
| 7 | ASIC and VRM temperature baseline recorded at the current steady state. | ☐ |
| 8 | Power draw and hashrate baseline recorded. | ☐ |
| 9 | Configuration backup completed using an audited supported method (AxeOS settings export / a written record of every setting). | ☐ |
| 10 | Known-good rollback firmware available **and its version matches what is installed** — `v2.14.2-43-gd333dc4`, SHA-256 as in §10.1, re-verified locally. | ☐ |
| 11 | Recovery connection available: USB cable attached, esptool/esptool-js working, factory image and its checksum at hand (see §10.3 — this is a prerequisite, not optional). | ☐ |
| 12 | **Timed-session store.** It **cannot be independently proven `EMPTY`/`CLEARED` before flashing** with today's tooling (§11). The owner must instead: (a) confirm the device has only ever run committed builds — none of which enables `CONFIG_NX_TIMED_SESSIONS`, so no `nx_tps` record can exist; **and** (b) explicitly accept that this is build-provenance inference, not a measurement; **and** (c) commit to rolling back immediately if the first boot does not report `store=STORE_EMPTY`/`STORE_CLEARED`. If the device's build history is not certain, **do not flash.** | ☐ |
| 13 | No timed-session owner exists — same evidentiary status as 12; verifiable only on the first boot log (`owner=OWNER_NONE`). | ☐ |
| 14 | No terminal acknowledgement is pending — same evidentiary status as 12; verifiable only on the first boot log (`restore=0`, `inv=PILOT_INV_OK`). | ☐ |
| 15 | The pilot artifact's manifest shows `CONFIG_NX_TIMED_SESSIONS_EXECUTION` and `CONFIG_NX_TIMED_SESSIONS_API` **disabled**. | ☐ |
| 16 | The owner-approved NTP source was injected locally through `NX_PILOT_NTP_SERVER` and appears **nowhere** in Git, the manifest or any note that will be shared. | ☐ |
| 17 | Artifact SHA-256 values match `…-pilot-SHA256SUMS.txt` on the machine that will do the upload. | ☐ |
| 18 | No unrelated hardware tuning change will be made during the pilot — no overclock, no voltage change, no fan-curve edit, no thermal-threshold experiment, no 1.5 TH/s attempt. | ☐ |

**If item 12 cannot be satisfied, do not flash — the physical-pilot verdict stays NOT READY.** Note
that item 12 is the one checklist entry this tooling genuinely cannot close for the owner (§11);
every other item is a measurement or a file check.

---

## 14. Owner-executed pilot plan (P0 – P6)

**Nothing below has been executed.** Every step is performed by the owner, physically, in front of
the device.

### P0 — baseline (on the *current* firmware, before any flash)

Run the device normally for **at least 20 minutes** and record: hashrate, ASIC temperature, VRM
temperature, fan command and RPM, power draw, free heap where available, uptime, and pool connection
stability (any disconnect/reconnect).

**Do not invent acceptance limits.** The measured baseline *is* the comparison band for P3.

### P1 — flash and first boot

- Owner flashes through the audited route (§9): `www.bin` first, then `esp-miner.bin`, via the AxeOS
  Update page.
- **No configuration reset.** The update method does not require one and must not be given one.
- Capture the serial boot log from the moment of reset.
- Confirm the pilot build marker: `PILOT_TIME_OBSERVE_BOOT`, then a first
  `PILOT_OBSERVATION_SUMMARY`.
- Confirm source mining resumes normally.
- Confirm the pool identity is **unchanged** against the private record from pre-flight item 3.
- Confirm frequency, voltage and fan settings are **unchanged** (625 MHz / 1150 mV / recorded fan
  configuration).
- Confirm no reboot loop.
- Confirm no session/store/owner mutation: `rt=runtime_free owner=OWNER_NONE restore=0 b3w=0 hbw=0
  exec=0 api=0 inv=PILOT_INV_OK`.

### P2 — initial trusted-time observation

- Observe for **at least the full configured sync window** (the committed Gate B10 default,
  reported on every summary as the bounded wait).
- Expect exactly one of `PILOT_SNTP_TRUSTED`, `PILOT_SNTP_TIMEOUT`, `PILOT_SNTP_ERROR` or
  `PILOT_SNTP_REJECTED`.
- **A trusted-time failure is not a mining failure.** Normal source mining must continue regardless.
- Capture the bounded summaries.
- Confirm no heap or stack deterioration trend (`heap`, `heapmin`, `hwm` across summaries).
- Confirm `b3w=0` throughout.

### P3 — steady-state observation

- Continue for **at least 60 minutes**.
- Compare mining, thermal, fan and power behaviour against the P0 baseline.
- **Change no tuning.**
- Confirm diagnostics stay bounded — roughly one summary per minute, no flood.
- Confirm no unexpected owner, record or protocol posture appears.

### P4 — controlled reboot

- One normal reboot using the device's own supported restart (AxeOS restart, or power-cycle if the
  dashboard restart is unavailable).
- Confirm mining returns.
- Confirm a **fresh** trusted-time lifecycle begins (`PILOT_TIME_OBSERVE_BOOT`, a new attempt).
- Confirm no session record is created (`b3w=0`, `owner=OWNER_NONE`).
- Confirm no configuration changed.

### P5 — controlled Wi-Fi interruption (only after P0–P4 pass)

- Interrupt the device's connectivity for a **short bounded interval**.
- **Do not power-cycle the miner during the interruption.**
- **Do not perform a router-wide disruption** if it would affect unrelated systems — prefer
  disabling the device's own association (e.g. an AP-side block for this client) over taking the
  network down.
- Restore Wi-Fi.
- Confirm `PILOT_WIFI_LOST` then `PILOT_WIFI_READY` appear.
- Confirm normal pool reconnection.
- Confirm the trusted-time provider lifecycle stays bounded and **no duplicate provider instance**
  appears (attempt count must not run away; the committed budget caps it at 5).
- Confirm mining resumes.
- Confirm no B3/session mutation (`b3w=0`).

### P6 — finish or roll back

- Export **only sanitized** logs and measurements (see §16).
- Compare against the P0 baseline.
- Roll back if **any** abort condition occurred (§15.2).
- **Do not enable execution or the API. Do not start a timed session.** Neither is possible with
  this artifact; do not build one that makes it possible in order to try.

---

## 15. Criteria

### 15.1 Success

- The correct board boots; no reboot loop.
- The normal source pool remains configured and unchanged.
- Normal mining resumes and continues.
- No timed session exists.
- Gate B5 owner remains `OWNER_NONE`.
- `restore_required` remains false.
- B3 session writes remain **zero** (`b3w=0`) and heartbeat writes remain **zero** (`hbw=0`).
- Execution and the API remain unavailable (`exec=0 api=0`).
- No target-mining grant appears.
- Observation diagnostics stay bounded (≈1 summary/minute; no flood).
- Trusted time reaches `TRUSTED`, **or** fails with a bounded, honest state.
- A trusted-time failure does **not** interrupt empty-store source mining.
- No material heap or stack deterioration trend.
- No new thermal or power anomaly relative to the **measured** P0 baseline.
- Controlled reboot (P4) passes.
- Controlled Wi-Fi recovery (P5) passes.

### 15.2 Immediate abort and rollback

- The pool identity changes.
- Frequency, voltage or fan configuration changes unexpectedly.
- The protocol stays held on an empty store (`proto=protocol_hold` with `rt=runtime_free`, or
  mining never starts).
- Normal mining cannot resume after stable network readiness.
- A timed-session record appears (`b3w` moves off 0, or `PILOT_INV_SESSION_RECORD_PRESENT`).
- A mutating Gate B5 owner appears (`owner=` anything but `OWNER_NONE`).
- `restore_required` becomes true.
- The B3 write count changes from zero.
- Execution or the API becomes reachable (`exec=1` or `api=1`).
- An unexpected target-mining grant appears.
- Repeated crash or reboot.
- Heap decreases **monotonically** across summaries (`heap` and `heapmin` both falling, run after
  run, with no recovery).
- Owner-task stack headroom runs out: `hwm` is the **smallest free stack the task has ever had**
  (bytes, ESP-IDF port), so it only ever shrinks — a value trending toward **0** means the stack is
  about to overflow. A pilot build runs the owner task with an 8 KiB stack, so a healthy `hwm`
  should settle in the low thousands and then stop moving.
- An unexpected thermal safety event occurs.
- The firmware emits `PILOT_INVARIANT_VIOLATION`.
- The recovery or rollback path turns out to be unavailable.

### 15.3 Not an immediate abort

A `TIMEOUT`, `REJECTED` or `ERROR` trusted-time result **alone** is not an abort when mining remains
normal, no invariant is violated, and the failure is bounded and honestly reported.

It is still a **failed trusted-time observation** and must be recorded as such. It is precisely the
kind of result this pilot exists to discover, and it must not be quietly re-run until it passes.

---

## 16. Log and measurement capture plan

| What | How | Sanitization |
|---|---|---|
| Serial boot + run log | USB serial console, **UART0 at 115200 baud** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`, `CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`), logged to a file from reset through P6. `CONFIG_LOG_DEFAULT_LEVEL=3` (INFO), so every pilot line is visible without changing the log level. | The pilot lines are token-only by construction. **Other subsystems are not**: the stratum tasks log the pool host and port. Redact those before sharing. |
| Bounded pilot summaries | `grep PILOT_` over the serial log | already private-free |
| Mining / thermal / power series | AxeOS dashboard readings or `/api/system/info` polling at a fixed interval, recorded manually or to a local file | `/api/system/info` includes the stratum URL and user — redact before sharing |
| Baseline (P0) and steady-state (P3) comparison | same series, same interval, same units | as above |
| Rollback evidence, if used | note which rollback path was taken and the checksum of what was uploaded | no NVS dump, ever |

**Never captured or exported:** an NVS dump, a flash dump, the trusted-time hostname, pool
passwords, Wi-Fi credentials, or any private recovery data.

---

## 17. Validation performed for this gate

All commands ran offline against the committed tree (`b7fb9e6`, clean status), Docker
`espressif/idf:v5.5.3` and the local QEMU image.

### 17.1 Test suites

| Suite | Result |
|---|---|
| QEMU firmware | **806 executed, 0 failed, 0 ignored** (752 committed baseline + 54 new; all 54 `b101` cases PASS) |
| Frontend `npm run test:gate` | **1270 / 1270 executed, 1270 success, 0 failed** — identical to the committed B9/B10 baseline (no frontend change) |
| Build-helper battery (`tools/pilot/test_build_time_observation_pilot.py`) | **98 passed, 0 failed** |

### 17.2 Firmware build postures (all built, all green)

| Posture | Configuration | `esp-miner.bin` | pilot syms | `pool_time_source_*` | exec syms | API syms |
|---|---|---|---|---|---|---|
| **A** | all timed-session flags off — **shipped default** | **1,658,416** | **0** | 0 | 0 | 0 |
| **B** | `NX_TIMED_SESSIONS` only | 1,694,160 | **0** | 7 | 0 | 1 † |
| **C** | B + `TIME_OBSERVE` | 1,694,272 | **0** | 7 | 0 | 1 † |
| **D** | **the pilot posture** — C + `…_PILOT_DIAGNOSTICS` | 1,698,656 | **7 + `runtime_step_pilot`** | 7 | **0** | 1 † |
| **E** | full B7 + B8 stack + `TIME_OBSERVE` | 1,739,664 | **0** | 7 | 51 | 3 |

† The single symbol is `nx_pool_session_api_send_conflict` — the standardized HTTP 409 **body
writer** used by the Gate B7 mutation fence in front of pool PATCH / restart / OTA. It is compiled
by `CONFIG_NX_TIMED_SESSIONS` alone, is a refusal reporter rather than a route or an executor, and
**must** stay linked so the owner's rollback OTA is correctly gated. The build helper allows exactly
this one name and still refuses every other execution/API symbol — including
`nx_pool_session_api_register_routes`, which is tested.

**Posture A is byte-for-byte the size of the committed Gate B10 posture A (1,658,416 B).** This is
the empirical proof that the unconditional `esp_wifi` requirement added an include path and no code:
`esp_wifi_sta_get_ap_info` is linked by the firmware's own Wi-Fi stack in **every** posture including
the untouched default, so nothing new was pulled in.

Posture C carries **zero** pilot symbols, which proves the pilot flag — not `TIME_OBSERVE` — is the
gate.

### 17.3 Other gates

- **Strict compile** of the 15 touched translation units under `-Wall -Wextra -Werror`, run with the
  real build's exact include paths and defines, in **three** postures (default A, pilot D, full
  stack E): **15 / 15, 0 failures** each.
- **RAM impact** (posture D vs posture C, measured on the real xtensa target): `s_pilot` 48 B,
  `s_pilot_line` 448 B, `s_pilot_heartbeat_writes` 4 B, plus the owner-task stack 4,096 → 8,192 B.
  Total **+4,596 B, in a pilot artifact only**. Postures A/B/C/E gain **0 B**. No new task.
- **Flash impact**: D − C = **+4,384 B**, pilot artifact only. `www.bin` unchanged at 3,145,728 B in
  every posture. Both partitions fit with room to spare.
- **No-mutation / no-session-write / no-owner audit** (source level): the pure module contains 0
  references to `pool_session_store_`, `esp_restart`, `nvs_`, the proposal tracker, FreeRTOS, SNTP,
  sockets, DNS, `esp_wifi`, `esp_netif` or any wall-clock call; its only `pool_operation_` references
  are the read-only owner-token function and the types header, and its only `ESP_LOG` occurrence is
  a comment. In the adapter's 118-line pilot block, **every assignment target is a local** — there
  is not one write to `rt->`, and no call to persistence, planning, lease reconciliation or restart.
- **Log privacy scan**: the three new log statements are `ESP_LOGI/W(TAG, "%s", s_pilot_line)`, and
  `s_pilot_line` can only come from the three pure formatters, none of which accepts a string input.
- **Build-helper privacy scan**: no owner-local path, no string literal that would validate as a
  trusted-time source, no `esptool`/`bitaxetool`/serial/`COM<n>`/`/dev/tty` usage, no socket, DNS or
  HTTP call.
- **Secret / owner-local-path scan** across every changed and new file: clean.
- **Line endings** (PowerShell byte counts, since MSYS `grep` reports phantom CRs on this checkout):
  every new and modified C/H/Python/Markdown file is **LF-only**, matching the existing repository
  files exactly; `main/Kconfig.projbuild` stays CRLF in the working tree as it already was. `git
  diff --check` reports nothing.
- **Temporary-config cleanup**: the helper shreds and deletes its fragment in a `finally` block, and
  re-checks the tracked-tree digest and `git status` afterwards.
- `test-ci/CMakeLists.txt` restored byte-exact (**22 B**); `report.xml`, `test-ci/build`,
  `test-ci/sdkconfig` and `tools/pilot/__pycache__` removed. All posture builds ran in a work tree
  **outside** the repository.

**No hardware. No COM/USB. No physical NVS. No real NTP. No DNS. No live pool. No OTA. No flash.**

---

## 18. Three separate verdicts, and every remaining prerequisite

These are three different questions with three different answers. Collapsing them into one
"NOT READY" hides the fact that the software is finished, and collapsing them the other way would
imply a device is ready to flash when nothing has been built or verified against it.

| Question | Verdict |
|---|---|
| **Software preparation** | **COMPLETE** — diagnostics, invariant checker, flag, build helper, tests, audits and this document are all done and green. |
| **Pilot artifact** | **NOT BUILT** — no binary exists, no SHA-256 exists, nothing has been staged. |
| **Physical pilot** | **NOT READY** — blocked on the prerequisites below, several of which can only be satisfied at the bench. |

### 18.1 Remaining prerequisites

| # | Prerequisite | Who | Status |
|---|---|---|---|
| 1 | **Owner-approved NTP source.** Exactly one DNS hostname (≥ 2 labels) or dotted-quad IPv4 literal, supplied as `NX_PILOT_NTP_SERVER`. Never invented, recommended or defaulted by tooling. | owner | **outstanding** |
| 2 | **Successful local pilot build.** `tools/pilot/build_time_observation_pilot.py` run to completion on a clean tree, producing `…-pilot-ota.bin` and `…-pilot-www.bin`. | owner | **outstanding** (blocked by 1) |
| 3 | **Artifact SHA-256 verification.** The staged checksums re-verified on the machine that will perform the upload, against `…-pilot-SHA256SUMS.txt`. | owner | **outstanding** (blocked by 2) |
| 4 | **Installed firmware identification.** Read the device's reported firmware version and `axeOSVersion`. Everything below depends on knowing what is actually on the board. | owner | **outstanding** — no device was contacted |
| 5 | **Verified rollback pair matching the current device posture.** The `v2.14.2-43-gd333dc4` pair is checksum-verified and internally coherent (§10.1), but whether it matches what is *installed* is unknown until 4 is done. If it does not, the matching known-good pair must be located or re-exported **before** flashing. | owner | **outstanding** |
| 6 | **Working serial/USB recovery path.** USB cable attached, esptool/esptool-js proven working, factory image and checksum in hand. Not optional — see the fail-closed OTA fence in §10.3. | owner | **outstanding** |
| 7 | **Physical timed-session store proven `EMPTY` or `CLEARED`.** Currently **not independently provable** — see §11. Until prerequisite 9 exists, this can only be an accepted residual risk based on build provenance plus a first-boot abort gate. | owner | **outstanding, and not fully satisfiable today** |
| 8 | **Final flag verification.** The staged manifest shows `CONFIG_NX_TIMED_SESSIONS_EXECUTION` and `CONFIG_NX_TIMED_SESSIONS_API` disabled and the three pilot flags enabled. (The helper enforces this and refuses to stage otherwise, but the owner should read it.) | owner | **outstanding** (blocked by 2) |
| 9 | **Read-only store-inspection preflight tool/gate.** A way to report the `nx_tps` posture alone — without modifying NVS, erasing configuration, flashing firmware or exposing pool credentials. **Does not exist**; §11 records why every current candidate fails. Designing it is a separate gate. | a later gate | **outstanding — new prerequisite raised by this gate** |

None of 1–9 is satisfied by software alone, and this gate deliberately implemented no
physical-access tooling and executed no physical access.

### 18.2 The one input that unblocks the chain

> **The owner must choose exactly one trusted-time source hostname (or dotted-quad IPv4 literal) and
> pass it as `NX_PILOT_NTP_SERVER` when running the build helper.**

No source is invented, recommended or silently defaulted here. The repository default stays an empty
string; a public NTP hostname is a privacy, availability and jurisdiction decision that belongs to
the product owner.

Once that value exists, §8 produces the artifact and §12 describes exactly what lands in the
package — but note that even a built artifact leaves prerequisites 4–7 and 9 open.

---

## 19. Open items carried forward

1. **The Gate B10 network-trust residual stands.** Standard SNTP/DNS is not cryptographically
   authenticated (no NTS, no DNSSEC). Unchanged, and not hidden by the word "trusted".
2. **The flag-on adapter body is not executed under QEMU.** `CONFIG_NX_TIMED_SESSIONS*` symbols are
   declared in `main/Kconfig.projbuild`, which belongs to the main application project — the
   `test-ci` project cannot define them, so the QEMU image always compiles the flag-off branch. The
   *pure* diagnostic domain, which holds every decision, is fully QEMU-covered in every posture; the
   flag-on adapter body is proved by the firmware build postures, the strict compile and the ELF
   symbol audit. Closing this properly would mean giving the test project its own Kconfig, which is
   a test-harness change this gate deliberately did not make.
3. **Whether the rollback artifact matches the installed firmware is unverified** (§10.4) and is a
   pre-flight item, not a software fact.
3a. **Pre-flash `EMPTY`/`CLEARED` is not independently provable** with existing repository tooling
   (§11). This gate withdrew an earlier claim to the contrary and raised prerequisite 9 — a
   read-only `nx_tps` store-inspection preflight tool/gate — as the proper fix. Not implemented
   here, and no physical access was designed or executed.
4. **The trusted-time diagnostics remain off the public API.** If a future gate wants them there,
   that is a deliberate privacy decision with an OpenAPI schema change — still not a side effect.
5. **`.gitignore` re-includes `build/config/sdkconfig.json`** on a line that only fails to take
   effect because `build/` itself is excluded. Harmless today, and the pilot build avoids the
   repository build directory entirely, but it is worth removing on its own merits.

---

## 20. What must be preserved by later gates

`CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS` default **n**, depending only on
`CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE`; the diagnostics as **observation that authorizes nothing**;
the string-free diagnostic models and the token-only formatters; the monotonic-only 60-second
cadence and the edge-triggered events; the invariant checker as **read-only, fail-closed, and never
an actuator**; `link_up` as an optional diagnostic that feeds no decision; the heartbeat counter as
an audit-only increment; the empty repository default for
`CONFIG_NX_TIMED_SESSIONS_NTP_SERVER`; the rule that the pilot hostname reaches neither Git, nor a
manifest, nor a report, nor a shared log; and every Gate B1–B10 invariant.

**A pilot build must never be shipped as a production default.**
