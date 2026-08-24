# NeuralAxe OS Phase 2W — Gate W6.4
## Crash-Safe Reboot / Schedule-Window Deduplication — Closure Report

Phase: Weather-Aware Tuning, Gates W6.4 / W6.4.1 · Branch: `neuralaxe-v0.1-weather-recommendation-pilot`
Implementation HEAD: **f40ba5eb4b96e379f0a352c303cfa9432fb14033** ("feat: add crash-safe weather window deduplication") = **v2.14.2-91-gf40ba5e**
Implementation parent: **35973a95bd6e0448b9de0a8ee5e3678e53e0da52** ("docs: finalize W6.3 async weather execution closure report")
Verification posture: fresh committed-state run against `f40ba5e` in unique build/log paths, phases separated (build → run → extract → cleanup), no reused logs.
Working tree clean · This report is the only uncommitted file (owner report commit gate).

| | |
|---|---|
| **Gate W6.4 software verdict** | **CLOSED** |
| **Physical recommendation-pilot verdict** | **NOT READY** |
| **Next gate** | **W6.5 — Gamma 601 Profile Validation (NOT started)** |

---

## 1. Purpose of W6.4 — the defect it closes

Before this gate, the committed W3 schedule tracked which windows had been served in
`WeatherScheduleProgress`, and that structure was **RAM-only**. The W3 header says so in its own
words: the progress model is "version-ready" and W3 "does NOT persist it".

The consequence was a concrete cross-reboot duplication:

```
window served (outbound W3 submission occurs)
  → reboot inside the SAME schedule window
  → RAM progress is gone; the served mask is zero again
  → trusted time is reacquired
  → the committed schedule evaluates the same slot as DUE
  → the same logical window is served a SECOND time
```

Nothing in the pre-W6.4 firmware could detect that. The schedule was correct, the clock was
correct, and the duplication came entirely from the fact that "already served" was not a durable
fact. W6.4 closes it by making the served-window claim a **persistent authority** in the committed
Gate W2 `nx_wtp` store, established *before* any outbound submission for that window is permitted.

---

## 2. The safety contract

The exact delivery contract W6.4 provides, stated in full:

> **AT MOST ONE OUTBOUND WEATHER SUBMISSION PER PERSISTED SCHEDULE WINDOW ACROSS ORDINARY
> CRASHES / REBOOTS WHILE `nx_wtp` SURVIVES.**

What that deliberately is **not**:

- **NOT exactly-once.** There is no distributed transaction between SPI flash and an HTTPS request,
  and none is claimed.
- **NOT at-least-once.** A window may legitimately be lost.
- A crash **after** the durable claim but **before** the HTTP submission **loses that
  recommendation permanently** for that window. This is intentional. Losing one optional weather
  recommendation is strictly safer than issuing duplicate outbound service, and the whole gate is
  built around preferring that direction.

### The load-bearing ordering

```
schedule DUE
  → persistence authorization request          (statistics task; no flash, no blocking)
  → nvs_task durable dual-slot nx_wtp claim    (internal-RAM owner task)
  → verified durable-success completion
  → a LATER statistics tick consumes that success
  → exactly ONE asynchronous W3 submission
```

The forbidden ordering, which the gate exists to make unrepresentable:

```
HTTP submit
  → persistence afterwards
```

Claiming afterwards permits the duplicate directly: power lost between the request and the write
leaves nothing on flash, and the next boot finds the window still open.

---

## 3. Architectural premise correction — W2 had no production consumer

An important finding surfaced during implementation and is recorded here because it changes how the
risk of this gate should be read.

**Before W6.4, `tuning_store` / `nx_wtp` had no production consumer anywhere in this firmware
lineage.** The component was compiled, fully tested and crash-proven in isolation, but it was absent
from `main`'s dependency closure and had zero callers outside its own test suite — which is why
earlier ELF audits found `tuning_store_*` absent from the pilot image entirely.

**W6.4 is therefore the first production wiring of W2 flash persistence into the weather pilot.**

This matters for two reasons:

1. **It raised the execution-context risk.** A store that has only ever run inside a unit-test task
   has never had its *calling context* validated on a real task graph. Nothing in W2's own test
   coverage could reveal which task would end up performing its flash operations, because in test
   the answer was always "the Unity task". That gap is exactly where the defect in §6 lived.
2. **It required explicit flash-owner validation** as a first-class deliverable of this gate, not an
   afterthought — hence the dedicated execution-context audit in §14.

Nothing in this report should be read as implying that W2 was already production-proven *from this
path*. It was proven as a component; W6.4 is what put it into production service.

---

## 4. Schema evolution — `nx_wtp` v1 → v2

The committed record schema moved from v1 to v2 **additively**.

```c
#define TUNING_RECORD_SCHEMA_VERSION      2u
#define TUNING_RECORD_SCHEMA_VERSION_MIN  1u
#define TUNING_RECORD_WINDOW_V2_BYTES     6u
```

Properties of the committed migration:

- **6 appended bytes**, written after every pre-existing v1 field.
- **Every existing W2 field is preserved** byte-for-byte in position and meaning.
- **v1 records are accepted.** The decoder admits the closed range `[MIN, VERSION]`.
- **A v1 record decodes to "no W6.4 historical claim known"** — `window.present == false`. This is
  explicitly *not* "already served", and explicitly *not* corrupt.
- **v2 carries the persistent schedule-window state.**
- **Unsupported future schemas are rejected**, with the stored record left intact — no destructive
  rewrite, no "repair", no erase.
- **Trailing or invalid records are refused, never guessed.** A v1-framed record carrying extra
  bytes is rejected rather than interpreted.
- **`nx_tps` is untouched.** The B3 timed-session namespace is never opened, named or linked from
  any W6.4 path.

The persisted structure is deliberately minimal and mirrors the committed W3 progress shape rather
than inventing a second representation:

```c
typedef struct {
    bool     present;      /* false = no service day claimed yet          */
    uint16_t year;         /* schedule-local service day; 0 iff !present  */
    uint8_t  month;        /* 1..12; 0 iff !present                       */
    uint8_t  day;          /* 1..31; 0 iff !present                       */
    uint8_t  served_mask;  /* bit i => slot i durably claimed on that day */
} TuningScheduleWindowState;
```

When `present` is false every other field **must** be canonical zero, enforced by
`tuning_window_state_validate()`, so a partially-filled absent state cannot exist on flash.

### Migration limitation (explicit, and not a defect)

A device upgrading from pre-W6.4 firmware **while still inside a window that older firmware had
already served** cannot know that historical service happened — the fact was never written down, and
the gate does not fabricate history it does not have.

That window may therefore be served **once** after the upgrade.

This is a **one-time migration limitation, not corruption**, and it is bounded to the single
in-progress window at the moment of upgrade.

---

## 5. Window identity and the write model

The canonical persisted identity is **schedule-local service day + slot index**:

- **Brussels service-day identity** — the schedule's own local date (year/month/day), not a clock
  reading and not an epoch. The same local time on a different day is a different window.
- **Committed schedule slot identity** — the slot index within the committed schedule.
- **Served/claimed mask** — one bit per slot, durably recording which slots of that service day have
  been claimed.
- **Generation and redundant-slot authority** — supplied by the committed W2 dual-slot algorithm:
  two record slots plus a separately versioned active pointer, with the pointer write as the logical
  commit point.

Behaviour that follows from this model:

- **An old day's mask does not suppress a new service day.** A newer service day compares greater,
  and the claim replaces day and mask together.
- **Later slots on the same day remain independently eligible.** A same-day claim extends the
  durable mask (`persisted_mask | slot_bit`) rather than replacing it — and the base is always the
  **durable** mask, never the schedule's RAM view.
- **Day rollover needs no standalone midnight write.** There is no scheduled write at 00:00.
- **The first claim on a new day atomically establishes the new day and its mask** in one commit,
  because the replacement and the first slot bit are written together.
- **A backward service day cannot reopen a spent window.** If trusted time moves backwards across a
  reboot, the older candidate day is refused (`DAY_REGRESSION`) rather than treated as fresh.

---

## 6. The first execution-context defect (found and corrected before owner commit)

The initial W6.4 implementation performed the durable claim **synchronously on the calling task**:

```
statistics_task
  → tuning_store
  → nvs_set_blob
  → nvs_commit
```

An adversarial pre-commit audit established that this was invalid, not merely slow.

**`statistics_task`** (`main/main.c`) is created with:

- priority **3**
- **8192-byte** stack
- **`MALLOC_CAP_SPIRAM`** — the stack lives in **PSRAM**

**The project already had a different contract for flash writers.** `main/nvs_config.c` carries the
comment `// nvs_task heap _must_ be internal memory` directly above:

- **`nvs_task`**, priority **5**
- **8192-byte internal-RAM** stack (created with plain `xTaskCreate`, not `xTaskCreateWithCaps`)
- a **bounded `ConfigUpdate` queue**, depth 20

On this ESP32-S3 build posture, SPI-flash operations **disable the shared flash/PSRAM cache**
(`SPI_FLASH_CACHE_NO_DISABLE` evaluates false: neither flash auto-suspend nor XIP-from-PSRAM is
enabled). ESP-IDF permits a PSRAM task stack only for tasks "where the stack is never accessed while
the cache is disabled".

A flash write from a PSRAM-stacked task therefore accesses an unreachable stack during the operation.
This was an **invalid execution context**, and the project's own committed comment had already said
so in advance.

**This never shipped.** It was found during pre-commit adversarial validation and corrected in Gate
W6.4.1 before the owner commit. It is recorded because the finding — and the fact that the codebase
had already documented the rule — is the substantive engineering result of this gate.

---

## 7. Final flash-owner architecture

The committed architecture routes every W6.4 flash operation through the **existing** internal-RAM
owner:

```
statistics_task
  → bounded persistence request
  → returns IMMEDIATELY (no flash, no blocking wait)

existing nvs_task  (priority 5, 8192 B internal RAM)
  → owns the nx_wtp flash transaction
  → durable dual-slot claim
  → publishes a bounded completion

a LATER statistics tick
  → consumes the durable result
  → ONLY on durable success may it submit W3
```

Committed properties:

- **No second persistence task.** Exactly one task in the entire firmware performs persistence
  writes, and it is the pre-existing `nvs_task`.
- **No task per claim.**
- **`ConfigUpdate` layout is unchanged** — the struct, and therefore every one of the seven existing
  producers, is untouched.
- **Queue depth remains 20.**
- **Weather uses a payload-free sentinel.** The queue item carries `update.key = NVS_CONFIG_COUNT`,
  which is not a settings key; the job itself never leaves the weather component's single pending
  slot. A dropped wake therefore loses nothing — the job is still pending and a later tick wakes
  again.
- **Dispatch occurs before normal configuration-key handling.** The sentinel check is the first
  statement after `xQueueReceive` and `continue`s. This ordering is mandatory: a zero key would be
  `NVS_CONFIG_WIFI_SSID`, and falling through would write a NULL string over the cached SSID.
- **Queue-headroom reservation** (`NX_WX_NVS_QUEUE_RESERVE 4`, checked via
  `uxQueueSpacesAvailable`) prevents weather work from consuming the queue slots that configuration
  writes — including priority-10 overheat writes — depend on.
- **Admission is non-blocking**: `xQueueSend(nvs_save_queue, &update, 0)`. Every other producer in
  that file uses `portMAX_DELAY`, which is correct for them and would be wrong here.

The seam is **injected** (`NxWeatherWindowExecutorOps { wake }`, bound at the end of
`nvs_config_init()`), so the dependency direction is `main → component` only. The weather component
knows nothing about the queue, the task or the sentinel.

---

## 8. Request and completion ownership

### Request

- **One pending slot.** "At most one weather job pending" is structural, not a convention.
- **No pointer to statistics-task stack data survives the request.**
- **Generation-bound.**
- **Carries the canonical window identity.**
- **Duplicate enqueue is structurally prevented** — the slot is claimed inside the critical section,
  so two callers can never both believe they posted.

### The load-bearing property: the executor re-decides

**The executor re-evaluates the window decision against its own latest loaded persistent record
immediately before staging the commit.**

The defect this avoids is specific and severe. The claim writes the window fields by **absolute
overwrite**:

```c
s_staged        = s_rec;
s_staged.window = next;      /* absolute overwrite, not a merge */
```

If the *decision output* were shipped across the task boundary and committed a tick later, a state
computed against an older record could **erase served-mask bits the durable record had gained since**
— re-opening every window of that day and making that erasure the durable truth. The job therefore
carries the decision **inputs** (the plan and slot count), and the authoritative decision is re-run
on the owner task microseconds before staging. The request side's decision is a pre-filter that
authorizes nothing.

### Completion

- **Single bounded slot.**
- **Generation matched**; a completion from a generation this boot no longer owns is **discarded**.
- **Consumed exactly once.**
- **`COMMIT_UNCERTAIN` / ambiguous durability never authorizes weather.** Any result other than
  `TUNING_STORE_OK` — including the uncertain pointer-write outcome — becomes `CLAIM_FAILED`. The
  submit credit is set at exactly one site, reachable only from `CLAIM_DURABLE`, and exactly one site
  returns `GATE_SUBMIT`, guarded by that credit.
- **The committed grant identity is used for the eventual W3 request**, rather than re-deriving the
  window from a fresh clock. Re-deriving would let a midnight rollover between the claim and the
  submission send a request for a window that was never claimed.

---

## 9. Two production blockers found and fixed during W6.4.1

### BLOCKER A — invalid persistence context

The initial runtime `begin` passed a **NULL backend context**:

```c
nx_weather_window_runtime_begin(tuning_store_nvs_ops(), NULL);
```

`tuning_store_nvs_ops()` returns a stateless operations table; the NVS handle and its open flag live
in a **caller-owned `TuningStoreNvsBackend`**. With `ctx == NULL`, `nvs_be_open()` returns
`TUNING_STORE_BACKEND_IO`, `tuning_store_init()` returns `TUNING_STORE_IO_ERROR`, and the store
classifies permanently `UNAVAILABLE`.

**The entire W6.4 dedup chain would have been dormant in every buildable posture**, while every
symbol audit still passed — because the symbols were all present and linked; only the runtime
initialization failed. This is also why the §6 execution-context defect had never manifested on a
board: no `nvs_set_blob` ever executed.

The committed code binds the real backend object:

```c
static TuningStoreNvsBackend s_wx_backend;

return nx_weather_window_runtime_begin(tuning_store_nvs_ops(), &s_wx_backend);
```

### BLOCKER B — unbounded LOAD retry

Once the load became asynchronous, a store that could not be opened or read would be re-requested on
**every ~1 s tick, forever**, queueing a flash transaction ahead of real configuration writes for the
entire uptime of the device.

**The fix has two mandatory parts, and neither works alone:**

1. **Maximum load attempts = 3** — `#define NX_WX_LOAD_MAX_ATTEMPTS 3u`.
2. **The runtime `begin` consumes its own completion**, so the attempt count actually advances.

Without part 2 the constant alone is inert: the completion is never taken, `s_load_attempts` never
increments past its first value, the `INHIBITED` latch is **unreachable**, and retry continues
indefinitely. The committed `begin` calls `consume_completion()` before reading the store fact, for
exactly this reason.

Final committed behaviour:

```
failed store loads
  → bounded attempts (3)
  → NX_WX_WSTORE_INHIBITED  (fail-passive; not serviceable)
  → ZERO outbound weather
  → no 1 Hz permanent flash retry
```

The latch is RAM-only, so a reboot receives a fresh budget and W2 recovery remains the authority.

---

## 10. Crash and failure semantics

The invariant that governs every row below:

> **Persistent failure or uncertainty → ZERO outbound authorization.**

| Case | Committed outcome |
|---|---|
| Virgin due window | Claimed durably, then served exactly once |
| Duplicate tick | One job, one wake, one claim; later ticks refuse |
| Reboot in the same claimed window | Recovered claim suppresses it; no submission |
| Queued claim before execution | No durable claim exists; recovery sees none; window still open |
| Interrupted dual-slot write | Old committed record remains authoritative; never a duplicate |
| Interrupted active-authority (pointer) update | `COMMIT_UNCERTAIN`; treated as failure; no authorization |
| Durable claim, crash before completion | Reboot recovers the claim; window suppressed (recommendation lost) |
| Completion consumed, crash before W3 submit | Reboot recovers the claim; window suppressed |
| W3 submitted, crash before result | Reboot recovers the claim; no duplicate submission |
| Transaction failure | Fail-passive latch for that window this boot; zero submission |
| Ambiguous / `COMMIT_UNCERTAIN` | Never authorizes; only W2 recovery on the next boot may resolve it |
| Stale completion | Generation mismatch → discarded |
| Worker refusal after durable claim | Claim remains **spent**; no rollback; no retry of the same window |
| One corrupt redundant slot | Surviving slot recovers; served window stays suppressed; nothing erased |
| Both slots corrupt | Fail closed; zero service; **nothing erased** |
| Unsupported future schema | Refused; record left intact |
| Later slot, same day | Independently eligible; extends the durable mask |
| New Brussels service day | Not suppressed by yesterday; day and mask replaced atomically |
| Persistence ready before trusted time | Converges on exactly one submission |
| Trusted time before persistence ready | No race to fetch; waits for the authority; converges identically |
| Thousands of ticks | Exactly 3 claims / 6 commits over 3000 ticks |

**Worker refusal after a durable claim**, stated explicitly because it is the most counter-intuitive
row: the claim **remains spent**, there is **no rollback**, and there is **no retry of the same
window**. One recommendation is lost. That is the correct trade.

---

## 11. Write budget

Derived from committed source, not assumed:

- Committed Brussels schedule **`slot_count = 3`** (`out->slot_count = 3u`), ceiling
  `WEATHER_SCHEDULE_MAX_SLOTS = 6`.
- Each successful dual-slot claim performs **2 `write_blob` + 2 `nvs_commit`** (record slot, then
  active pointer).

**Maximum normal daily W6.4 persistence:**

```
3 successful claims / day
  = 6 blob writes / day
  + 6 commits  / day
```

Dynamically confirmed: 3000 ticks produce exactly 3 claims and exactly 6 commits.

**Zero claim transactions occur for:**

- schedule disabled
- no trusted time
- not due
- already claimed
- ordinary statistics ticks

**Queue admission attempts are not persistence writes.** A refused wake performs no flash operation
at all; admission failure and transaction failure are categorically distinct, and only the latter
latches.

---

## 12. Kconfig safety coupling

The W6.4 capability symbol is **`CONFIG_NX_WEATHER_PILOT_WINDOW_DEDUP`** (default `n`,
`depends on NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS`).

The coupling that matters:

```
config NX_WEATHER_PILOT_SCHEDULE
    depends on NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS
    depends on NX_WEATHER_PILOT_WINDOW_DEDUP
```

**Posture D** — schedule requested, dedup capability withheld — was built and audited:

- `posture_d.defaults` requests `CONFIG_NX_WEATHER_PILOT_SCHEDULE=y` and deliberately omits the dedup
  symbol.
- The **generated** `sdkconfig.h` grants **neither**: schedule 0, dedup 0.
- The resulting ELF contains no `nx_weather_window_authorize`, `_decide`, `_runtime_execute`,
  `_runtime_begin`, and no `tuning_store_init` / `_commit_record` / `_nvs_ops`. Only the two
  token-to-string helpers remain.

This prevents a later pilot build from authorizing outbound weather service while bypassing
crash-safe deduplication. The dependency edge is a mechanism; the built posture is the proof.

---

## 13. Destructive NVS recovery boundary

Recorded precisely:

- **W6.4 itself never invokes `nvs_flash_erase`.** Neither `components/weather_window/` nor
  `components/tuning_store/` contains a production erase path (the single occurrence is in a test,
  inside an isolated QEMU image).
- The **pre-existing global NVS recovery path** in `main/nvs_config.c` may call `nvs_flash_erase()` on
  `ESP_ERR_NVS_NO_FREE_PAGES` / `NEW_VERSION_FOUND`, which erases the **entire partition** —
  including `nx_wtp`, alongside Wi-Fi credentials and pool configuration.

The exact guarantee is therefore:

> at-most-once **across ordinary crashes and reboots while `nx_wtp` survives** — **not** across a
> deliberate or destructive partition erase or factory recovery.

**This is not a W6.4 bug.** It is an explicit external scope boundary, owned by a committed path that
predates this gate and that W6.4 neither invokes nor modifies.

---

## 14. Final committed-state validation

All figures below come from the **fresh closure run against
`f40ba5eb4b96e379f0a352c303cfa9432fb14033`**, in unique build/log paths, with no reused logs. Where an
earlier pre-commit figure differed, the fresh figure supersedes it.

| Check | Result |
|---|---|
| **W6.4 tests** | **53 / 53** (record 6, decision 15, runtime 32, e2e+line 10) |
| **Weather QEMU** | **539 / 539**, 0 failures, 0 ignored, **0 in-test panics** |
| **Baseline QEMU** | **851 / 851**, 0 failures, 0 ignored, **0 in-test panics** |
| **Strict compile** | **5 / 5** |
| **Helper suite** | **101 / 101** |
| **Firmware — baseline** | complete, 0 errors |
| **Firmware — pilot** | complete, 0 errors (synthetic private-free configuration) |
| **Firmware — posture D** | complete, 0 errors; capability refused as designed |
| **Real ELF audit** | **AUDIT_TOTAL_FAILURES = 0** |

The ELF audit was run only after **confirmed build completion** and a minimum-size gate — never on a
path that merely exists. Verified categories:

- **Required chain** — 14/14 links present: the `nx_wtp` authority (init / load / commit / nvs ops),
  the W6.4 load and decision, the schedule evaluator, the W6.3 async worker and its submit, the W3
  request builder, the coherent telemetry read, the W1 input projection, the W1 policy evaluator, and
  the single trusted-time provider.
- **Forbidden execution / hardware surfaces** — absent: B10.2 store preflight, B7 timed-session
  execution, weather hardware apply, tuning hardware apply.
- **Singleton authorities** — exactly one each: SNTP initializer, `nx_wtp` commit entry point, W6.4
  executor entry point, W6.4 authorization gate, schedule evaluator, Open-Meteo request builder.
- **Execution-context ownership** — the pilot adapter archive **cannot call**
  `nx_weather_window_runtime_execute`, `tuning_store_init` / `_load` / `_commit_record`,
  `nvs_set_blob`, `nvs_commit` or `nvs_open`; and `main` (which hosts `nvs_task`) **does** call the
  executor.
- **Baseline exclusion** — `BASELINE_FAILURES = 0`; zero W6.4 symbols linked.

Reachability audit additionally confirmed that neither weather archive calls `nvs_flash_erase`,
`nvs_flash_erase_partition`, `esp_restart` or `esp_system_abort`, and neither references `nx_tps_` or
the timed-session store.

---

## 15. Test-evidence nuances (non-blocking, recorded honestly)

Two residual **test-quality** observations. Neither is unresolved runtime behaviour.

**1. `COMMIT_UNCERTAIN`.** The behaviour is exercised by the crash-stage matrix — the "power loss at
EVERY commit stage" test fails the pointer write across successive stages — and it is proven
unreachable from the authorization credit path by construction: the sole credit-granting site is
reachable only from `CLAIM_DURABLE`, which is set only under `r == TUNING_STORE_OK`. There is,
however, **no test whose name is specifically "COMMIT_UNCERTAIN"**. This is a **test-tightening
opportunity, not a software blocker**.

**2. Bounded-load test.** Production code guarantees a maximum of **3** load attempts. The current
dynamic test asserts a **looser upper bound** (historically `<= 4` wakes across 40 cycles). The
looseness is in the assertion, not the code. Again a **test-tightening opportunity, not a production
defect**.

---

## 16. Baseline posture

- The **baseline carries zero W6.4 linked symbols**. Symbol sets before and after are exactly equal
  (7887 / 7887), with none added and none removed.
- The **W6.4 feature cost in the baseline is eliminated by linker garbage collection**. The
  `weather_window` archive is compiled into the baseline build tree and contributes nothing to the
  image — exactly the behaviour of its committed peers (`weather_pilot_diag`, `weather_io`,
  `tuning_store`), all of which build an archive and link zero symbols.

**The generated binaries are NOT claimed to be byte-identical, and should not be.** ESP-IDF builds
carry reproducibility nuances: the `esp_app_desc` structure embeds build metadata (date, time, IDF
version) and the image carries an appended checksum / SHA-256. Differences arising there are build
identity, not code or data. The untouched bootloader differing between two builds of the *same*
sources demonstrates the point directly.

**Symbol and reachability evidence is the semantic baseline proof**, and it is exact.

---

## 17. Safety invariants

W6.4 intentionally adds bounded `nx_wtp` persistence. It adds **no hardware tuning execution
whatsoever**.

The required invariant posture is unchanged:

```
mut_hw      = 0
mut_pool    = 0
mut_proto   = 0
mut_restart = 0
mut_ota     = 0
mut_session = 0

tuning_same = 1
executed    = 0
```

All Gamma 601 profiles remain **`TUNING_VALIDATION_UNVALIDATED`** — all three production registry
entries, verified in the committed source. No validation evidence, no payload, no execution permit
and no hardware apply path exists.

The end-to-end tests each assert `actionable = false`, a `NOT_EXECUTED_NO_PROFILE_SELECTED` reason,
and a clean mutation delta across the whole chain. Zero mutation-assertion failures in the closure
run.

---

## 18. Privacy, network and namespace posture

- **`nx_wtp` is the W6.4 persistence authority** — one namespace macro, one `nvs_open` site.
- **`nx_tps` is untouched.** Zero real references from any W6.4 path; every textual occurrence
  elsewhere is a comment asserting non-use or a forbidden-symbol audit entry.
- **No second persistence namespace** is introduced.
- **No coordinate, provider host, timezone string, NTP server, pool identity or credential** can
  leak through W6.4 diagnostics: every field added to the pilot line is a bounded scalar or a token
  id, pinned by width assertions. The service day and slot mask are governed scheduling state, not
  owner-private data.
- **No raw private configuration appears in diagnostics.**
- **Tests use synthetic, in-memory persistence exclusively.** No physical NVS is touched.
- **No real external network** is contacted anywhere in the validation: the transport is a canned
  body, the clock a synthetic anchor, the store an in-memory model.

---

## 19. Resource impact

- **Static RAM: 2093 bytes**, pilot-only, measured from `libweather_window.a` in the `f40ba5e`
  closure build (36 objects). Absent from the baseline, where the component links zero symbols.
- **No new task.** The gate creates no task, no queue and no timer.
- **`nvs_task` stack unchanged** — 8192 B internal RAM, priority 5.
- **`statistics_task` stack unchanged** — 8192 B, `MALLOC_CAP_SPIRAM`, priority 3.
- **No unbounded per-claim heap.** The gate performs no allocation on any path.
- **W6.4 execution stack fits within the existing `nvs_task` budget.** The deepest W6.4 frame chain
  was measured statically with `-fstack-usage` on the target compiler at roughly 720 bytes before the
  ~440-byte staged record was moved off the stack into a static, which reduces the executor's own
  frame to a few tens of bytes.

**No physical stack high-water measurement exists**, because no hardware run has occurred. The figure
above is a static derivation and is stated as such.

---

## 20. Physical pilot status

**Physical recommendation pilot: NOT READY.**

**This is not a W6.4 defect.** Every W6.4 objective is met, verified and closed.

The remaining reason is singular and belongs to the next gate: **all Gamma 601 tuning profiles remain
`TUNING_VALIDATION_UNVALIDATED`**. No physical hardware validation has authorized any profile or any
hardware execution. Hardware execution remains disabled.

That work is deliberately not started here.

---

## 21. Next gate

**W6.5 — Gamma 601 Profile Validation. NOT started.**

W6.5 will own:

- physical profile-validation evidence
- cooling and PSU qualification
- the evidence fingerprint
- governed validation status
- any future transition from non-actionable to potentially actionable recommendations

No W6.5 work is included in this gate or this report.

---

## 22. Process notes

Validation-process observations from this gate, recorded because they are auditable and reusable:

- **Generated build artifacts must be confirmed complete before any QEMU or ELF inspection.** A
  path existing is not evidence that an artifact is auditable; a mid-link ELF exists and is
  unusable. The audit gates on both a build-completion marker and a minimum artifact size.
- **Unique output and log paths were used for the final closure run**, so no earlier artifact or log
  could be read by mistake.
- **Build → run → result extraction → cleanup phases were separated.**
- **`git archive` was not sufficient**, because `managed_components/` is gitignored and required by
  the build. Validation therefore ran from a **clean working tree proven byte-identical to committed
  HEAD** — `git status --untracked-files=all` and `git diff HEAD` both empty, before and after.
  This is an auditable assumption and is stated rather than hidden.

---

## 23. Closure

**Gate W6.4 software verdict: CLOSED.**

**Physical recommendation-pilot verdict: NOT READY.**

**Implementation HEAD: `f40ba5eb4b96e379f0a352c303cfa9432fb14033` (`v2.14.2-91-gf40ba5e`).**

No W6.5 work is included.

Verification for this closure performed no Git write operation, and touched no physical Gamma
hardware, COM/USB, physical NVS, owner LAN, live weather provider, real NTP, mining pool, OTA path or
owner-private value.
