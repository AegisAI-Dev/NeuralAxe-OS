# NeuralAxe OS — Phase 2M.1B, Gate B3

## Crash-Safe Dual-Slot Persistent Session Record Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-persistence` · **Commit:** `7e85a41` — `feat: add crash-safe dual-slot pool session record store` (**v2.14.2-50-g7e85a41**, parent `639aad4` = the committed Gate B2 report; descends from the B2 implementation `463417d` and the B1 implementation `e72db15`)
**Scope:** isolated persistence foundation only — **no runtime wiring** (no scheduler, boot-order integration, recovery execution, SNTP startup, pool mutation, Stratum control, restart control, HTTP, OTA blocking, or frontend). No production runtime behavior changed; the store compiles in the firmware graph but is never instantiated and is linker-discardable.

Architectural sources of truth: the committed Phase 2M.1A reports (`…_AUDIT.md` §11/§13, `…_STATE_MACHINE.md` §5/§11, `…_SECURITY.md` T12/T13) plus the B1 and B2 foundation reports.

---

## 1. Verdict

**Gate B3 PASS (Phase A implemented + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 10 approved B3 files, 4261 insertions (see §8) |
| Runtime wiring | **none** — zero references to `pool_session_store`/`pool_session_record` outside the component; no task, queue, boot hook or production caller; linker-discardable like B1/B2 |
| B1/B2 sources | untouched (`components/pool_session/`, `components/pool_time/`, `main/` all unchanged) |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**, unchanged; no `=y` in any tracked config |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** (complete run) |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0; regenerated sdkconfig carries the Unity-64-bit + 8 KB-stack test settings |
| QEMU | **298 Tests, 0 Failures, 0 Ignored — exit 0** (83 base + 57 `[pool_session]` + 82 `[pool_time]` + **47 `[pool_record]` + 29 `[pool_store]`**) |
| Main app, flag OFF / ON | both exit 0 (temp sdkconfig copies; tracked `sdkconfig` untouched); store compiles in the main graph, no references |
| Module warnings | all 5 new translation units compile **clean under `-Wall -Wextra -Werror`** (first pass) |
| Secret / local-path scan | clean; synthetic `*.example` fixtures only |
| Tree after verification | clean; `test-ci/CMakeLists.txt` restored byte-exact (22 B); no `report.xml`; no release artifacts |
| Access | no hardware, no physical-device NVS, no real NTP/DNS/pools/LAN/recovery data; no outbound networking anywhere |

---

## 2. What Gate B3 delivers

A new self-contained component **`components/pool_session_store/`** in three layers:

1. **Pure record domain + codec** (`pool_session_record.h/.c`) — the persisted model (only the bounded facts recovery needs), the explicit v1 wire format, the semantic validator, the latest-trusted-epoch floor helper, bounded saturating counters, and field-by-field converters to/from the live B1 `PoolSession`.
2. **Abstract store** (`pool_session_store.h/.c`) — the dual-slot commit/load/tombstone algorithms over an injected `PoolStoreBackendOps` table (synchronous, caller-owned; no task, no queue).
3. **Real ESP-IDF NVS adapter** (`pool_session_store_nvs.c`) — opens ONLY namespace **`nx_tps`** (keys `rec_a`, `rec_b`, `active`); never calls `nvs_flash_init`, never erases anything, never logs payloads/identities/accounts; never instantiated by production runtime in B3.

### 2.1 Wire format (v1, little-endian, field-by-field — never struct memcpy)

`magic 0x4E585053 "NXPS"` · schema u16=1 · header_len u16=24 · total_len u32 · generation u32 (≥1) · kind u8 (SESSION=1 / TOMBSTONE=2) · flags u8=0 · reserved u16=0 · payload_len u32 · payload · **CRC32 (LE) over every byte except itself**. Strings are length-prefixed, bounded (host ≤79, user ≤127, profile ≤23), printable-bytes-only; booleans are strict 0/1 bytes; a disabled fallback is canonically empty; unknown flags/enums/kinds, truncation, oversize and trailing bytes are rejected; the B1 enum numbering is **pinned by `_Static_assert`s** so a renumbering breaks the build, never the flash format. A TOMBSTONE has an **empty payload by definition** — any content is malformed. The active pointer is a **separate 16-byte versioned encoding** (`"NXPT"` · version · slot · reserved · generation · CRC32), never a naked integer. Max encoded record 1024 B (typical ~300–400 B). Golden vectors were derived with an **independent Python/zlib implementation**: tombstone byte-exact (28 B, CRC `0x9F14182D`), session fixture (total 186 B, CRC `0x12B214A6`), pointers (`0xF49303EA`/`0x2D7A7FA1`); the CRC itself is pinned to the standard check value `0xCBF43926` and QEMU-asserted equal to `esp_rom_crc32_le(0,…)`.

### 2.2 What is persisted (and what never is)

Persisted: session id, B1 model version, persistent FSM state, explicit source/target chains, both bounded pool identities (host/port/account/protocol/TLS, primary + fallback), **`restore_required`** and the other safety/verify flags, failure code, six bounded retry counters, duration, verified-start / UTC-deadline (+B2 sync generation) / **latest-accepted trusted epoch** facts with strict valid-flag canonicalization, and bounded reboot/recovery counters (100/10/10, saturating, never wrapping) plus a B4-supplied reset classification. **Never persisted:** passwords (no field exists anywhere; the wire password-policy byte must equal KEEP), HTTP/cJSON bodies, browser state, history, logs, telemetry, raw clock values, NTP server names, pointers, padding. The **account/worker identity is persisted** because restoration requires it; it is never printed or returned by this component and remains **plaintext at rest** because NVS/flash encryption is not enabled in this release — stated, not hidden.

### 2.3 Crash-consistency algorithm

Commit: validate the committed base (a store in any recovery condition **refuses** to commit over the evidence) → write the **inactive** slot → commit → read back and verify **byte-exactly and semantically** → then, and only then, write/commit/read-back/verify the pointer. **The pointer write is the logical commit point**; any pointer-phase failure returns `STORE_COMMIT_UNCERTAIN` and a reload learns the flash truth. The old slot is never erased (crash-recovery evidence). Load never guesses: slots without a pointer are recovery, a corrupt pointer is `STORE_ACTIVE_POINTER_INVALID` with **no highest-generation fallback**, generation mismatches are recovery, a staged newer slot is only a diagnostic and is never auto-promoted. `pool_store_result_permits_session_load()` is true **only** for `STORE_OK` — no corrupt/missing/unsupported outcome can ever recommend pool mutation. Acknowledgement is a **committed tombstone** through the same path, allowed only for `COMPLETE` or pre-mutation `CANCELLED`/`RECOVERY_REQUIRED` with `restore_required == false` — an unresolved restore obligation is unclearable, and `RESTORE_FAILED` is structurally excluded.

### 2.4 ESP-IDF NVS grounding (attributed precisely)

**[IDF documented]:** per-pair power-loss guarantee ("no loss of data, except for the new key-value pair being written at power-off"), automatic page-state recovery, internal thread safety, 15-char key/namespace limit, and — without NVS encryption — physical-access alteration of pairs is possible. **[B3 design]:** nothing stronger than per-pair atomicity is assumed; the multi-step commit is explicitly NOT atomic, which is exactly what the dual-slot + pointer-write-last + readback-verify sequence compensates for. The fake backend models the conservative reading (reads see staged values as real NVS does; power loss drops staged, keeps committed) and is the primary deterministic crash-consistency proof — the real-NVS tests are adapter validation, not the power-loss oracle.

---

## 3. Test suite (76 new tests → QEMU 222 ⇒ 298)

**`[pool_record]` (47):** model init/tombstone/constants · CRC golden + ROM equivalence · deterministic round trips (baseline, active, string extremes, fallback combinations, all persistent states, chains/protocols/retries/failure codes, epoch combinations) · byte-exact golden vectors · decoder rejection (truncation, bad magic/schema/header, unknown flags, length inconsistencies, CRC mismatch, generation zero, unknown kind, tombstone-with-payload, bad state/pw/chain/string bytes, invalid ports, control characters) · semantic invariants (target-active evidence, COMPLETE proof + discharged obligation, obligation-required states, pre-mutation-only CANCELLED, both-valued RECOVERY_REQUIRED, ephemeral rejection, source≠target, epoch relations, counters/ids/durations/layering, fallback rules, hostname-never-determines-chain) · pointer codec (goldens, invalid inputs, **every covered bit corruption detected**) · epoch-floor accept/reject · counter saturation/exhaustion · converters (canonicalization; ephemeral/tombstone rejection) · privacy (clean tokens; tombstone byte-scan) · properties (input immutability, re-encode stability, **every byte corruption of a full encoding detected**).

**`[pool_store]` (29):** lifecycle/guards · empty load · first/alternating commits with generation monotonicity and old-slot retention · restore_required + source identity persistence across simulated reboot · power-loss injection at every commit step (pre-slot, staged-lost, slot-committed-pointer-lost, pointer-staged-uncommitted, post-pointer-commit, slot-readback failure) · staged-slot rules (never promoted, pointerless-slots ⇒ recovery, corrupt-pointer-with-two-valid-slots never guessed) · active-slot corruption (missing, CRC, generation mismatch, unsupported schema; inactive corruption harmless) · tombstone (safe ack, crash-before-flip keeps the old session and stays re-acknowledgeable, unresolved-obligation and empty/cleared conflicts, pre-mutation clearables, **no identity/account bytes in the committed tombstone**) · the single-point-failure sweep (no failed commit ever changes the pointer-selected record to anything but a valid committed one) · result classification totality · clean tokens · real-NVS adapter round trip/reopen, A/B + tombstone on real NVS, and the unrelated-namespace sentinel.

---

## 4. Security posture (honest)

- **CRC32 = accidental-corruption detection only** (torn writes, truncation, malformed blobs). Not authentication, not anti-tamper, not cryptographic integrity, not anti-rollback, not anti-forgery. Generations + A/B slots = **crash consistency only**.
- **Malicious offline rollback or forgery of records remains the documented T13b residual**: with no Secure Boot / flash / NVS encryption, an offline flash writer can recompute CRCs and replay or forge internally-valid records. Bounded by fixed session duration and automatic restore; not mitigated on this build; no MAC/signature code was added in B3.
- No password anywhere; account/worker plaintext-at-rest posture stated in §2.2; no identity, account, key name or raw NVS error text in any public token; the adapter logs nothing.

---

## 5. Engineering notes & gotchas (recorded for future gates)

1. **Whole-struct memcmp round-trip tests need canonical fixtures**: the decoder zero-pads strings, so shortening a fixture string in place leaves residual bytes and a false mismatch — rebuild fixtures with fill helpers. Records need `generation ≥ 1` before encoding (the store assigns it; tests must model that).
2. **Fake-NVS model**: reads through the same handle see staged values (as real NVS does); power loss drops staged and keeps committed — the conservative documented guarantee. Keep the fake, not QEMU NVS, as the power-loss oracle.
3. **Real-NVS tests must be order-independent**: the round-trip test wipes `nx_tps` first via a test-harness `nvs_erase_all` on the isolated QEMU image — the adapter itself has no erase capability.
4. **Flash budget**: the 24 KB `nvs` partition is shared with the entire pool configuration. Typical records ~300–400 B (≤1 KB max), two slots + a transient old-value copy during updates. Gates B6/B7 must persist on state transitions + a bounded heartbeat (2M.1A proposes 60 s) — never per telemetry tick.
5. Static scratch records in the store (`~1 KB` each) are deliberate: task stacks are 8 KB (B1 §5.1) and the store contract is single-owner-synchronous (B5 enforces ownership).

---

## 6. Validation summary (Phase B, against commit `7e85a41`)

- Branch/ancestry verified read-only; only the 10 approved files in the commit; tree clean before and after; **no Git write operations**.
- Frontend gate, fresh test-ci build, full QEMU **298/0/0**, main flag-OFF and flag-ON builds: all green (§1). The mission-named properties (pointer-written-last, staged-newer-ignored, corrupt-pointer-never-guesses, restore_required survival, crash-safe tombstone, no-identity tombstone) were each re-confirmed by their specific committed tests in this exact run.
- Secret/local-path scan clean; `test-ci/CMakeLists.txt` restored byte-exact; no `report.xml`; no release artifacts.
- **No hardware, physical-device NVS, COM/USB, real NTP, live DNS, real BTC/BCH pools, accounts, wallets, credentials, owner LAN, or private TCH recovery data were accessed.** All NVS activity ran against the fake backend or the isolated QEMU test image.

---

## 7. Committed files (10)

| File | Change |
|---|---|
| `components/pool_session_store/CMakeLists.txt` | new — `REQUIRES pool_session`, `PRIV_REQUIRES nvs_flash` |
| `components/pool_session_store/include/pool_session_record.h` | new — model/wire/codec/validator API + pins |
| `components/pool_session_store/pool_session_record.c` | new — CRC32, LE codec, validator, converters, helpers |
| `components/pool_session_store/include/pool_session_store.h` | new — backend ops, store, 16 results, classifiers |
| `components/pool_session_store/pool_session_store.c` | new — dual-slot commit/load/tombstone algorithms |
| `components/pool_session_store/pool_session_store_nvs.c` | new — real NVS backend (`nx_tps` only; no init/erase/logging) |
| `components/pool_session_store/test/CMakeLists.txt` | new — test registration |
| `components/pool_session_store/test/test_pool_session_record.c` | new — 47 Unity tests |
| `components/pool_session_store/test/test_pool_session_store.c` | new — 29 Unity tests (fake-NVS harness + real-NVS adapter) |
| `test/CMakeLists.txt` | `TEST_COMPONENTS` += `pool_session_store` |

---

## 8. Gate B4 prerequisites

1. Branch from this committed B3 head (`7e85a41`).
2. **B4 = pure boot-recovery decision table** (audit §13 / state-machine §6 rows): consume `PoolStoreResult` + the loaded `PoolSessionRecord` + B2's `pool_time_decide_recovery`, and decide — without executing — which pool configuration may start, whether restore is due, and when `RECOVERY_REQUIRED` applies. Specifically: supply the persisted **`latest_trusted_epoch_s` as `required_min_epoch_s`** (the B2 §9 anti-regression floor — never merely `verified_start_epoch`); classify resets into `last_reset_class` (B4 owns `esp_reset_reason` interpretation); advance the bounded counters via the pure increment helpers and map exhaustion (`pool_record_counters_exhausted`) to recovery-required; treat every non-`STORE_OK` load result per `pool_store_result_requires_recovery` with **no pool-mutation recommendation**.
3. Still no execution, no scheduler task, no boot wiring, no NVS writes from recovery logic beyond the store contract, no HTTP.
4. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; the B1 restore-obligation invariants; B2 trust semantics; QEMU ≥ 298 green; frontend 1052.
