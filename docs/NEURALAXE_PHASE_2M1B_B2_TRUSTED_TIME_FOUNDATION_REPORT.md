# NeuralAxe OS — Phase 2M.1B, Gate B2

## Trusted SNTP Time Provider & Abstract Clock Foundation Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-time` · **Commit:** `463417d` — `feat: add trusted SNTP time and abstract clock foundation` (**v2.14.2-48-g463417d**, parent `e6f360e` = the committed Gate B1 report; descends from the B1 implementation `e72db15`)
**Scope:** isolated, testable firmware time foundation only — **no runtime wiring** (no NVS, dual-slot records, scheduler tasks, pool mutation, Stratum control, boot-order integration, HTTP, OTA blocking, frontend, or restore execution). No production runtime behavior changed; the provider compiles but is never instantiated or started.

Architectural sources of truth: the committed Phase 2M.1A reports (`…_AUDIT.md` §7, `…_STATE_MACHINE.md`, `…_SECURITY.md` T16/T17) and `NEURALAXE_PHASE_2M1B_B1_FSM_FOUNDATION_REPORT.md`.

---

## 1. Verdict

**Gate B2 PASS (Phase A implemented + owner trust-boundary pass + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 9 approved B2 files, 3509 insertions (see §8) |
| Runtime wiring | **none** — zero references to `pool_time` outside `components/pool_time/`; `main/` unchanged since `e72db15`; the provider and both UTC-deadline helpers have no production callers |
| Stratum ntime behavior | **unchanged** (no `main/` diff since B1; `SYSTEM_notify_new_ntime` untouched) |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**, unchanged since B1; effective config carries no `=y` |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** (complete; Brave via `CHROME_BIN`, per the B1 host note) |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0; regenerated sdkconfig carries `CONFIG_UNITY_ENABLE_64BIT=y` + `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` |
| QEMU | **222 Tests, 0 Failures, 0 Ignored — exit 0** (83 baseline + 57 `[pool_session]` + **82 `[pool_time]`**) |
| Main app, flag OFF / ON | both exit 0 (temp sdkconfig copies; tracked `sdkconfig` untouched); `pool_time` compiles in the main graph and is linker-GC'd (no references) |
| Module warnings | `pool_time.c`, `pool_time_sntp.c`, `test_pool_time.c` compile **clean under `-Wall -Wextra -Werror`** (verified per-TU via `compile_commands.json`) |
| Secret / local-path scan | clean (synthetic `*.example` fixtures only; no hostnames, secrets, IPs, or absolute paths) |
| Network | **no traffic attempted** — tests use fake ops exclusively; the real adapter was compiled, never initialized, never started; QEMU runs offline |
| Tree after verification | clean; `test-ci/CMakeLists.txt` restored byte-exact (22 B); no `report.xml`; no release artifacts |

---

## 2. What Gate B2 delivers

A new self-contained component **`components/pool_time/`** (modeled on the B1 `pool_session` precedent, deliberately **not** coupled to it — duration-bound parity is asserted in the test file only):

- `include/pool_time.h` + `pool_time.c` — the **pure time domain**: machine codes, bounded models, the abstract clock contract, the exact trusted-time predicate, SNTP-anchor validation, overflow-safe monotonic and UTC deadline helpers, the bounded synchronization-window policy and the pure reboot/recovery time decision. No ESP-IDF, networking, NVS, FreeRTOS, heap, logging or global mutable state.
- `include/pool_time_sntp.h` + `pool_time_sntp.c` — the **ESP-IDF v5.5.3 adapter**: owns the in-boot SNTP-to-monotonic anchor behind an injectable `PoolTimeSntpPlatformOps` table; the real ops bind `esp_netif_sntp` / `esp_sntp` / `esp_timer`. Compiled as a production component; **never instantiated or started by any production path in B2**.
- `test/test_pool_time.c` — **82 exhaustive deterministic Unity tests** (`[pool_time]`), fake clock + fake SNTP completion only.

### 2.1 Trust definition (exact wording)

**"Trusted time" means: time accepted by the NeuralAxe scheduler trust policy after an SNTP callback in the current boot.** It is an *operational* verdict, not a cryptographic one (§5).

### 2.2 The predicate

TIME_OK iff **all**: an SNTP synchronization completed this boot (valid anchor + flag) · anchor epoch inside the compiled sanity band **[2025-01-01T00:00:00Z … 2100-01-01T00:00:00Z]** (= 1735689600…4102444800, static-asserted) · `monotonic_now ≥ anchor.monotonic` · `anchor + elapsed` does not overflow · derived epoch ≤ ceiling · derived epoch ≥ `required_min_epoch_s` when supplied. Never trusted: plausible raw `time()`, a past `settimeofday()`, **Stratum ntime**, an unsynced RTC-like value, a persisted epoch without a fresh sync, SNTP status without a captured anchor.

### 2.3 The anchor (validated against actual ESP-IDF v5.5.3 sources)

- `esp_netif_sntp_init(.start=false)` performs **no networking**; only `esp_netif_sntp_start()` polls.
- The sync callback runs in the **lwIP tcpip thread** and receives the **server-supplied `struct timeval` directly** — the anchor pairs that value with `esp_timer_get_time()` and **never reads the raw system clock**, so a concurrent or later Stratum `settimeofday()` cannot leak into it.
- `sntp_get_sync_status()` is a **destructive read** (COMPLETED auto-resets) — status polling is structurally unusable as a trust signal; only the callback-captured anchor is used.
- lwIP stores the server-name **pointer** → names live in provider-owned stable config storage.
- The callback carries **no context pointer** → a single static real-stack binding slot (claimed only when initialized with the real ops); fake-ops providers never bind, so tests freely create fresh instances to simulate reboots.
- Publication is atomic under a bounded spinlock critical section; readers always get a consistent copy; generation is saturating; a rejected re-anchor (regression, band, overflow) leaves the accepted anchor **byte-identical**; no lock is held across any platform op.

### 2.4 Deadlines and recovery

- **Monotonic deadline** (same boot): arm = `now + duration`, duration ∈ [900 s, 24 h] (µs conversion statically proven overflow-free; addition guarded); expired iff `now ≥ deadline`; ceiling-rounded remaining seconds; **monotonic regression fails SAFE** (reported due). Shaped for the B1 intent `ARM_MONOTONIC_DEADLINE`; not connected.
- **UTC deadline** (cross-reboot): `trusted_epoch + duration`, creatable only from a trusted snapshot (§4).
- **Recovery decision** (pure, for Gate B4): no source snapshot → `RECOVERY_REQUIRED`; no persisted UTC deadline → immediate `FAIL_SAFE_RESTORE` (no waiting; fail-safe restore needs no clock); trusted ≥/< deadline → `RESTORE_DUE` / `RESUME_TARGET_WITH_REMAINING_TIME` (exact remaining); untrusted → `WAIT_FOR_TRUSTED_TIME` strictly inside the bounded window (**default 600 s, hard max 900 s**), then `FAIL_SAFE_RESTORE`; trusted-but-before-verified-start → rejected trust (bounded wait, then fail-safe); corrupt timing input → fail-safe when the source snapshot is intact, recovery otherwise. DNS/NTP unavailability is exactly the bounded untrusted path — it can never yield indefinite target mining (property-tested).

---

## 3. Owner-directed trust-boundary correction (second Phase A pass)

The original UTC-deadline helper trusted the caller-supplied `snapshot.trusted/status` fields. Corrected as follows:

- `pool_time_create_utc_deadline(snap, policy, duration, out)` now **revalidates every field available to pure code**: trusted+TIME_OK · `sync_generation ≥ 1` · second/microsecond field agreement (`trusted_utc_us/10⁶ == trusted_epoch_s`) · `anchor_age ≤ uptime` · sanity band · required-minimum floor · duration bounds · overflow guard (now structurally unreachable for any snapshot passing the earlier checks, kept for totality).
- New preferred **`pool_time_create_utc_deadline_from_clock(clock, policy, duration, out_snap, out)`** — obtains the snapshot itself from the provider-owned accepted anchor; no caller-supplied snapshot exists on that path.
- The dedicated test `deadline-utc: manually inconsistent snapshots are rejected` proves every listed inconsistent combination is rejected with its specific code (wrong status, generation 0, out-of-band epochs, disagreeing views, impossible anchor age, untrusted-with-plausible-epoch, required-minimum mismatch).

**Honest boundary (stated in `pool_time.h` and the tests):** this is **consistency checking and misuse resistance, NOT authentication**. A `PoolTimeSnapshot` is a plain C value struct — no keyed MAC, signature, opaque handle, capability or cryptographic provenance exists — so code already executing inside the firmware can fabricate a fully self-consistent snapshot that no pure check can detect. Snapshots are *intended* to come from `pool_time_snapshot()` (operational provenance: an internal API contract).

---

## 4. Test suite (82 `[pool_time]` tests → QEMU 140 ⇒ 222)

Model/policy/config bounds (band constants, wait-window 0/1/600/900/901, server-name and count bounds, invalid-enum tokens) · trusted predicate (no sync, no-flag, band floor/ceiling boundaries, monotonic regression, required-minimum, exact derivation, overflow, null args) · anchor/provider (first accept gen 1, rejected candidates leave the provider untrusted, forward resync gen++, **generation saturation at UINT32_MAX**, backward/out-of-band resync byte-identical rejection, deinit clears trust, fresh-provider reboot simulation, post-deinit sync ignored) · trusted-now (anchor-instant equality, exact advancement, subsecond carry, nondecreasing reads) · **Stage-13 raw-clock isolation** (huge forward/backward/repeated raw jumps: estimate stays exactly A+X; valid re-anchor without regression; rejected backward re-anchor keeps the original line) · monotonic deadline (min/max arm, bound rejections incl. UINT32_MAX, addition overflow, 1 µs-before/exact/after expiry, regression fail-safe, ceiling-rounding contract, invalid structs) · UTC deadline (trusted-only, from-clock path, duration bounds, untrusted/previous-boot rejection, **manually-inconsistent-snapshot matrix**) · recovery (all decision rows, boundary elapsed values, DNS/NTP sweep never-RESUME, corrupt window/epochs, null inputs, exact remaining across magnitudes) · lifecycle (double init/start/stop/deinit, stop-before-start, pre-init ops, argument totality, platform-op failure → ERROR → deinit recovery, snapshot journey, stop-retains-anchor) · privacy (token charset, planted server marker never reaches anchors/snapshots/tokens) · properties (distinct stable tokens, recovery totality/safety/purity sweep, deadline never-wrap edges, nondecreasing trusted estimate, invalid anchors never trusted, rejected-resync immutability sweep, pure-call input immutability).

---

## 5. Security posture (honest)

- **Operational trust vs cryptographic authenticity:** the trust verdict is a policy decision over an SNTP callback; snapshot validation is consistency checking (§3). Cryptographic authenticity of time values is **absent** on this build and not claimed.
- **Network-trust residual (documented, not hidden):** standard SNTP and DNS as used here are **not cryptographically authenticated** (no NTS, no DNSSEC). The anchor prevents later Stratum `settimeofday()` manipulation; the sanity band and anti-regression checks detect classes of error; they do **not** authenticate the remote NTP server or network path. **Malicious DNS/NTP manipulation remains a residual**, bounded by the sanity band, monotonic-only same-boot timing, the bounded sync window and fail-safe restore. It does not block the local board-601 MVP.
- No password, pool account, wallet, hostname or address field exists in any time model or token; NTP server names live only in the bounded provider config.

---

## 6. Engineering notes & gotchas (recorded for future gates)

1. **`CONFIG_UNITY_ENABLE_64BIT` is default n.** `TEST_ASSERT_EQUAL_UINT64` then fails at runtime with "Unity 64-bit Support Disabled" (27 such failures on the first QEMU run). Fixed in `test-ci/sdkconfig.defaults`; remember the gitignored `test-ci/sdkconfig` masks defaults changes until deleted. `test/` was deliberately left untouched (same posture as the B1 stack fix).
2. **The ESP-IDF sync callback has no context pointer**, forcing a single static binding slot for the real stack; keep fake-ops providers unbound so reboot simulation stays possible.
3. **`sntp_get_sync_status()` destructively resets on read** — never build trust logic on it.
4. **lwIP keeps server-name pointers** — config storage must outlive the running service.
5. `-Wextra`'s `-Wtype-limits` rejects provably-false runtime overflow guards — replace with `_Static_assert` proofs.
6. PowerShell mangles `$`/`\$` inside `docker … bash -c "…"`; run containers from Git Bash with single quotes (`MSYS_NO_PATHCONV=1`).

---

## 7. Validation summary (Phase B, against commit `463417d`)

- Branch/ancestry verified read-only (`463417d` → `e6f360e` → `e72db15`); working tree clean before and after; **no Git write operations**.
- Only the 9 approved files are in the commit; `git diff e72db15..HEAD -- main/` is **empty** (Stratum ntime + all runtime paths untouched); no production caller of any `pool_time` symbol exists.
- Frontend gate, fresh test-ci build, QEMU **222/0/0**, main flag-OFF and flag-ON builds: all green (table in §1).
- Secret/local-path scan clean; `test-ci/CMakeLists.txt` restored to its exact 22-byte tracked form; `report.xml` removed; no release artifacts.
- **No hardware, COM/USB, real NTP servers, live DNS, real BTC/BCH pools, pool accounts, wallets, real credentials, owner LAN, or the private TCH recovery dump were accessed.** No outbound network traffic was attempted by any test or build.

---

## 8. Committed files (9)

| File | Change |
|---|---|
| `components/pool_time/CMakeLists.txt` | new — component registration (`PRIV_REQUIRES esp_timer esp_netif lwip`) |
| `components/pool_time/include/pool_time.h` | new — pure models/predicate/deadlines/recovery/tokens + trust-boundary documentation |
| `components/pool_time/pool_time.c` | new — pure implementation |
| `components/pool_time/include/pool_time_sntp.h` | new — provider config/lifecycle/ops contract (ESP-IDF-free header) |
| `components/pool_time/pool_time_sntp.c` | new — adapter: anchor ownership + real ESP-IDF ops (never invoked in B2) |
| `components/pool_time/test/CMakeLists.txt` | new — test registration (`REQUIRES cmock pool_time pool_session`) |
| `components/pool_time/test/test_pool_time.c` | new — 82 Unity tests |
| `test/CMakeLists.txt` | `TEST_COMPONENTS` += `pool_time` |
| `test-ci/sdkconfig.defaults` | +`CONFIG_UNITY_ENABLE_64BIT=y` (see §6.1) |

---

## 9. Gate B3 prerequisites

1. Branch from this committed B2 head (`463417d`).
2. **B3 = persistent record store** (audit §11/§17): dual A/B `nvs_set_blob` slots + CRC32 + monotonic generation in dedicated namespace `"nx_tps"`, single-entry `active_slot` write as the power-safe commit point, an explicit versioned serializer (never raw struct memory), corrupt-record ⇒ **no pool mutation** ⇒ `RECOVERY_REQUIRED`. CRC is crash-consistency, **not** authenticity (T13b residual stands).
3. **Cross-reboot minimum-epoch prerequisite (exact, from the B2 trust-boundary pass):** B3 must persist a **monotonically-advancing "latest accepted trusted epoch"** (e.g. refreshed on the TARGET_ACTIVE heartbeat) plus **bounded reboot/recovery counters**; Gate B4 must supply that persisted floor — not merely `verified_start_epoch` — as `required_min_epoch_s`. Rationale: a floor of only the session start would let an erroneous or malicious time source repeatedly report near-start values after reboots and extend the apparent remaining duration. The B2 contracts already accept any such stronger floor; B2 itself persists nothing.
4. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; the B1 restore-obligation invariants; Stratum ntime untrusted for deadlines; QEMU ≥ 222 green; frontend 1052.
