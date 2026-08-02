# NeuralAxe OS — Phase 2M.1B, Gate B10
## Explicit Trusted-Time Source, Production SNTP Wiring and Observation-Only Pilot Readiness

**Product:** NeuralAxe OS 0.1.0-dev
**Hardware scope:** Bitaxe Gamma / board 601 / BM1370 / ESP32-S3
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-trusted-time`
**Base:** `8023b23` (committed Gate B9 report), descending from `a7793af`
**Commit:** `d256deb` — `v2.14.2-65-gd256deb`
**Verdict:** **PASS** — Phase A and Phase B both green.
**Hardware-pilot verdict:** **READY FOR A SEPARATE OBSERVATION-ONLY PILOT** (prerequisites in §12). Not started.

**Scope:** close the trusted-time source product decision, wire the committed Gate B2 SNTP
provider safely into the production runtime, and add a separately default-disabled
observation-only mode that can later support a supervised physical pilot **without creating,
applying or executing a timed pool session**.

---

## 1. Result summary

| Check | Result |
|---|---|
| QEMU (fresh ESP-IDF v5.5.3 `test-ci` build) | **752 Tests, 0 Failures, 0 Ignored**, 0 explicit `FAIL` lines |
| — of which Gate B10 | **59** new tests (693 committed baseline + 59 = 752) |
| QEMU with the observation flag compiled in | **752 / 0 / 0**, observation path proven to execute |
| Frontend `npm run test:gate` | **1270 / 1270 executed, 1270 success, 0 failed** — unchanged |
| Firmware postures A–F | all **BUILD_OK** |
| Posture A (shipped default) | **zero** Gate B10 symbols; 1,658,416 B |
| Strict compile `-Wall -Wextra -Werror` | **13 / 13** translation units clean |
| Static RAM | **+120 B**, feature-enabled builds only; **0 B** in the default build |
| New tasks | **none** — observation reuses the single Gate B6 owner task |
| Files changed | **12** (4 new, 8 modified) |
| B1 / B3 / B4 / B5 / B7 / B8 / B9 | **0** changed files |
| Frontend / `http_server` | **0** changed files |
| Tree after verification | clean; `test-ci` stub restored byte-exact |
| Access | no hardware, COM/USB, flashing, physical-device NVS, real NTP, live DNS, real pools/accounts/wallets/credentials, OTA, device restart, owner LAN or private recovery data. All builds, QEMU and audits ran offline. |

---

## 2. The defect this gate found and fixed

The committed Gate B2 provider captured the SNTP-to-monotonic anchor correctly, but its
synchronization callback **notified nobody**. The Gate B6 owner task only re-evaluates the B4
plan on an explicit `RUNTIME_EVENT_TIME_SYNC_CHANGED` / `MONOTONIC_BOUNDARY` event, or when the
bounded wait expires.

Consequence on real hardware: after a reboot with an active session, a *successful* SNTP
synchronization would have sat unobserved for the full bounded window (default 600 s) and the
device would then have failed safe toward restore — correct-but-wrong, because trusted time had
in fact been available almost immediately.

**Fix.** `pool_time_sntp.h` gains a bounded `PoolTimeSntpSyncObserver`. It is invoked from
`pool_time_sntp_handle_sync()` — the same function the real ESP-IDF callback calls — **after** the
anchor decision is published and **outside** the module critical section:

```c
    observer     = p->observer;          /* copied under the lock ... */
    observer_ctx = p->observer_ctx;
    portEXIT_CRITICAL(&s_pool_time_lock);/* ... released here ...      */
    p->last_error = verdict;
    if (observer != NULL) {
        observer(observer_ctx, verdict); /* ... invoked with no lock held */
    }
```

The observer receives **only the verdict** — no epoch, no sync generation, no server name. The
runtime's observer records the bounded verdict and posts exactly one task notification through a
new `pool_session_runtime_notify_from_callback()`, which **never** latches into
`control.pending_events` when no task exists (that would be an unsynchronized read-modify-write
from the lwIP tcpip thread); it returns `RUNTIME_ERR_NOT_INITIALIZED` and the bounded tick covers
the gap.

**Test-suite consequence.** The committed Gate B6 helper `inject_trusted_sync()` called
`pool_time_sntp_handle_sync()` *and then* posted `TIME_SYNC_CHANGED` manually. With the observer in
place that delivered the same event twice and drove a second re-evaluation, which retried a
deliberately-injected one-shot write failure and let a test that asserts "blocked until a retry
proves it" advance early. The redundant manual notify was removed; the production callback path now
delivers it exactly once.

---

## 3. Product decision — trusted-time source (closed)

1. NeuralAxe OS ships with **no implicit public NTP hostname**. Verified: no production file in the
   commit contains `pool.ntp.org`, `time.google.com`, `time.windows.com`, `time.apple.com`,
   `time.cloudflare.com` or `time.nist.gov`. Those six strings appear in exactly one place — the
   anti-implicit-server guard test, as a *forbidden* list asserted against the compiled value.
2. `CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` remains an explicit **administrator-build** option.
3. Its safe default remains the **empty string** (`default ""`).
4. Empty **or invalid** means trusted time is unavailable: no fake trust, any state requiring
   trusted time stays protocol-held, observation reports `TIME_SOURCE_UNCONFIGURED`.
5. The hostname is **not** configurable through the unauthenticated LAN API in B10. No API,
   request model or response field was added or changed.
6. DHCP-provided NTP is not automatically trusted (`CONFIG_LWIP_DHCP_GET_NTP_SRV` is unset and the
   B2 adapter rejects the request when it is not compiled in).
7. For a supervised pilot the owner compiles **one** chosen source into a pilot-only build.
8. The selected hostname never appears in a public snapshot, dashboard response or routine log.
9. Standard SNTP and DNS remain **operational** trust mechanisms, not cryptographic
   authentication.
10. Weather-Aware Tuning will consume this same foundation; it must not create a second provider.

---

## 4. Bounded source validation

New pure module `components/pool_time/pool_time_source.{h,c}`. No ESP-IDF, no networking, no DNS,
no NVS, no FreeRTOS, no heap, no logging, no global mutable state, no clock read.

`pool_time_source_validate()` reads **at most `POOL_TIME_SOURCE_HOST_MAX + 1` = 64 bytes** and
never copies, resolves, contacts or logs the candidate.

**Accepted**
- a DNS hostname of 2..16 labels over `[A-Za-z0-9.-]`, each label 1..63 characters, no leading or
  trailing hyphen, no empty label, non-numeric last label; or
- a dotted-quad IPv4 literal: exactly four all-digit labels, 1..3 digits, **no leading zero**,
  value 0..255.

**`TIME_SOURCE_UNCONFIGURED`** (not an error — the shipped default): `NULL` or `""`.

**Rejected** (`TIME_SOURCE_INVALID`, reason `TIME_ERR_INVALID_SERVER_CONFIG`, output fully zeroed):
overlong or unterminated; leading/trailing/embedded whitespace; control characters and any
non-ASCII byte; URL schemes, paths, queries, fragments, userinfo and credentials; empty, overlong
or hyphen-edge labels; leading, trailing or consecutive dots; a single label with no dot;
malformed or ambiguous numeric literals; and **all IPv6 forms**.

Two deliberate design points, both audited against ESP-IDF v5.5.3 sources:

- **Leading zeros are rejected** because lwIP `dns.c:1766` short-circuits numeric hosts through
  `ipaddr_aton()`, which accepts octal (`010` → 8) and hex (`0x0A`) forms. An operator reading
  `010.0.2.1` would not get the address the device contacts.
- **IPv6 is rejected, not half-supported.** `:` is outside the accepted character set and the
  bracketed URI form adds `[`/`]`. lwIP's IPv6 SNTP path is not audited by this gate, so it is
  refused explicitly rather than silently.
- **A single label is rejected** because DNS search-domain behaviour is not audited.

Ten stable machine states are defined: `UNCONFIGURED`, `CONFIGURED`, `INVALID`, `START_PENDING`,
`SYNCING`, `TRUSTED`, `REJECTED`, `TIMEOUT`, `STOPPED`, `ERROR`. `TIME_SOURCE_UNCONFIGURED == 0`, so
a zeroed verdict trusts nothing. The verdict struct stores **no copy of the candidate** — only its
shape (length, label count, IPv4-literal flag). The production adapter retains the compile-time
string only as `deps.ntp_server` plus one `strncpy` into the provider's own bounded config; a
static assertion guarantees any accepted source fits that buffer.

---

## 5. SNTP provider lifecycle

```
B6 runtime boot completes (synchronous, before the protocol barrier)
  -> source validated ONCE at bind time (no DNS, no networking, no allocation)
  -> network-ready event arrives (main.c:183, the single bounded notification)
  -> pure start rule decides
  -> bounded start budget / monotonic backoff
  -> pool_time_sntp_init()          (configure only; ESP-IDF .start = false)
  -> pool_time_sntp_set_observer()  (registered BEFORE start)
  -> pool_time_sntp_start()         (exactly ONE server; never a fallback list)
  -> bounded monotonic synchronization window
  -> callback publishes the candidate anchor through B2
  -> callback notifies the B6 owner task (ONE xTaskNotify, no lock held)
  -> owner task re-evaluates B4/B6
  -> stop/deinit on runtime deinit (observer cleared BEFORE teardown)
```

The pure start rule is total and decides configuration **before** any need or network fact, so an
unconfigured or malformed source can never reach the network:

| # | Condition | Result |
|---|---|---|
| 1 | runtime feature off | no start · `UNCONFIGURED` |
| 2 | no source string | no start · `UNCONFIGURED` |
| 3 | source present but invalid | no start · `INVALID` |
| 4 | neither recovery nor observation needs time | no start · `CONFIGURED` |
| 5 | observation wanted but a session owner exists | no start · `CONFIGURED` |
| 6 | network not ready | no start · `START_PENDING` |
| 7 | provider already started | no start · `SYNCING` (idempotent) |
| 8 | otherwise | **start**, `observation_only = !trusted_time_required` |

No provider start from an HTTP handler, a timer callback or the sync callback — only the single
runtime owner task reaches it.

**Honest limit.** A platform *start* failure drives the B2 provider to its `ERROR` lifecycle, which
by the committed B2 contract only a deinit clears. Later attempts cannot revive it; they consume
the bounded budget and diagnostics report `TIME_SOURCE_ERROR`. No automatic deinit/reinit cycle was
added — tearing the service down and rebuilding it from a bounded retry path is exactly the kind of
unattended recovery this gate is not authorized to introduce.

---

## 6. Callback contract

The observer may perform **only** a bounded task notification. The committed body is:

```c
static void runtime_sync_observer(void *ctx, PoolTimeError verdict)
{
    PoolSessionRuntime *rt = (PoolSessionRuntime *)ctx;
    if (rt == NULL) { return; }
    assert(pool_session_runtime_coordinator_depth() == 0u);
    portENTER_CRITICAL(&s_runtime_lock);
    rt->time_last_sync_result = verdict;
    if (rt->time_sync_callbacks < UINT32_MAX) { rt->time_sync_callbacks++; }
    portEXIT_CRITICAL(&s_runtime_lock);
    (void)pool_session_runtime_notify_from_callback(rt, RUNTIME_EVENT_TIME_SYNC_CHANGED);
}
```

- **No** NVS access, B5 transition, pool operation, protocol operation, restart, HTTP operation,
  allocation or blocking.
- **No** logging of the server hostname — it is not even passed in.
- Lock depth is **zero**: the `assert` makes contract 10 fail loudly rather than be reasoned about,
  and every fake NVS/SNTP op in the suite asserts the same.
- Fires for acceptance **and** rejection; a rejection is a trust-relevant fact the owner must see.
- Duplicate callbacks are idempotent: B2 has already made an unchanged anchor a no-op, and the
  notification is an OR of event bits.
- Cleared before deinit, so no in-flight callback can reach a runtime being zeroed.

---

## 7. Trust acceptance and the anti-regression floor

Trust policy is **entirely B2's** — B10 adds no trust rule and changes no B3 schema field. The
persisted latest-accepted trusted epoch remains the anti-regression floor, supplied by B4 through
`pool_session_recovery_build_time_policy()`.

Proven by test against the real committed engine:

- floor − 1 s (a plausible, in-band, but regressed epoch) → **untrusted**,
  `TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN`, state stays `WAITING_FOR_TRUSTED_TIME`, protocol **HOLD**;
- exactly **at** the floor → accepted (a floor is a minimum, not a gap);
- below the sanity band → `TIME_ERR_EPOCH_BELOW_MIN`; above it → `TIME_ERR_EPOCH_ABOVE_MAX`; both
  refusals observed, neither trusted, diagnostics report `TIME_SOURCE_REJECTED`;
- a duplicate identical sync leaves the accepted anchor byte-identical;
- raw wall-clock mutation never moves the anchor (B2 property, re-asserted);
- an accepted sync alone **never** grants target mining: `target_mining_authorized` and
  `pool_mutation_permitted` are false on every produced snapshot.

Observation mode with no session may accept a B2 anchor but **never writes B3**.

---

## 8. Bounded wait and retry

| Parameter | Value |
|---|---|
| Maximum start attempts | **5** (`POOL_TIME_SOURCE_ATTEMPTS_MAX`) |
| First attempt | immediate |
| Backoff | 15 → 30 → 60 → 120 s (`base << min(n−1, 3)`, capped) |
| Backoff ceiling | 120 s, statically asserted ≤ the B2 bounded sync window (900 s) |
| Clock | monotonic only |

A monotonic regression **fails safe** by waiting the full backoff again rather than treating the
anomaly as elapsed time. An invalid policy fails closed (`exhausted`, never retries). There is no
tight-poll path, no unbounded retry and no retry from callback context.

Behaviour at expiry:

- **active session / recovery requiring time** — B4 remains authoritative; reaching the bound
  never releases the protocol hold and never frees ownership;
- **observation-only, empty store** — timeout is **diagnostic only**; normal source mining
  continues; no restart and no hold is introduced;
- **source invalid or unconfigured** — no DNS, no SNTP start, stable diagnostic posture.

---

## 9. Sanitized diagnostics

A new internal bounded model, published under the runtime lock and copied out through
`pool_session_runtime_time_diagnostics()`:

```
model_version, source_configured, observation_mode_enabled, state,
trusted_time_available, trusted_time_operational, sync_attempt_count,
sync_age_valid, sync_age_s, last_sync_result, wait_elapsed_s, wait_limit_s
```

**Privacy by construction.** Fixed-width scalars only — no string field exists, so a hostname,
resolved IP, DNS error text, server index, raw SNTP status, pool identity, session id, lease token
or record generation cannot be carried. It publishes a **monotonic age**, never a wall-clock epoch,
so no raw system time leaks either. The whole view is smaller than a single hostname buffer.

State priority (total): no runtime / no source → `UNCONFIGURED`; unusable source → `INVALID`;
platform error → `ERROR`; accepted anchor → `TRUSTED`; stopped service → `STOPPED`; refused
candidate → `REJECTED`; expired wait or spent budget → `TIMEOUT`; running → `SYNCING`; initialized
→ `START_PENDING`; else `CONFIGURED`.

`trusted_time_operational` exists separately from `trusted_time_available` precisely so the model
never overstates service health: an accepted anchor keeps trust *available* even while the service
sits in `ERROR`, but not *operational*.

**No Gate B8 or B9 change was made.** The public status JSON, the OpenAPI schema and the operator
dashboard are byte-for-byte the committed B9 versions. Growing the public response surface is a
privacy decision this gate did not need to take, so the diagnostics stay internal and available to
future consumers.

---

## 10. Observation-only pilot mode

`CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE` — **default n**, `depends on NX_TIMED_SESSIONS` **only**.
A fourth, independent safety flag that **authorizes nothing**. It does not depend on the Gate B7
execution flag or the Gate B8 API flag (postures C and D carry the same runtime symbol count as
posture B, which is the empirical proof).

When the runtime feature is on, the observe flag is on, the store is `EMPTY`/`CLEARED` and no
session or recovery owner exists, the runtime may start trusted-time observation after network
readiness. Everything else is forbidden **by construction**: the code reachable from the two
observation `#ifdef` blocks is exactly three functions —

| Function | What it may do |
|---|---|
| `runtime_start_time_provider` | the pure start rule + B2 init/observer/start |
| `runtime_refresh_time_diagnostics` | read bounded facts, publish the diagnostics view |
| `runtime_step_observation` | advance the observation window, refresh the B2 snapshot and diagnostics |

None of them can write B3, acquire or reconcile a B5 lease, mutate the pool, touch the protocol or
the ASIC gate, commit a heartbeat, or restart the device. A mechanical scan of those blocks finds
**0** references to any executor or API symbol.

Observation ends the moment a session owner appears — including one created later in the same boot
through the Gate B8 API:

```c
    if (!rt->time_observation_active || rt->plan.trusted_time_required ||
        rt->record_present) {
        return;
    }
```

**Proven by test on an empty store, on both flag builds:**

| Claim | Evidence |
|---|---|
| protocol permission unchanged | `POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE` before and after; state stays `RUNTIME_FREE` |
| normal mining unchanged | true on success, on timeout, and with an unconfigured source |
| no B3 write | backend write **and** commit counters unchanged; store independently reloaded → still `STORE_EMPTY` after 5 accepted + 5 refused syncs |
| no B5 owner | `OP_OWNER_NONE`, `restore_required` false |
| no heartbeat | `tracker.commit_count == 0` |
| never a session store | `session_present` false; independent reload confirms `STORE_EMPTY` |
| no mining grant / pool mutation | both false on every snapshot |
| unconfigured source | 0 SNTP inits, 0 starts — no network request at all |
| success | diagnostics `TIME_SOURCE_TRUSTED`, available **and** operational |
| timeout | diagnostics `TIME_SOURCE_TIMEOUT`, mining untouched |
| session owner present | provider never starts (0 inits, 0 starts) |

This mode exists only to prepare a supervised pilot. **It is not Weather-Aware Tuning.**

---

## 11. Validation

All commands ran offline against the committed tree (`d256deb`, clean status), Docker
`espressif/idf:v5.5.3` for builds and the `nx-qemu-action` image for QEMU only.

### 11.1 Test suite — 59 new Gate B10 cases

| Group | Cases | Coverage |
|---|---|---|
| `b10 src` | 30 | empty, NULL, valid names, maximum length, overlong, unterminated, whitespace in every position, control and non-ASCII, schemes, paths, queries, fragments, userinfo, credentials, empty/overlong/hyphen-edge labels, dot placement, single label, IPv4 literals, ambiguous/octal/oversized literals, all IPv6 forms, purity, no copy of the candidate |
| `b10 start` | 14 | flag matrix, observation vs session owner, unconfigured/invalid, network gating, duplicate events, and a 256-case exhaustive totality/purity property test |
| `b10 retry` | 10 | defaults, immediate first attempt, backoff growth and cap, hard attempt bound, monotonic regression, invalid policy fails closed |
| `b10 diag` | 16 | fail-closed init, full state-priority table, trust impossible without a source, saturating monotonic age, invalid-enum fallback, no hostname/address/epoch, stable distinct tokens |
| `b10 obs` (provider) | 6 | observer fires on accept and reject, silent after stop/deinit, cleared observer keeps trust, duplicate sync idempotent |
| `b10 policy` | 4 | no implicit public server, B2 defaults still refuse to invent one, accepted source always fits the B2 buffer |
| `b10 rt` | 22 | boot touches no network, exactly one server, duplicate network-ready, deinit drops trust, reboot model, invalid source behaves as none, accepted sync lifts the wait, floor regression, out-of-band refusal, callback isolation, callback without a task |
| `b10 obs` (runtime) | 12 | the observation table in §10 |
| `b10 flags` / `b10 privacy` | 4 | observe cannot enable without the runtime; diagnostics carry no source or session data |

Total QEMU: **752 Tests, 0 Failures, 0 Ignored**, 0 explicit `FAIL` lines — 693 committed baseline
plus 59. Re-run with `-DCONFIG_NX_TIMED_SESSIONS=1 -DCONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE=1`:
again **752 / 0 / 0**, with the observation-only start path proven to execute in the device log.

> **Harness note.** The `test-ci` project does not inherit `main/Kconfig.projbuild`, so every
> `CONFIG_NX_TIMED_SESSIONS*` symbol is unset in the default QEMU image and the flag-gated branches
> are exercised by passing the defines through `CMAKE_C_FLAGS`. The committed `test-ci` runner
> excludes the two `[not-on-qemu]` tests; it must never be replaced by `test/main/unit_test_all.c`.

### 11.2 Firmware build postures

| Posture | Configuration | Build | Size (B) | Gate B10 symbols |
|---|---|---|---|---|
| **A** | all timed-session flags off — **shipped default** | OK | 1,658,416 | **0** |
| **B** | `NX_TIMED_SESSIONS` only | OK | 1,694,144 | source policy + observer wiring |
| **C** | B6 + `TIME_OBSERVE`, empty source | OK | 1,694,240 | + `runtime_refresh_time_diagnostics` |
| **D** | B6 + `TIME_OBSERVE`, configured source | OK | 1,694,256 | same as C |
| **E** | full B8 stack, `TIME_OBSERVE` off | OK | 1,739,520 | same as B |
| **F** | full stack, `TIME_OBSERVE` on | OK | 1,739,632 | same as C |

`nm` on posture A finds **no** `pool_time_source*`, no observer and no diagnostics symbol — the
default build carries none of this gate. `runtime_refresh_time_diagnostics` appears out-of-line
only when the observation flag is set; `runtime_step_observation` is inlined into the owner task
loop and accounts for the C − B delta of **+96 B**. Postures C and D require neither B7 nor B8.

### 11.3 Other gates

- Strict compile of the 13 Gate B10-touched translation units under `-Wall -Wextra -Werror`, with
  the observation branches compiled in: **13 / 13, 0 failures**.
- Frontend `npm run test:gate`: **1270 / 1270 executed, 1270 success, 0 failed** — identical to the
  committed B9 baseline (no frontend change).
- Static RAM, measured on the real xtensa target by diffing struct sizes against the base commit:
  `PoolSessionRuntime` **5192 → 5312 B (+120)**, of which `PoolTimeSntpProvider` +8 B and
  `PoolSessionRuntimeDeps` +4 B. The single instance exists only under `CONFIG_NX_TIMED_SESSIONS`,
  so the default build gains **0 B**. **No new task**; no new stack.
- Callback lock-depth audit: the observer is invoked with no provider lock held and asserts
  coordinator depth 0.
- No-network audit: the Gate B10 tests contain **0** socket, DNS, HTTP or real-SNTP references, and
  `pool_time_sntp_real_ops` is referenced by **no** test in the repository.
- Hostname privacy scan: the source appears in exactly six production lines (two Kconfig
  assignments, a two-line presence check, one `strncpy` into the provider's bounded config, and the
  validator call). It reaches **no** log, snapshot, token or response.
- Secret / owner-local-path scan across every committed file: clean.
- Line endings: CRLF in the index, consistent with every untouched file in the repository.
- `test-ci/CMakeLists.txt` (22 B), `test-ci/main/CMakeLists.txt` (84 B) and
  `test-ci/main/unit_test_all.c` (638 B) restored byte-exact; `report.xml` and build outputs
  removed; `git status` clean apart from this report.

No release export, no hardware flash, no live NTP or DNS access.

---

## 12. Supervised hardware-pilot prerequisites

The pilot is a **separate, owner-executed activity**. It has not been started.

1. This report is committed.
2. The owner chooses **one** NTP source and compiles it into a **pilot-only** build:
   `NX_TIMED_SESSIONS=y`, `NX_TIMED_SESSIONS_TIME_OBSERVE=y`, `NX_TIMED_SESSIONS_EXECUTION=n`,
   `NX_TIMED_SESSIONS_API=n`. The chosen hostname must not enter the repository.
3. Confirm the device session store is `STORE_EMPTY` / `CLEARED` before flashing.
4. Flash, power and observe the device physically — owner-executed, never automated from here.
5. Confirm mining continues normally throughout, and that the pool configuration is identical
   before and after the run.
6. Read the sanitized trusted-time diagnostics. Expect `TIME_SOURCE_TRUSTED` with a plausible sync
   age, or `TIME_SOURCE_TIMEOUT` / `TIME_SOURCE_ERROR` with mining unaffected.
7. Verify the session store is **still empty** after the run.
8. Repeat across at least one reboot and one Wi-Fi drop.

**Verdict: READY FOR A SEPARATE OBSERVATION-ONLY PILOT**, conditional on 1–3.

---

## 13. Open items carried forward

1. **Network-trust residual stands.** Standard SNTP/DNS is not cryptographically authenticated (no
   NTS, no DNSSEC). Bounded by the sanity band, the monotonic-only same-boot anchor, the persisted
   floor, the bounded window and fail-safe restore. Unchanged from Gate B2 and not hidden by the
   word "trusted".
2. **A platform start failure is not auto-recovered** (see §5). Deliberate.
3. **IPv6 and DHCP-provided NTP remain unsupported**, pending an explicit audit and an explicit
   product gate.
4. **The trusted-time UX in the operator dashboard is unchanged.** If a future gate wants the new
   diagnostics on the public surface, that is a deliberate privacy decision with an OpenAPI schema
   change, not a side effect.

---

## 14. What must be preserved by later gates

`CONFIG_NX_TIMED_SESSIONS_NTP_SERVER` default empty and never a public hostname; no implicit NTP
server anywhere; `CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE` default n depending only on
`CONFIG_NX_TIMED_SESSIONS`; the bounded validator and its rejection set; the exactly-one-server
configuration; the callback contract (bounded, lock-free, notification-only, hostname-free);
provider start only after network readiness, at most once per lifecycle, never from an HTTP handler
or timer; the bounded attempt budget and monotonic backoff; B2 as the sole trust authority and the
persisted epoch floor as the anti-regression bound; observation as measurement that authorizes
nothing and writes nothing; the string-free diagnostics model; and every Gate B1–B9 invariant.
QEMU ≥ 752 green; frontend 1270.

**Weather-Aware Tuning must consume this trusted-time foundation. It must not create a second time
provider.**
