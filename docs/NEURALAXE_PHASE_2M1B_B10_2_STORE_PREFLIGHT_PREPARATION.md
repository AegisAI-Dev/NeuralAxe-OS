# NeuralAxe OS — Phase 2M.1B, Gate B10.2
## Read-Only Timed-Session Store Preflight and Rollback Readiness

**Scope:** NeuralAxe OS 0.1.0-dev, Bitaxe Gamma / board 601 / BM1370 / ESP32-S3 N16R8 only.
**Base:** committed Gate B10.1 (`3bbfb07` = `v2.14.2-67-g3bbfb07`); branch
`neuralaxe-v0.1-timed-pool-sessions-store-preflight`.
**Nature:** software, tooling and owner instructions only. **No physical preflight has been
performed.** No hardware, COM/USB device, physical NVS, flash dump, LAN, OTA endpoint, NTP server,
DNS resolver or mining pool was contacted while preparing this gate.

---

## 1. The two gaps this gate closes

Gate B10.1 finished with an honest negative finding and two open prerequisites:

> *Pre-flash EMPTY/CLEARED cannot currently be independently proven using the existing repository
> tooling.*

and prerequisite 5, that a rollback pair must be shown to match what is actually installed.

This gate closes both:

1. a **default-disabled read-only timed-session-store inspector** that classifies the `nx_tps`
   namespace and reports one bounded token. It is read-only *for that namespace*; the image it
   ships in is not NVS write-free, and §3a says exactly why and what follows from it;
2. an **offline rollback-readiness verifier** that checks a local rollback package against an
   owner-recorded installed posture.

Neither closes the *physical* question by itself — the owner still executes H0–H6 below. What
changed is that the questions are now answerable at all.

---

## 2. NVS architecture audit

### 2.1 What the committed Gate B3 store already provides

Read in full: `pool_session_store.h/.c`, `pool_session_record.h/.c`, `pool_session_store_nvs.c`,
plus the B4 recovery classification, B5 ownership, B6 bootstrap, B7 admission fence and the
B8/B10/B10.1 surfaces.

| Question | Finding |
|---|---|
| Is `pool_session_store_load()` genuinely read-only? | **Yes.** Its entire call graph — `read_pointer`, `probe_slot`, `read_slot_record` — issues `ops->read_blob` and nothing else. There is no `write_blob` and no `commit` call on any load branch, including every error branch. |
| Does the loader repair, migrate, normalize or promote? | **No.** The committed contract is explicit that a staged newer slot "is IGNORED and never auto-promoted", the loader "never guesses between ambiguous slots", and ambiguity is *returned as a result*, not fixed. Nothing on the load path increments a counter or rewrites a record. |
| Can the decoder be safely reused? | **Yes**, and it should be. Duplicating the schema decoder would create a second implementation that could drift from the one that actually writes records. The preflight injects a read-only ops table into the committed loader instead. |
| Does `pool_session_store_init()` write? | **No** — it validates the ops table and calls `open`. It does, however, **require all five ops to be non-NULL**, which shapes the adapter design below. |

### 2.2 The blocker in the committed NVS backend

`pool_session_store_nvs.c` opens the namespace with **`NVS_READWRITE`**:

```c
err = nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READWRITE, &h);
```

Verified against ESP-IDF v5.5.3 `components/nvs_flash/include/nvs.h`:

- `NVS_READONLY` — *"will open a handle for reading only. All write requests will be rejected for
  this handle."*
- `ESP_ERR_NVS_NOT_FOUND` — *"namespace doesn't exist yet and mode is `NVS_READONLY`"*
- `ESP_ERR_NVS_NOT_ENOUGH_SPACE` is documented for the open path — because a `NVS_READWRITE` open
  **creates** the namespace when it is absent.

**Therefore the committed backend must not be used for a preflight.** On a device that has never
held a timed-session record it would *create* `nx_tps` and write NVS metadata during an operation
the owner intended as a pure inspection — converting "nothing was ever here" into "an empty
namespace now exists". A dedicated read-only backend is mandatory, not stylistic.

### 2.3 ESP-IDF behaviours relied upon

| Behaviour | Finding |
|---|---|
| `nvs_open(..., NVS_READONLY, ...)` on a missing namespace | Returns `ESP_ERR_NVS_NOT_FOUND`; creates nothing. This is the EMPTY answer. |
| Write rejection | The IDF layer rejects writes on a read-only handle — a second line of defence behind the refusing stubs. |
| `nvs_get_blob` size query (`NULL` buffer) | Returns the stored length without copying; mirrored exactly from the committed backend so oversized/truncated values classify identically. |
| Truncated / corrupt values | Surfaced as a length or a decode failure and classified by the committed B3 validator + CRC — never repaired. |
| Handle close | `nvs_close` is idempotent in the adapter and runs on every path, including every error path. |
| NVS initialization | The preflight boot gate **owns** it: one `nvs_flash_init()` call with **no recovery path**, run BEFORE `nvs_config_init()`. It never erases. (The read-only adapter TU itself calls neither.) See §3a.3. |
| Flash encryption | Not enabled on this build (recorded in the Phase 2M.1A audit). A read-only open is unaffected either way; no plaintext is exported regardless. |

### 2.4 Earliest safe boot point

`app_main()` calls `nvs_config_init()` and only afterwards touches anything else. The preflight is
inserted **immediately after that call and before the Gate B6 block**, which is the earliest point
where NVS is usable and nothing has yet opened the timed-session namespace, started Stratum, or
initialized the ASIC.

---

## 3. Exact namespace and read-only strategy (scope: the inspector)

| Item | Value |
|---|---|
| Namespace | **`nx_tps`** (`POOL_STORE_NVS_NAMESPACE`) — the only one opened |
| Keys | `rec_a`, `rec_b`, `active` — the three committed keys, nothing else |
| Open mode | **`NVS_READONLY`**, exactly once |
| Namespace enumeration | none |
| Full-partition read | none |
| Writes / erases / commits **by the inspector** | **none, on any path** |

The **two** mutation entry points in the ops table (`write_blob`, `commit`) exist **only** because
the committed store contract requires a complete table. They are unconditional refusals that count
the attempt:

```c
static int pf_write_blob(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    ...
    if (b != NULL && b->write_attempts < UINT32_MAX) { b->write_attempts++; }
    return POOL_STORE_BACKEND_IO; /* never, on any path */
}
```

The ESP-IDF write API (`nvs_set_*`, `nvs_erase_*`, `nvs_commit`) is **not referenced anywhere in the
adapter translation unit**. That makes "the inspector performed no mutation" a *measured runtime
fact* rather than a claim — and the classifier fails closed if any counter is non-zero, so a broken
inspector can never report a permitting outcome.

**This is a claim about `nx_tps` only.** For what the rest of the image does to NVS, see §3a.

Two honest caveats about the counters:

- `erase_attempts` is a **structural placeholder**. `PoolStoreBackendOps` has no erase entry, so
  nothing in production can increment it; it keeps the fail-closed check total if an erase op is ever
  added. It is not evidence that an erase was attempted and refused — there is no erase path.
- An `nvs_open` failure that is **not** "namespace does not exist" is tracked separately
  (`open_failed`) and classified `IO_ERROR`. Collapsing "unreadable" into "absent" would have
  produced `EMPTY` + `permits_pilot=true` on a store nobody read; that fail-open was found by
  adversarial review of this gate and fixed before any artifact was produced.

---

## 3a. Whole-boot NVS mutation audit — and a corrected claim

An earlier draft of this document said the preflight image "provably cannot write". **That was an
overclaim and is withdrawn.** What was proven is narrower and must be stated precisely:

> **The dedicated inspector never writes the `nx_tps` namespace. The IMAGE is not NVS write-free.**

### 3a.1 Why: `nvs_config_init()` runs first, and it writes

The inspection runs after `nvs_config_init()`, which is pre-existing upstream code. Every NVS
mutation site reachable in a preflight boot, from `app_main` entry onwards:

| # | Site | When | Namespace | Before/after classification |
|---|---|---|---|---|
| 1 | **`nvs_flash_erase()`** (`nvs_config.c:310`) | `nvs_flash_init()` returns `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` | **the WHOLE partition** | **before** |
| 2 | `nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, …)` | always | creates `main` if absent | **before** |
| 3 | `nvs_set_str` — ASIC-frequency migration | legacy key present | `main` | **before** |
| 4 | `nvs_set_u16` — fan-speed migration | legacy key present | `main` | **before** |
| 5–7 | `nvs_erase_key` + `nvs_set_str` — stratum-protocol and SV2-channel-type migrations (3 pairs) | legacy u16 values present | `main` | **before** |
| 8 | `nvs_set_u16` ×2 — `nvs_config_apply_fallback` | settings written back | `main` | **after** |
| 9 | `nvs_set_*` ×6 + **`nvs_commit`** — `nvs_task` settings queue | normal operation / mining startup | `main` | **after** |

### 3a.2 The four required findings

1. **Writes to `nx_tps`: none by key.** Every site above uses the handle opened on
   `NVS_CONFIG_NAMESPACE` (`"main"`); none can address an `nx_tps` key. The inspector's own handle is
   `NVS_READONLY` and its write/commit ops are counting refusals. **But see the erase case below.**
2. **Writes to unrelated namespaces: yes.** The `main` namespace can be created, migrated
   (up to 8 mutations) and later written by the settings queue with a real `nvs_commit`.
3. **Mutations before classification: yes** — sites 1–7.
4. **Mutations after classification, as normal mining starts: yes** — sites 8–9.

> **Correction.** The table above describes the **default** firmware. In preflight posture, sites 1
> and 2 no longer apply as written — see §3a.3. Sites 3–9 still occur, but only **after** the
> classification has completed.

### 3a.3 The case that could destroy the evidence — and how it is now PREVENTED

Site 1 is not an unrelated-namespace write. **`nvs_flash_erase()` erases the entire `nvs`
partition — `nx_tps`, the Wi-Fi credentials and the pool configuration together.**

An earlier revision of this gate *detected* that after the fact and blocked the verdict. **That was
not a mitigation.** It prevented a false permit but not the destruction: by the time the token was
printed, the store, the Wi-Fi configuration and the pool settings were already gone. Detection has
been replaced by prevention.

**In preflight posture the destructive recovery does not exist:**

1. `main/nvs_config.c` compiles the whole recovery branch out under
   `CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT`. There is no `nvs_flash_erase()` call site in the
   preflight build — not an avoided one, an absent one.
2. `nx_tps_preflight_boot_gate()` runs **before** `nvs_config_init()` and owns NVS initialization:
   exactly one `nvs_flash_init()` call, and **no recovery path whatsoever**.
3. `ESP_ERR_NVS_NO_FREE_PAGES`, `ESP_ERR_NVS_NEW_VERSION_FOUND` and every other initialization
   error **fail closed** — emit `TPS_PREFLIGHT_BLOCKED_NVS_INIT`, classify
   `NX_TPS_PREFLIGHT_NVS_INIT_FAILED`, return false.
4. `app_main` returns immediately on a false gate. `nvs_config_init()` is never entered and nothing
   after it runs: no configuration init, no Wi-Fi, no pool, no protocol, no ASIC, no mining. The
   device is left inert for owner recovery.

The blocking token carries **no raw ESP-IDF error text and no numeric error value**.

Because the gate runs first, the classification also completes **before** sites 2–7 can occur:
`nvs_config_init()` has not yet opened `main` `NVS_READWRITE`, has not created it, and has run no
schema migration.

### 3a.4 Post-classification policy: **normal boot continues**

Two policies were available after a *successful* classification.

| Policy | Consequence |
|---|---|
| **A — continue normal boot** (chosen) | No mining downtime, and the AxeOS web UI comes up so the owner can roll back over OTA. |
| B — remain inert until rollback | Side-effect-free, **but Wi-Fi never starts, so the only rollback left is a serial factory flash — which erases NVS.** |

**Policy B makes recovery strictly worse**, and that is decisive: it would force the owner into the
exact destructive operation this gate exists to avoid. A middle option (inert but networked) needs
`nvs_config_init()` anyway, so it buys nothing. Policy A is therefore chosen under the "repository
architecture proves normal continuation is necessary and bounded" carve-out — rollback capability
*requires* the web UI, which requires normal boot.

Continuation is bounded and cannot revise the verdict:

- the verdict is captured in RAM **before** `nvs_config_init()` is entered;
- `nx_tps` cannot be addressed through the `main` handle — different namespace, different handle;
- `nvs_flash_erase()` has no call site in this build;
- `nx_tps_preflight_run_once()` is one-shot, so no later pass can re-classify.

The `main`-namespace schema migrations would in any case run on *any* current firmware — the pilot
image and the rollback image included — so deferring them buys no protection.

### 3a.5 Resolution chosen: **B — read-only for `nx_tps` only**, with destructive recovery removed

Resolution A ("the whole image is write-free") is false on the evidence and is not claimed.
Resolution C (a fully inert posture) is rejected for the rollback reason above. What ships is
Resolution B **plus** the prevention in §3a.3: the image is not write-free, but it can never erase
the partition, and the classification precedes every write-capable path.

Consequently this document says **"read-only timed-session-store inspector"**, never "read-only
image", and the manifest records `wholeImageNvsWriteFree: false` alongside
`timedSessionStoreWritesPossible: false` and `destructiveNvsRecoveryDisabled: true`.

---

## 4. Classification model

Ten stable outcomes, in a pure classifier that never sees a record:

| Outcome | Meaning | Pilot |
|---|---|---|
| `NX_TPS_PREFLIGHT_EMPTY` | namespace absent, or `STORE_EMPTY` | **permitted** |
| `NX_TPS_PREFLIGHT_CLEARED` | committed tombstone (`STORE_CLEARED`) | **permitted** |
| `NX_TPS_PREFLIGHT_RECORD_PRESENT` | a live, non-terminal session record | blocked |
| `NX_TPS_PREFLIGHT_TERMINAL_PENDING` | a terminal record awaiting acknowledgement | blocked |
| `NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN` | `STORE_COMMIT_UNCERTAIN` | blocked |
| `NX_TPS_PREFLIGHT_CORRUPT` | corrupt / invalid / ambiguous / recovery-required | blocked |
| `NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA` | `STORE_UNSUPPORTED_SCHEMA` | blocked |
| `NX_TPS_PREFLIGHT_IO_ERROR` | read failure or uninitialized store | blocked |
| `NX_TPS_PREFLIGHT_NVS_INIT_FAILED` | NVS itself could not be initialized, and **nothing was erased trying** (`TPS_PREFLIGHT_BLOCKED_NVS_INIT`) | blocked |
| `NX_TPS_PREFLIGHT_INTERNAL_ERROR` | **the zero value** — fail-closed default | blocked |

**`INTERNAL_ERROR` is deliberately 0.** Making `EMPTY` zero would mean "forgot to classify" and "the
store is safe" were the same value — the one mistake this gate exists to make impossible. A zeroed
struct, an early return, or an uninitialized read all report BLOCKING.

Additional fail-closed rules, each asserted by test:

- any non-zero write/erase/commit counter ⇒ `INTERNAL_ERROR`, whatever the store said;
- an NVS-init failure ⇒ `NVS_INIT_FAILED`, whatever the store appears to say (asserted across all
  eight store results); a non-zero mutation counter still outranks it, because "the inspector is
  broken" is more severe than "NVS is unusable";
- the loader not having run ⇒ `INTERNAL_ERROR`;
- an out-of-range store result, record kind or session state ⇒ `INTERNAL_ERROR` or `CORRUPT`;
- an unresolved `restore_required` obligation downgrades even a permitting outcome to blocking;
- `pool_state_is_terminal()` — the committed B1 predicate — decides TERMINAL_PENDING, so this gate
  does not re-derive which states are terminal.

Only `EMPTY` and `CLEARED` return `permits_pilot = true`; a sweep over the whole enum asserts
**exactly two** permitting outcomes, and an out-of-enum value never permits.

---

## 5. Proof that CLEARED was derived, not invented

`STORE_CLEARED` is a **pre-existing, independently distinguishable** committed state, not something
this gate introduced:

1. `pool_session_store.h` defines it: `STORE_CLEARED, /* the committed state is a tombstone */`.
2. `pool_session_record.h` defines the kind it comes from:
   `POOL_RECORD_KIND_TOMBSTONE = 2, /* committed acknowledgement/clear marker */`.
3. `pool_session_store_load()` returns it on exactly one condition:

   ```c
   if (committed_kind == (uint8_t)POOL_RECORD_KIND_TOMBSTONE) {
       return STORE_CLEARED;
   }
   ```
4. The B3 crash-consistency model documents how a tombstone is produced: "Acknowledgement/clear is
   itself a crash-safe commit of a TOMBSTONE record through the exact same path."

So CLEARED means: *a tombstone was durably committed through the dual-slot pointer-write-last
algorithm, and the pointer selects it.* The preflight maps `STORE_CLEARED → CLEARED` and
additionally re-checks `POOL_RECORD_KIND_TOMBSTONE` defensively, so a future loader change cannot
quietly widen what counts as a live session record.

**No CLEARED semantics were fabricated.** The gate needed no new meaning.

---

## 6. Feature flag

```
config NX_TIMED_SESSIONS_STORE_PREFLIGHT
    bool "Timed pool session READ-ONLY store preflight (EXPERIMENTAL, Gate B10.2)"
    depends on !NX_TIMED_SESSIONS_EXECUTION && !NX_TIMED_SESSIONS_API && \
               !NX_TIMED_SESSIONS_TIME_OBSERVE && \
               !NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    default n
```

- **default n**, never auto-enabled;
- **mutually exclusive** with execution, the control API, trusted-time observation and pilot
  diagnostics — in Kconfig *and* via a C-level `#error`, so a hand-written `sdkconfig` cannot
  produce a posture that inspects the store while something else may act on it;
- **independent of `CONFIG_NX_TIMED_SESSIONS`.** This was audited, not assumed: the
  `pool_session_store` and `pool_session_record` components compile unconditionally (only
  `pool_session_runtime_boot.c` gates the runtime instance on that flag), so the preflight reads the
  committed schema **without** the timed-session feature being enabled at all.

That independence is the strongest safety property of this posture: with `CONFIG_NX_TIMED_SESSIONS`
off there is no runtime instance storage, no owner task, no B5 coordinator, no B4 recovery
evaluation, no B7 executor, no B8 route and no SNTP — not "disabled", but *not present*.

---

## 7. Boot integration and output tokens

One call, immediately after `nvs_config_init()`:

```c
#ifdef CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT
    nx_tps_preflight_run_once(NULL);
#endif
```

`nx_tps_preflight_run_once()` is **idempotent**: a second call returns the cached result and re-reads
nothing, so no polling loop can exist (asserted by a test that calls it 20 times and checks the run
count stays ≤ 1). The result lives in RAM only and is never persisted.

Emitted tokens:

```
TPS_PREFLIGHT_BOOT
TPS_PREFLIGHT_EMPTY | TPS_PREFLIGHT_CLEARED | TPS_PREFLIGHT_BLOCKED_RECORD
  | TPS_PREFLIGHT_BLOCKED_TERMINAL | TPS_PREFLIGHT_BLOCKED_UNCERTAIN
  | TPS_PREFLIGHT_BLOCKED_CORRUPT | TPS_PREFLIGHT_BLOCKED_SCHEMA
  | TPS_PREFLIGHT_BLOCKED_IO | TPS_PREFLIGHT_INTERNAL_ERROR
TPS_PREFLIGHT_COMPLETE outcome=<TOKEN> pilot=<0|1> ns=<0|1> reads=<n> w=0 e=0 c=0 up=<s>
```

`TPS_PREFLIGHT_BLOCKED` is additionally logged at warning level whenever the outcome does not permit
a pilot, so a blocked result is visible even in a truncated capture.

**Never logged:** the namespace, any key name, NVS error text, record size, generation, session id,
pool identity, credentials, raw payload or trusted epoch. A test sweeps every outcome and asserts the
line contains none of `nx_tps`, `rec_a`, `rec_b`, `active`, `example`, `.`, `:`, `@`, `ESP_ERR`,
`generation` or `epoch`. No HTTP endpoint is added; serial/UART evidence only.

---

## 8. Build helper

```bash
python tools/pilot/build_store_preflight.py --out-dir '<external-artifact-directory>'
```

`--out-dir` may instead come from `$NX_PREFLIGHT_ARTIFACT_ROOT`; a missing directory is a hard error,
never a silent default, and **no location is hardcoded**.

Unlike the B10.1 pilot helper this one requires **no private input at all**: it selects no NTP
source, and it refuses to stage an image whose configuration carries one.

It refuses a dirty tree, records the exact committed HEAD, builds in a work tree **outside** the
repository, verifies the five flags from the generated `sdkconfig.h`, audits the ELF, checks
partition fit, stages with SHA-256, removes its temporary configuration, and re-checks the
tracked-tree digest afterwards.

ELF audit — **refuses** any of `pool_session_execution*`, `pool_exec_*`, `nx_pool_execution_*`,
`pool_session_api_*`, `pool_session_command*`, `nx_pool_session_api_*`, `pool_api_command_*`,
`pool_pilot_*`, `pool_time_sntp_*`, `pool_time_source_*`, plus the two committed B3 **mutation**
entry points `pool_session_store_commit_record` and `pool_session_store_commit_clear`; **requires**
`nx_tps_preflight_*`.

---

## 9. Flash-method finding

Re-confirmed from source for this gate; unchanged from B10.1:

| Route | Writes | Preserves NVS |
|---|---|---|
| `POST /api/system/OTA` | inactive OTA slot + `otadata` | **yes** |
| `POST /api/system/OTAWWW` | the `www` partition only | **yes** |
| merged/factory image at `0x0` | everything, including `nvs` | **NO — erases NVS** |

**The preflight package therefore contains the OTA application image and the matching web image, a
manifest and checksums — and no merged/factory image.** For this gate that exclusion is not merely
prudent: a factory flash would erase the very store the image exists to inspect, destroying the
evidence before it could be read.

---

## 10. Rollback match verifier

```bash
python tools/pilot/verify_rollback_readiness.py --manifest '<rollback-manifest.json>' \
    --installed-version <read off the device> --installed-axeos-version <read off the device> \
    --board 601 --require-factory
```

Strictly offline: it imports no networking or serial module, spawns no subprocess (so it cannot
invoke esptool or Git), and never contacts the device. The installed identity is **supplied by the
owner** — the tool cannot obtain it and does not pretend to.

Checks: files exist; SHA-256 and sizes match the manifest; application and web artifacts are one
coherent pair; board/device/ASIC are Gamma/601/BM1370 and `supportedBoards` names no other; the
manifest records no dirty revision; a factory image exists and matches when serial recovery is
claimed; and the installed posture is compared against the rollback pair.

| Outcome | When |
|---|---|
| `ROLLBACK_READY_EXACT_MATCH` | the rollback pair *is* the installed posture |
| `ROLLBACK_READY_EXPLICIT_DOWNGRADE` | it differs, and `--allow-downgrade` was given |
| `ROLLBACK_BLOCKED_VERSION_UNKNOWN` | no/unparseable installed version, or a differing pair **without** `--allow-downgrade` |
| `ROLLBACK_BLOCKED_BOARD_MISMATCH` | board, device or ASIC is not Gamma/601/BM1370 |
| `ROLLBACK_BLOCKED_PAIR_MISMATCH` | rollback app/web disagree, or the installed app/web disagree |
| `ROLLBACK_BLOCKED_HASH_MISMATCH` | a checksum, a size, or a missing file — i.e. a silent substitution |
| `ROLLBACK_BLOCKED_FACTORY_MISSING` | serial recovery claimed with no factory image |
| `ROLLBACK_BLOCKED_MANIFEST_INVALID` | unreadable, malformed, empty or dirty manifest |

**A downgrade always requires the explicit flag and is never reported as an exact match** — asserted
by test.

---

## 11. Owner-executed preflight plan (H0 – H6)

**Nothing below has been executed.** Every step is performed by the owner, physically.

### H0 — identify the current installation
Record: device firmware version; `axeOSVersion`; board model; the current pool identity
**privately, off this repository**; frequency; voltage; fan configuration; and that the device is
mining normally right now.

### H1 — rollback verification
Run `verify_rollback_readiness.py` against the local rollback package. Require
`ROLLBACK_READY_EXACT_MATCH`, or `ROLLBACK_READY_EXPLICIT_DOWNGRADE` if a different pair is being
accepted deliberately. Confirm the serial/USB cable and the recovery command are available, and that
the factory artifact and its checksum are in hand.

### H2 — baseline
Confirm stable mining before any update. Record ASIC temperature, VRM temperature, hashrate, power
and uptime.

### H3 — install the store-inspector image
**This image is not globally read-only** (§3a). It never writes `nx_tps`, and the classification
completes before anything else touches NVS — but afterwards the pre-existing configuration init may
write the `main` namespace (schema migrations, first-boot defaults, the settings queue), exactly as
any current firmware would. Treat it as a normal firmware update that happens to carry an inspector.

**It cannot erase NVS.** The destructive recovery branch is compiled out in this posture: there is no
`nvs_flash_erase()` call site, and an NVS initialization failure halts boot instead of wiping the
partition (§3a.3). This is the one property that makes the image safe to install for an inspection.

Use **only** the audited OTA application method: upload `…-preflight-www.bin`, then
`…-preflight-ota.bin`, through the AxeOS Update page. **Do not use a merged image.** Do not reset the
configuration. Capture UART0 at **115200 baud** (`CONFIG_LOG_DEFAULT_LEVEL=3`, so every token is
visible without changing the log level).

### H4 — classify the store

**Accept only:** `TPS_PREFLIGHT_EMPTY`, `TPS_PREFLIGHT_CLEARED`.

**Block the observation pilot for:** `TPS_PREFLIGHT_BLOCKED_RECORD`,
`TPS_PREFLIGHT_BLOCKED_TERMINAL`, `TPS_PREFLIGHT_BLOCKED_UNCERTAIN`,
`TPS_PREFLIGHT_BLOCKED_CORRUPT`, `TPS_PREFLIGHT_BLOCKED_SCHEMA`, `TPS_PREFLIGHT_BLOCKED_IO`,
**`TPS_PREFLIGHT_BLOCKED_NVS_INIT`**, `TPS_PREFLIGHT_INTERNAL_ERROR`, **and a missing or ambiguous
token** — an absent result is a blocking result, not a pass.

Confirm the summary line reports `w=0 e=0 c=0` — those are the **inspector's** counters, i.e. proof
that nothing wrote `nx_tps`. They say nothing about the `main` namespace, which the configuration
code may legitimately migrate later in the same boot.

**If `TPS_PREFLIGHT_BLOCKED_NVS_INIT` appears, the device will stop booting — that is intended.**
NVS could not be initialized, and the image refused to erase the partition to recover, so **nothing
was destroyed**: the store, the Wi-Fi credentials and the pool configuration are all still on flash
exactly as they were. Mining will not start and the web UI will not come up.

Recovery from that state:
1. Roll back over **serial/USB** using the known-good factory image (H1 prerequisite). NVS is
   already unusable, so the factory flash costs nothing that was still intact.
2. Re-provision the device and re-run H0–H4.
3. **Do not** re-flash the preflight image expecting a different answer, and do not treat the
   blocked result as a reason to skip to the pilot.

### H5 — confirm no side effect

**Normal mining continues under the inspector image** — that is the chosen policy (§3a.4), so the
device should be back to ordinary operation within the usual boot time. If it is *not* mining,
something is wrong; check for `TPS_PREFLIGHT_BLOCKED_NVS_INIT` first.

Source mining resumes; pool identity unchanged; frequency unchanged; voltage unchanged; fan
configuration unchanged; no timed-session owner; no restore action; no reboot loop.

**On NVS specifically, check the right thing.** The inspector reports `w=0 e=0 c=0` for `nx_tps` —
that is the claim being verified. A one-off configuration *migration* of the `main` namespace on
this boot is expected upstream behaviour and is **not** a preflight failure; it is visible in the
`Migrating NVS config …` log lines. What matters is that the settings the owner recorded at H0 are
unchanged afterwards. Compare them explicitly rather than assuming NVS was untouched.

### H6 — decision
Only after **all** of: an acceptable store result; rollback ready; serial recovery ready; and stable
normal mining — may the owner later proceed to build the Gate B10.1 trusted-time pilot artifact.

**No NTP source is selected in Gate B10.2.** That decision remains entirely with the owner and
belongs to B10.1.

---

## 12. What must be preserved by later gates

`CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT` default **n** and mutually exclusive with execution, the
API, observation and pilot diagnostics; `NVS_READONLY` as the only open mode; the refusing,
counting mutation stubs; `INTERNAL_ERROR == 0` as the fail-closed zero; exactly two permitting
outcomes; unknown or corrupt data never becoming `EMPTY`/`CLEARED`; the string-free models and
token-only reporting; one-shot inspection with no polling; and the rule that a preflight result
authorizes nothing.

**A preflight build must never be shipped as a production default, and must never be the image a
timed session runs on.**
