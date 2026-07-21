# NeuralAxe OS — Phase 2M.1A

## Device-Side Timed Pool Sessions — Architecture, Persistence & Security Audit

**Product:** NeuralAxe OS 0.1.0-dev
**Primary target:** Bitaxe Gamma / board 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-audit`
**Baseline release:** v2.14.2-43-gd333dc4 (Phase 2M.0), audit branch HEAD v2.14.2-44-g452fde6
**Phase type:** Architecture & security audit only — **no firmware, frontend or test source changed.**

> **Evidence legend.** Each finding is tagged:
> **[FACT]** observed directly in this repository · **[IDF]** documented ESP-IDF framework guarantee ·
> **[INFER]** reasoned conclusion from facts · **[REC]** recommendation · **[OPEN]** unresolved question.
> Citations are repository-relative (`path — symbol`). No owner-local absolute paths are used.

---

## 1. Executive Verdict

**Verdict for Phase 2M.1B: CONDITIONAL GO.**

Unattended, device-side timed pool sessions are **architecturally feasible on the current firmware** and can be built on the validated Phase 2M / 2M.0 pool-switch behavior without weakening it. The concurrency model (a single `protocol_coordinator` owner), the layered verification telemetry, and the existing PATCH+restart apply path are all reusable.

However, two hard constraints shape the MVP and are the source of the *conditional*:

1. **No secure storage at rest.** NVS encryption, flash encryption and Secure Boot are **all disabled** in the validated board-601 build, and the firmware can read the pool password back from plaintext NVS. A persisted second (restore) secret would sit in plaintext flash. **Therefore different-password unattended sessions are a security blocker and must be rejected in the MVP.** The MVP must be **"Keep current password" only** (Policy 1).

2. **No trustworthy independent clock — a dedicated SNTP provider must be added in 2M.1B.** There is **no SNTP client** today; the only wall-clock source is the pool-supplied Stratum `ntime`, which is **pool-controlled and must remain explicitly untrusted for session deadlines**, and the RTC clock does not survive full power removal. **Phase 2M.1B must introduce a dedicated SNTP-based trusted-time provider** built on the existing ESP-IDF networking stack (`esp_netif_sntp_*` / `esp_sntp`). A timed deadline may be **persisted only once that SNTP-backed clock has reached the defined trusted state**; the active session is governed by **monotonic time within the current boot**; after a reboot the scheduler **waits a bounded period for trusted SNTP time** and, if it cannot be acquired, applies a **bounded fail-safe restore to the captured source configuration** — it never silently keeps mining the temporary target indefinitely on untrusted time. Pool-supplied `ntime` can never satisfy this requirement.

With those two constraints enforced, the remaining design (persistent A/B session record, firmware-owned state machine, boot-recovery table, bounded verification, single-owner concurrency) is safe and reviewable. Detailed conditions for GO are in §16 and the companion Security report.

---

## 2. Scope, Method & Guardrails

This phase answers the 25 primary audit questions against exact source, then designs (but does **not** implement) the Phase 2M.1B firmware.

**What was inspected (read-only):** pool config models, NVS persistence, HTTP pool endpoints, restart/OTA handlers, Stratum V1 startup/reconnect, protocol coordinator, mining telemetry, system startup, Wi-Fi/time code, partition table, `sdkconfig`, encryption/secure-boot config, native/QEMU test structure, and the frontend Pool Strategy contracts the firmware must later serve.

**Guardrails honored:** no Git write operations; no hardware, COM/USB, real BTC/BCH pools, pool accounts, wallet addresses, real credentials, owner LAN, or the private TCH recovery dump were accessed. No firmware was uploaded. No release artifacts were generated. Only the three documentation files in §16 were added.

---

## 3. Current Architecture Map

### 3.1 Pool configuration storage — individual NVS keys

**[FACT]** Pool settings are stored as **individual, typed NVS keys** in a single namespace, not as a struct or blob.

`main/nvs_config.c` — `#define NVS_CONFIG_NAMESPACE "main"`; the `settings[]` table (nvs_config.c:51–136) maps each logical key to one NVS key name:

| Logical setting | NVS key | Type | Notes |
|---|---|---|---|
| Primary URL | `stratumurl` | STR | |
| Primary port | `stratumport` | U16 | |
| Primary user/worker | `stratumuser` | STR | |
| Primary password | `stratumpass` | STR | **plaintext** |
| Primary TLS / cert | `stratumtls` / `stratumcert` | U16 / STR | |
| Primary protocol | `stratumprot` | STR | `SV1`/`SV2` |
| Fallback URL/port/user/pass | `fbstratumurl` / `fbstratumport` / `fbstratumuser` / `fbstratumpass` | STR/U16/STR/STR | |
| Fallback TLS/cert/proto/diff… | `fbstratum*` | — | mirror of primary |
| Use fallback flag | `usefbstartum` | BOOL | drives `is_using_fallback` |

**[FACT]** There is **no `nvs_set_blob` / `nvs_get_blob`** anywhere in `main/` (verified by search). Every setting is a discrete key/value.

### 3.2 Component ownership

| Concern | Owner (file — symbol) |
|---|---|
| Persistence (get/set/commit) | `main/nvs_config.c` — `nvs_config_set_*`, `nvs_task`, `nvs_config_init` |
| In-RAM working copy of pool config | `main/system.c` — `SYSTEM_init_system` loads `SYSTEM_MODULE.pool_url/port/user/pass/…` (system.c:62–102) |
| Host / port / account / password validation + write | `main/http_server/http_server.c` — `check_settings_and_update` (validation), `PATCH_update_settings` (http_server.c:711) |
| Restart | `main/http_server/http_server.c` — `POST_restart` → `esp_restart()` (http_server.c:810–843) |
| Pool restart / reconnect / failover / recovery | `main/tasks/protocol_coordinator.c` — `protocol_coordinator_task` |
| Stratum V1 connect / subscribe / authorize / recv | `main/tasks/stratum_v1_task.c` — `stratum_v1_task` |
| Stratum V2 | `main/tasks/stratum_v2_task.c` |
| Mining telemetry | `main/system.c` — `SYSTEM_notify_accepted_share`, `work_received`, block state |

### 3.3 Write path & commit semantics

**[FACT]** All setters are asynchronous. `nvs_config_set_string/_u16/_bool/…` (nvs_config.c:470–620) build a `ConfigUpdate` and `xQueueSend` it to `nvs_save_queue`. A single worker task `nvs_task` (nvs_config.c:228–304) dequeues each update and performs `nvs_set_*(handle,…)` **followed by `nvs_commit(handle)` per item** (nvs_config.c:291–296). An in-RAM cache (`setting->value`, guarded by `nvs_cache_mutex`) is updated first so getters never block on flash.

**[INFER]** Because every field commits independently, **a multi-field pool change is a *sequence* of independent atomic commits, not one transaction.** A power loss partway leaves a mix of old and new fields — e.g. new URL + old password. This is the central atomicity finding (see §5).

### 3.4 System startup order

**[FACT]** `main/main.c` — `app_main` runs, in order: I2C → ASIC reset-low → ADC → **`nvs_config_init`** → SSID load → `device_config_init` → `self_test_init` → **`SYSTEM_init_system`** (loads pool config into RAM) → scoreboard → **`wifi_init`** → `SYSTEM_init_peripherals` → power-management + fan tasks → **`start_rest_server`** (HTTP) → versions → BAP → **busy-wait until `SYSTEM_MODULE.is_connected`** → `queue_init` → `asic_initialize` → **`create_jobs_task`/protocol coordinator (Stratum + mining)** (main.c:34–180).

Ordering summary: **NVS → pool-config load → Wi-Fi → HTTP server → (wait for link) → Stratum/mining.** The HTTP API is up before mining; mining requires Wi-Fi association first.

---

## 4. Persistence & Atomicity Findings

**Q1 — Where are pool settings stored?** `[FACT]` NVS namespace `"main"`, individual keys (§3.1).

**Q3 — Individual keys, struct or blob?** `[FACT]` Individual typed keys. No struct, no blob.

**Q4 — Which writes require commit?** `[FACT]` Every setter, indirectly: `nvs_task` calls `nvs_commit(handle)` after each successful `nvs_set_*` (nvs_config.c:291–296). Boot-time migrations also call `nvs_set_str`/`nvs_erase_key` but rely on the same handle; commit occurs on the next `nvs_task` write or is implicit at `nvs_flash` teardown.

**Q5 — Atomicity / power-loss guarantees of the current mechanism.**
- `[IDF]` A **single** NVS entry write + `nvs_commit` is power-safe: NVS is log-structured with per-entry CRC and page state, so an interrupted write discards the partial value and the previous value remains readable (ESP-IDF `nvs_flash` component). A multi-chunk `nvs_set_blob` is likewise replace-then-index so the old blob survives an interrupted write.
- `[FACT]` The firmware commits **per field**, so the *group* of pool fields is **not** transactional.
- `[INFER]` Guarantee that actually exists today: *each individual field* is power-safe; *the pool identity as a whole* is **not**.

**Q6 — Can one pool config be captured/restored as one atomic identity today?** `[FACT/INFER]` **No.** A pool identity spans ~10 keys, each committed separately. Restoring it is ~10 independent writes; a crash mid-restore yields a partial identity. This is precisely why a timed session must not restore by re-issuing individual `nvs_config_set_*` calls (see §6 record design).

---

## 5. Encryption, Flash & Secure-Boot Findings (validated board-601 build)

All values read from the tracked, validated `sdkconfig` and `partitions.csv`.

**Q7 — NVS encryption:**
`[FACT]` **Not enabled.** `# CONFIG_NVS_ENCRYPTION is not set` in `sdkconfig`. `partitions.csv` contains **no `nvs_keys` partition** (required to provision NVS encryption). The only NVS partition is `nvs, data, nvs, 0x9000, 0x6000`. NVS encryption is *SoC-supported but neither compiled, configured, provisioned, nor enabled.*

**Q8 — Flash encryption:**
`[FACT]` **Not enabled.** `# CONFIG_SECURE_FLASH_ENC_ENABLED is not set` and `# CONFIG_FLASH_ENCRYPTION_ENABLED is not set`. The `CONFIG_SOC_FLASH_ENC_SUPPORTED=y` / `CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES*` lines are **hardware-capability** flags of the ESP32-S3, not enabled features.

**Q9 — Secure Boot:**
`[FACT]` **Not enabled.** `# CONFIG_SECURE_BOOT is not set`. `CONFIG_SECURE_BOOT_V2_PREFERRED=y` and `CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED=y` are *default preferences / SoC capabilities* that only take effect if Secure Boot is turned on; it is not.

**Q10 — How are passwords stored?** `[FACT]` **Plaintext**, in two places: (a) NVS key `stratumpass` / `fbstratumpass` (plaintext flash); (b) RAM: `SYSTEM_MODULE.pool_pass` / `fallback_pool_pass`, loaded at boot (system.c:83–84).

**Q11 — Can firmware read back the stored password?** `[FACT]` **Yes.** `nvs_config_get_string(NVS_CONFIG_STRATUM_PASS)` returns it; `SYSTEM_init_system` already does this at boot. The "write-only" property is only a **UI/GET convention** — GET `/api/system/info` omits the password (§8) but the firmware holds it in cleartext.

**Coredump note.** `[FACT]` A 64 KB `coredump` partition exists, but `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y` — coredumps are **not** written to flash, reducing (not eliminating) the "secrets in crash dump" surface.

**Combined implication `[INFER]`:** at rest, anyone with physical flash access can read the pool password. Introducing a *second* persisted secret (a different restore password) **adds a second plaintext secret to flash** without any at-rest protection. This is the decisive input to §2 / Stage 2.

---

## 6. Secret-Storage Decision (Stage 2)

A timed session can involve up to two passwords: the **temporary target** password (used while the session is active) and the **original restore** password (needed to return to the source pool unattended).

| Case | Situation | Finding |
|---|---|---|
| **A** | Source pw == target pw | `[FACT]` The single persisted `stratumpass` already satisfies both. **No second secret needed.** Safe today. |
| **B** | Target replaces the pw (source ≠ target) | `[INFER]` Unattended restore would require persisting the original secret separately. No third NVS slot exists; adding one stores a **second plaintext secret** (see §5). **Not safe at rest.** |
| **C** | Encrypted persistent storage available & enabled | `[FACT]` **Not the case here** — NVS/flash encryption disabled (§5). Would be acceptable *only if proven enabled*. |
| **D** | Encrypted storage unavailable/disabled | `[FACT]` **This is the current state.** Plaintext dual-secret persistence must not be presented as secure. |

**Decision — Policy 1 (MVP): "Keep current password" only.**
`[REC]` Timed sessions in Phase 2M.1B are allowed **only** when the target profile uses `passwordMode: keep` (Case A semantics: the persisted password is unchanged across the session). Different-password sessions are **rejected at the API** with a stable error code until encrypted persistent storage is *proven enabled* (Policy 2 becomes available only then). Policy 3 (browser-tethered different-password) is explicitly **not** an unattended session and must never be labelled device-autonomous.

Consequences enforced by design:
- The scheduler **never persists a password** in the session record; it relies on the already-persisted `stratumpass` being correct for both target (same profile family via keep) and restore.
- `[OPEN]` Keep-current with a *different target pool host* but the *same password string* is Case A and is allowed. The profile model must guarantee `passwordMode: keep` means the persisted secret is valid for the target host — the frontend Pool Strategy already treats keep-current this way (`pool-strategy.component.ts` / `pool-profile.ts`), but the firmware must validate at create time that no password write is requested.

Full secret lifecycle, zeroization and residual risk: see the companion **Security** report.

---

## 7. Trusted-Time Model (Stage 3)

**Q13/Q14 — Time sources present.**
- `[FACT]` **No SNTP.** `esp_sntp.h` is `#include`d in `main/tasks/stratum_v1_task.c:11` but **no `sntp_init` / `esp_netif_sntp_init` / `sntp_setservername` exists anywhere in `main/`** (verified by search).
- `[FACT]` The **only** wall-clock source is pool-supplied Stratum `ntime`: `SYSTEM_notify_new_ntime` (system.c:329–343) calls `settimeofday(&tv,NULL)` at most hourly, `tv.tv_sec = ntime` from `MINING_NOTIFY` (stratum_v1_task.c:333).
- `[FACT]` `esp_timer_get_time()` (monotonic microseconds since boot) is used for `SYSTEM_MODULE.start_time` (system.c:52) and coordinator timing. Resets to 0 every boot.
- `[FACT]` FreeRTOS ticks at `CONFIG_FREERTOS_HZ=1000`. No RTC-memory time persistence, no persisted boot counter for time.

**Q17 — Can mining begin before wall-clock is trustworthy?** `[FACT/INFER]` **Yes.** Mining starts as soon as Wi-Fi + ASIC are up; the wall clock is only set *after* the first `MINING_NOTIFY`. So mining **precedes** any wall-clock value, and the first value comes **from the pool** the session is mining.

**[INFER] Time-trust properties that matter for a deadline:**
1. Wall clock is **pool-controlled** (a malicious/compromised pool can set `ntime` forward to force early restore, or backward to extend a temporary session). It is *not* an authenticated time source.
2. Wall clock is **absent** immediately after any reset until the first job arrives.
3. `[IDF]` The ESP32 RTC keeps `settimeofday` time across a *software* reset / brownout that retains RTC power, but **not** across full power removal — after a cold power-on the clock restarts near epoch 0.

**[REC] Time model — Phase 2M.1B must add a dedicated SNTP trusted-time provider.**

*Provider mandate (hard prerequisite).* Phase 2M.1B **must introduce a dedicated SNTP client** as the trusted-time source, built on the **existing ESP-IDF networking stack** (`esp_netif_sntp_init` / `esp_sntp_*`; LWIP is already present). The session-deadline logic depends on it. **Pool-supplied Stratum `ntime` is explicitly NOT a trusted-time source for session deadlines** and must never be used to establish, extend, or expire a deadline. `ntime` may keep driving the cosmetic `settimeofday` clock exactly as today, but the scheduler ignores it for all timing decisions.

*Trusted-time predicate (exact).* The SNTP-backed clock is **trusted** only when ALL of: (a) at least one successful SNTP sync has completed **this boot** (`esp_sntp` reports `SNTP_SYNC_STATUS_COMPLETED`); (b) the resulting epoch is within a fixed sanity band (≥ a compiled floor such as `2025-01-01T00:00Z`, ≤ a far-future ceiling); (c) for a cross-reboot deadline check, the value is ≥ the persisted `verified_start_epoch`. A clock that is merely *set* — from `ntime`, or an unsynced RTC — is **untrusted** and cannot satisfy the predicate.

*Establishing the deadline.* At `TARGET_ACTIVE`, the deadline is defined **primarily** as *"monotonic runtime since `TARGET_ACTIVE` = requested_duration"* for the current boot (`esp_timer_get_time()`), which is immune to any clock manipulation while powered. **An absolute `deadline_epoch` is persisted only when the SNTP-backed clock is already trusted at that moment** (`deadline_epoch_valid = 1`); otherwise the record keeps `deadline_epoch_valid = 0` and relies on monotonic-within-boot plus the reboot fail-safe.

*Within a boot.* Monotonic `esp_timer_get_time()` governs session end (elapsed ≥ duration → `RESTORE_DUE`). Forward/backward clock jumps cannot shorten or extend the powered session.

*After a reboot* (the monotonic base is gone): the scheduler **starts SNTP and waits a bounded synchronization window — proposed default 10 minutes, hard maximum 15 — for trusted time.** During the wait it may resume the target pool to keep mining/telemetry alive **only while any known deadline has not yet passed.** Then: (i) if the trusted clock shows the deadline (`deadline_epoch`, or `verified_start_epoch + duration`) has passed → **restore immediately**; (ii) if trusted time is acquired and the deadline is still in the future → resume with a fresh monotonic base for the remaining duration; (iii) **if trusted time cannot be acquired within the bounded window → perform a fail-safe restore to the captured source configuration.** The scheduler must never silently continue mining the temporary target indefinitely on untrusted time.

*DNS / NTP-server unavailable.* SNTP name-resolution failure or unreachable servers are treated **identically to "time untrusted"**: the bounded window still applies and, on its expiry, the scheduler **fail-safe restores** rather than waiting forever. The provider should configure **multiple NTP servers** (and DHCP-provided NTP where offered); repeated sync failures feed the reboot-loop / `retry_count` guard so a network outage cannot become a reboot loop. **Fail-safe restore needs no clock** — it is driven purely by the persisted record state, so it still works with DNS and every NTP server down.

*Overflow / limits.* Duration bounded 15 min – 24 h (§16); `deadline_epoch = verified_start_epoch + duration_s` computed in 64-bit and validated against the bound before persistence. Timezone-independent (UTC epoch only).

Boot-recovery rows, maximum wait, and exact status wording: §13 and the State-Machine report.

---

## 8. HTTP API, Auth & Privacy Findings

**Q24 — API patterns to follow.** `[FACT]` REST under `/api/system/*`, registered in `start_rest_server` (http_server.c:1297+). PATCH `/api/system` for settings; POST `/api/system/restart|pause|resume|OTA` for actions; GET `/api/system/info` for sanitized state. cJSON for bodies. Every handler calls `is_network_allowed(req)` then `set_cors_headers(req)`.

**Auth model `[FACT]`:** `is_network_allowed` (http_server.c:279–321) permits a request **only** if the peer IP **and** the `Origin`-header IP are both in a private range (or AP mode). There is **no password, token, or session authentication.** CORS is `Access-Control-Allow-Origin: *` (http_server.c:360). `[INFER]` This is LAN-scoped trust with weak origin-IP CSRF mitigation, not authenticated access — a material input to the threat model.

**GET privacy `[FACT]`:** `system_api_json.c` (155–219) serializes `stratumURL/Port/User/Cert/TLS`, fallback equivalents, protocol and `isUsingFallbackStratum`, but **never** `stratumPassword` / `fallbackStratumPassword`. The password is genuinely absent from GET. A timed-session GET must preserve this: expose state/timestamps/remaining/verification, **never** a secret.

---

## 9. Stratum Verification Findings & Contract (Stage 6)

### 9.1 Signals the firmware already produces

**Q18/Q19 — Available proof signals**, all readable from `GLOBAL_STATE`/`SYSTEM_MODULE` without HTTP coupling:

| # | Proof | Source |
|---|---|---|
| 1 | Pool config applied | scheduler wrote NVS + triggered restart |
| 2 | Restart/reconnect initiated | `esp_restart()` / coordinator task start |
| 3 | TCP connection established | `esp_transport_connect()==ESP_OK` (stratum_v1_task.c:254) |
| 4 | Stratum subscribe sent | `STRATUM_V1_subscribe` (stratum_v1_task.c:291) |
| 5 | Authorization accepted | `STRATUM_RESULT_SETUP` + `response_success` → `protocol_coordinator_notify_success()` (stratum_v1_task.c:389–394) |
| 6 | Valid job received | `MINING_NOTIFY` → `SYSTEM_MODULE.work_received++` (stratum_v1_task.c:331–332) |
| 7 | Hashing resumed | positive hashrate (`hashrate_monitor_task`) + `!mining_paused` |
| 8 | Accepted share observed | `SYSTEM_notify_accepted_share` → `shares_accepted` (stratum_v1_task.c:381) |
| 9 | Fallback active | `SYSTEM_MODULE.is_using_fallback` |
| 10 | Restore host matched | active `pool_url` (per `is_using_fallback`) == source host |
| 11 | Restore mining resumed | #7 + #10 |

**Q19 — Consumable without dangerous UI coupling?** `[FACT/INFER]` Yes. These are plain fields in shared state; the frontend `pool-verify.ts` already derives the layered verdict from the GET projection of exactly these fields. A firmware scheduler task can read the same fields (or subscribe to `protocol_coordinator` events) directly, with no dependency on an HTTP client being connected.

### 9.2 Frontend contract to mirror device-side

**[FACT]** `main/http_server/axe-os/src/app/components/pool-strategy/pool-verify.ts` defines the authoritative layered model the firmware must reproduce:

`settingsApplied → poolConnected (fresh telemetry) → miningResumed (unpaused, hashrate>0) → targetHostVerified (active host == target primary|fallback) → shareActivityObserved (optional)`.

Basic success (`connectedAndMining`) = **connected + mining + target host, NOT share-gated** (solo cadence varies). Timeouts: `RECONNECT_TIMEOUT_MS = 90 s`, `VERIFY_TIMEOUT_MS = 45 s`.

### 9.3 Verification contract for timed sessions `[REC]`

| Milestone | Required evidence | Bounded by |
|---|---|---|
| **Target applied (success)** | #1–#6 (config applied, reconnected, authorized, first job) | reconnect ≤ 90 s |
| **Target session START (timer begins)** | **connected + mining resumed + target host verified** (#3,#5,#6,#7,#10-as-target) — the frontend `connectedAndMining` bar. **NOT** share-gated. | verify ≤ 45 s after reconnect |
| **Restore success (exact)** | connected + mining + **source host** verified + (Case A) password unchanged | reconnect ≤ 90 s, verify ≤ 45 s |
| **Restore success (operational)** | connected + mining + source host verified, but on **fallback** rather than primary | same |
| **Partial restore** | source config applied + reconnected, host not yet matched at timeout | terminal after retries |
| **Failed restore** | no reconnect / wrong host after retries | terminal, needs operator |

**When does the requested duration start? `[REC]` After bounded mining-resumed verification** (the `TARGET_ACTIVE` milestone), **not** at request acceptance and **not** at first accepted share. Rationale: starting at acceptance would silently burn the requested minutes during an unbounded connect/verify; requiring an accepted share could stall forever on solo cadence (Invariant 4). Verification uses **bounded levels** — if `TARGET_ACTIVE` is not reached within the reconnect+verify budget, the session goes to `TARGET_FAILED` and auto-restores; it never sits in verification indefinitely.

---

## 10. Concurrency & Operation-Ownership Findings (Stage 7)

**Q22 — Locks / queues / tasks protecting pool state.** `[FACT]`
- `GLOBAL_STATE->stratum_mux` (portMUX) — UID / transport handle.
- `valid_jobs_lock` (pthread mutex) — job set.
- `nvs_cache_mutex` (semaphore) + `nvs_save_queue` — NVS cache & write serialization.
- `protocol_coordinator` `s_event_queue` (8-deep) + single `protocol_coordinator_task` — **sole owner** of starting/stopping V1/V2 tasks and of `is_using_fallback` / `stratum_protocol` / pause state (protocol_coordinator.c).
- `stratum_queue` (work queue) — jobs.

**Q20/Q21 — Changing pool config while Stratum runs.** `[FACT/INFER]` Today, `SYSTEM_MODULE.pool_*` is loaded **once** at boot; PATCH writes NVS only and does **not** refresh the RAM snapshot. To take effect, a **full `esp_restart()`** is required (which the browser triggers via POST `/api/system/restart`). The coordinator *can* stop/start stratum tasks without a full reboot, but it reads the same boot snapshot, so it will not pick up new pool config without a restart or a new "reload SYSTEM_MODULE from NVS" path (which does not exist).

**Q23 — Operations that can conflict with a timed session.** `[FACT]` manual PATCH `/api/system`, browser Pool Strategy switch (PATCH+restart), POST `/api/system/restart`, OTA (`POST_OTA_update` → `esp_restart`), thermal/emergency restart, watchdog reset, protocol-coordinator failover. **No user-facing factory-reset endpoint exists** (only boot-time `nvs_flash_erase()` on NVS page/version errors, nvs_config.c:308–312, and migration `nvs_erase_key`).

**[REC] Ownership model: a single firmware-owned `pool_session_scheduler` task + persisted operation lease, modeled on `protocol_coordinator`.**
- Exactly **one pool-operation owner**. The scheduler holds a **persisted lease** (a flag in the session record) whenever a session is non-terminal. Reboot-safe: the lease lives in NVS, not RAM.
- **Manual pool PATCH policy (MVP) `[REC]`:** while a session lease is held, `PATCH /api/system` fields that alter *pool identity* (url/port/user/pass/tls/cert/protocol/useFallback, primary or fallback) are **rejected with a deterministic 409 conflict** and a stable error code. Non-pool fields (fan curve, display, difficulty-only, theme) remain allowed. The operator's route to change pools is **Restore Now / Cancel first**. This preserves Invariant 9 (manual changes cannot silently destroy the restore contract) without the ambiguity of "treat PATCH as implicit cancel."
- **OTA policy (MVP) `[REC]`:** `POST_OTA_update` / `POST_WWW_update` **rejected with 409** while a session lease is held (except after Restore Now). Rationale: OTA reboots and may change the record schema; simplest safe rule is *restore first*. (§13.)
- **Browser Pool Strategy coexistence `[REC]`:** when a firmware session is active, the frontend must **not** run its own rollback/restore timer; it becomes a **read-only monitor** of the firmware session (GET), and its manual switch UI is disabled with an explanatory state. The firmware is the single authority.
- No secret ever appears in lock/lease diagnostics; the lease stores only state + IDs.

---

## 11. Recommended Persistent Session Record (Stage 4)

**[REC] Structure: dual A/B NVS blob slots + generation counter + CRC, with a tiny active-slot pointer.** This design depends only on the *single-entry* commit atomicity that is proven-safe (§5, `[IDF]`), and does **not** rely on unproven multi-key or intra-blob atomicity.

- Two blob keys in a dedicated namespace `"nx_tps"` (isolated from `"main"`): `rec_a`, `rec_b`.
- A small `u16` key `active_slot` (0/1) written **last**, as the commit point (single-entry, power-safe).
- Each slot is a fixed-size, versioned, CRC-covered struct written whole via `nvs_set_blob` (single blob replace, `[IDF]` old value survives an interrupted write) then `nvs_commit`.

**Slot schema (fixed, bounded — no unbounded strings):**

| Field | Type | Purpose |
|---|---|---|
| `schema_version` | u16 | migration gate |
| `session_id` | u32 | unique per session |
| `generation` | u32 | monotonic; newest valid slot wins |
| `state` | u8 | persistent FSM state (§ State-Machine report) |
| `password_mode` | u8 | `KEEP` only in MVP (rejects != KEEP) |
| `restore_secret_present` | u8 | always 0 in MVP (Policy 1) |
| `requested_duration_s` | u32 | validated 900…86400 |
| `verified_start_epoch` | u64 | wall clock at `TARGET_ACTIVE` (0 = unknown) |
| `deadline_epoch` | u64 | `verified_start_epoch + duration` (secondary check) |
| `deadline_epoch_valid` | u8 | 1 only if wall clock existed at start |
| `monotonic_start_us` | u64 | `esp_timer_get_time()` at `TARGET_ACTIVE` (boot-scoped) |
| `boot_id` | u32 | random per boot; detects reboot vs same-boot |
| `source_chain` / `target_chain` | u8 | explicit chain label (never hostname-inferred) |
| `source_primary` / `source_fallback` | pool-identity (host[64], port, user[64], tls, proto, useFallback) | **non-secret** restore snapshot |
| `target_primary` / `target_fallback` | pool-identity | non-secret target snapshot (for host verify) |
| `source_profile_id` / `target_profile_id` | char[24] | optional labels |
| `target_verified` / `restore_verified` | u8 bitfields | milestone flags |
| `last_failure_code` | u16 | stable machine code |
| `retry_count` | u8 | bounded |
| `created_epoch` / `last_transition_epoch` | u64 | history timestamps |
| `crc32` | u32 | covers all preceding bytes |

**Do NOT store:** full history, raw telemetry, logs, browser state, wallet data beyond the Stratum user already in config, duplicate secrets, raw HTTP bodies, unbounded strings. **No password field exists in the record at all** (Policy 1).

**Write/commit protocol for a transition:** (1) build new slot image with `generation = max(gen)+1`, updated `state`, recompute `crc32`; (2) `nvs_set_blob` into the **inactive** slot + `nvs_commit`; (3) write `active_slot` to the just-written slot + `nvs_commit` — **this single-entry write is the commit point.** A crash before step 3 leaves the old active slot intact; a crash during step 2 leaves the inactive slot possibly corrupt but the active slot untouched. On load, pick the slot that is (a) CRC-valid **and** (b) has the higher `generation`; if `active_slot` points at a CRC-invalid slot, fall back to the other valid slot.

**What the record guarantees — and what it does NOT `[INFER]`.** The dual A/B slots, `generation` counter and `crc32` provide **crash-consistency only**. The `crc32` is an **accidental-corruption / error-detection check — NOT a cryptographic authenticity code (MAC/signature)**: any attacker who can write flash can recompute a valid CRC, so it gives zero protection against deliberate tampering. Together they defend against torn writes, interrupted commits, accidental corruption, and *accidentally* selecting an older-but-valid slot after an interrupted transition. They do **NOT** provide authenticity, anti-rollback, or anti-forgery. Because this build has **no Secure Boot, no flash encryption, no NVS encryption, and no secure monotonic counter** (§5), an attacker with **physical/offline flash access** can either **roll the record back to an older internally-valid generation OR forge an entirely arbitrary record** (any state, pool identity, or deadline) with a correct CRC, and the loader will accept it. This **malicious offline record rollback-or-forgery is a documented residual risk**, not a mitigated one (§15, and the Security report threat matrix). It does not block the local board-601 MVP, but it must not be described as "prevented" by generation+CRC.

**Power-loss resilience is defined at every transition point** (before first write, during record write, after record write / before pool mutation, during pool mutation, after target apply, during verification, active, deadline-due, during restore, during restore-verify, after completion/before cleanup) — the exact commit point for each is enumerated in the State-Machine report.

---

## 12. State Machine — Summary (full detail in companion report)

**[REC]** A deterministic, firmware-owned FSM. Persistent states survive reboot (their `state` byte is in the record); ephemeral states are recomputed within a boot.

`IDLE → PREPARING → TARGET_SNAPSHOT_COMMITTED → APPLYING_TARGET → RESTARTING_FOR_TARGET → VERIFYING_TARGET → TARGET_ACTIVE → RESTORE_DUE → APPLYING_RESTORE → RESTARTING_FOR_RESTORE → VERIFYING_RESTORE → COMPLETE`

Off-path terminals/branches: `TARGET_FAILED`, `RESTORE_FAILED`, `INTERRUPTED`, `RECOVERY_REQUIRED`, `CANCELLED`.

- **Persistent** (in record): `TARGET_SNAPSHOT_COMMITTED`, `APPLYING_TARGET`, `TARGET_ACTIVE`, `RESTORE_DUE`, `APPLYING_RESTORE`, `COMPLETE`, `TARGET_FAILED`, `RESTORE_FAILED`, `RECOVERY_REQUIRED`.
- **Ephemeral** (recomputed each boot from persistent state + telemetry): `PREPARING`, `RESTARTING_FOR_TARGET`, `VERIFYING_TARGET`, `RESTARTING_FOR_RESTORE`, `VERIFYING_RESTORE`.

**The source snapshot is committed (`TARGET_SNAPSHOT_COMMITTED`) *before* any pool mutation** — Invariant 1 & 2. The timer starts only on `TARGET_ACTIVE` — Invariants 3 & 4. Expiry always drives toward restore — Invariant 5. Every persistent state has a defined reboot action (§13). Only one active session (single record). Every externally-triggered transition is idempotent via `session_id` + `generation`.

---

## 13. Boot-Recovery Contract — Summary (full table in State-Machine report)

At boot, **after `nvs_config_init` and before the coordinator starts mining**, the scheduler loads the record and decides which pool config may start. Safety priority order: (1) never lose the restore identity; (2) never silently mine the temporary target indefinitely; (3) never apply a corrupt/unverified config; (4) never claim restore without evidence; (5) preserve operator recovery.

| Record state at boot | Pool allowed to start | Stratum? | Restore attempted? | Terminal / next |
|---|---|---|---|---|
| No record | normal config (`main`) | yes | n/a | IDLE |
| `COMPLETE` (awaiting ack) | normal config | yes | no | stays COMPLETE until acked, then cleanup |
| `TARGET_SNAPSHOT_COMMITTED` (target not applied) | **source** (already active) | yes | no | resume PREPARING or cancel to IDLE — source never mutated |
| `APPLYING_TARGET` (mutation maybe partial) | **re-derive from record**: reapply *source* (safe rollback) | after reconcile | yes→source | INTERRUPTED→auto-restore |
| `TARGET_ACTIVE`, before deadline (monotonic lost) | **target** | yes | no (yet) | re-acquire time; run remaining duration; new `monotonic_start` |
| `TARGET_ACTIVE`, deadline already passed | **target** briefly only to confirm, then restore | yes | **yes** | RESTORE_DUE→restore |
| `RESTORE_DUE` | **source** | yes | yes | APPLYING_RESTORE |
| `APPLYING_RESTORE` / `VERIFYING_RESTORE` | **source** | yes | yes | re-verify; COMPLETE or RESTORE_FAILED |
| `TARGET_FAILED` | **source** (auto-restored) | yes | yes | restore then COMPLETE/RECOVERY_REQUIRED |
| `RESTORE_FAILED` | **source** (best effort) | yes | yes (bounded retries) | RECOVERY_REQUIRED if still failing |
| Corrupt record / bad CRC (both slots) | **normal `main` config** | yes | **no** (never mutate on corruption — Invariant 7) | RECOVERY_REQUIRED flag, operator-visible |
| Unsupported `schema_version` | normal `main` config | yes | no | RECOVERY_REQUIRED |
| Missing restore config in record | normal `main` config | yes | no | RECOVERY_REQUIRED |
| Time untrusted after reboot, deadline unknown | current record pool | yes | **fail-safe restore within bounded window** | prefer restore over indefinite target |

**Reboot-loop guard `[REC]`:** each auto-apply/restart increments `retry_count` in the record *before* the restart; if `retry_count` exceeds a small bound (e.g. 3) for the same transition, the scheduler stops auto-restarting, forces `source` config, and sets `RECOVERY_REQUIRED`. `esp_reset_reason()` (ESP_RST_TASK_WDT / PANIC / BROWNOUT, system_api_json.c:20–38) is used to distinguish clean restarts from crash loops.

---

## 14. API Contract Proposal — Summary (Stage 8)

**[REC]** New REST surface under `/api/system/pool-session`, following existing conventions (`is_network_allowed` + CORS + cJSON), passwords never in any response.

| Method / Route | Purpose | Key request → response |
|---|---|---|
| `POST /api/system/pool-session` | Create timed session | `{targetProfileId, targetChain, durationSeconds, passwordMode:"keep", idempotencyKey}` → `201 {sessionId,state}` / `409` if a session exists / `422` if `passwordMode!="keep"` or duration out of `[900,86400]` |
| `GET /api/system/pool-session` | Current session (sanitized) | → `200 {state, sessionId, sourceChain,targetChain, sourceHost,targetHost (host+user only, no pw), createdAt,startedAt, remainingSeconds, deadline (if valid), targetVerification{connected,mining,host,shares}, restoreVerification{…}, failureCode, recovery}` / `204` none |
| `POST /api/system/pool-session/restore-now` | Restore immediately | → `202 {state:"APPLYING_RESTORE"}` (idempotent) |
| `POST /api/system/pool-session/cancel` | Cancel (pre/post apply) | → `202`; pre-apply → IDLE, post-apply → triggers restore |
| `POST /api/system/pool-session/ack` | Acknowledge terminal result → cleanup | → `200`; only valid on terminal states |
| `POST /api/system/pool-session/recover` | Operator recovery action after RECOVERY_REQUIRED | → `202` |

**Rules `[REC]`:** idempotency via client `idempotencyKey` + server `session_id`/`generation` (duplicate create with same key returns the existing session, not a new one). Duration bounds enforced server-side. **No password field is ever accepted** (create rejects any pool-secret field) or returned. Account/worker follow the existing GET convention (shown; never masked further than today). GET never exposes raw NVS or internal stack errors. Stable machine-readable `failureCode`s are defined separately from user-facing text. Full request/response shapes and error-code table: State-Machine + Security reports.

---

## 15. Failure & Threat Summaries

**Failure matrix (Stage 10) — summary `[REC]`.** Every persistent transition has: detection, bounded retry count + spacing, persisted state, fallback action, frontend status, history result, operator action, and whether auto-restore remains safe. Cross-cutting rules: **bounded retries** (no endless loops), **reboot-loop guard** (§13), **fail-safe restore** on time/verify exhaustion, and **never mutate pools on a corrupt record**. The complete matrix (write-error, commit-error, power-loss-before/after-commit, corrupt blob, CRC mismatch, unsupported schema, restart failure, target unreachable/auth-rejected/no-jobs/no-hash/no-shares, source unreachable/auth-rejected, fallback-instead-of-primary, Wi-Fi/DNS/SNTP-absent, clock jump, reboot loop, watchdog, OTA reboot, manual restart, manual pool change, duplicate command, cleanup failure) is in the State-Machine report.

**Threat model (Stage 11) — blockers `[REC]`.** Assets: active + restore pool identity, passwords, account/worker, deadline, session record, operation ownership, history, API. Full matrix in the Security report. **Phase-2M.1B blockers (all mitigated by design):**
1. **Different-password unattended session at rest** — plaintext second secret. **Blocked** (Policy 1 rejects it).
2. **Permanent temporary-pool mining after clock failure** — mitigated by the **mandatory SNTP trusted-time provider** + monotonic-within-boot timing + bounded fail-safe restore (§7, Invariant 5). Pool `ntime` is never trusted for deadlines. Must be implemented, not optional.
3. **Corrupt-record-triggered pool mutation** — mitigated by "never mutate on corruption" + CRC + dual-slot (Invariant 7).
4. **Accidental / crash generation rollback** (an interrupted transition leaves an older-but-valid slot) — mitigated by monotonic `generation` + CRC; the higher valid generation always wins. **This covers accidental rollback only** (crash-consistency), *not* deliberate offline rollback (see residuals).

**Non-blocking residual risks (honestly separated from crash-consistency):**
- **Malicious offline record rollback or forgery** — an attacker with physical/offline flash access can either replay an **older, internally-valid** record or **forge an arbitrary one** (any state / pool / deadline) with a valid CRC. **The `crc32` is accidental-corruption detection, not authenticity** — it is trivially recomputed by any flash writer; generation+CRC therefore **do not** prevent this. **Not cryptographically preventable on this build** (no Secure Boot, flash encryption, NVS encryption, or secure monotonic counter). Blast radius is bounded by the fixed duration and automatic restore. **Documented, not mitigated;** does not block the local board-601 MVP but must be reported honestly.
- LAN-scoped no-auth API + `Origin:*` CORS (pre-existing product posture).
- Plaintext single pool password at rest (pre-existing).
- Hostname-based chain spoofing — mitigated: chain is explicit from the applied profile, never inferred from hostname (Invariant 14).

---

## 16. MVP Scope Decision (Stage 13)

**[REC] Phase 2M.1B MVP — the safest viable scope:**

| Dimension | MVP decision |
|---|---|
| Boards | **Gamma / 601 / BM1370 only** (Invariant 17) |
| Concurrency | **one session at a time** |
| Profile / chain | explicit profile selection; explicit chain label (never hostname-inferred) |
| Password | **Keep-current only** (Policy 1); different-password **rejected** at API |
| Duration | **15 min – 24 h**; presets 15 m / 1 h / 6 h; custom within bounds; **no recurring / calendar** |
| Timer start | after **bounded mining-resumed verification** (`TARGET_ACTIVE`), not at request, not at first share |
| Target verify | connected + mining + target host (not share-gated), bounded 90 s reconnect / 45 s verify |
| Restore verify | connected + mining + source host; exact vs operational vs partial vs failed wording |
| Expiry | automatic restore at deadline (monotonic within boot) |
| Reboot / power loss | deterministic boot recovery (§13); **immediate restore** if trusted time proves expiry; **fail-safe restore** if trusted time unobtainable within bounded window |
| Retries | bounded per transition + reboot-loop guard |
| OTA | **blocked while active** (except Restore Now) |
| Manual pool change | **rejected (409)** while active; operator must Restore Now / Cancel first |
| Cancel / Restore Now | supported at every state; pre-apply cancel → IDLE, post-apply → restore |
| History / cleanup | terminal result **retained until acknowledged**, then record cleared |
| Schema | versioned record (`schema_version`), dual-slot A/B + CRC |
| No profitability / auto-coin / price / wallet-convert / cloud / fleet-schedule / SupraHex | out of scope |

Every item above was checked against source reality in §§3–13; none contradicts the firmware.

---

## 17. Implementation Decomposition (Stage 14)

**[REC]** Phase 2M.1B as **reviewable commit gates** — never the whole scheduler in one change. Each gate lists expected files, tests, the safety invariant it establishes, rollback, and hardware-validation status.

| Gate | Scope | Files (expected) | Tests | Invariant | HW |
|---|---|---|---|---|---|
| B1 | Pure session models + FSM transition table (no I/O) | new `components/pool_session/` (`pool_session_types.h/.c`, `pool_session_fsm.c`) | native unit (test-ci) | 1–5, 8 | deferred |
| B2 | Abstract clock/time provider (monotonic + wall, injectable) | `pool_session_clock.h/.c` | native fake-clock | 4,5,6 | deferred |
| B3 | Persistent record store: dual-slot A/B + CRC + generation | `pool_session_store.c` | native + QEMU NVS | 6,7 | deferred |
| B4 | Boot-recovery decision (pure, table-driven) | `pool_session_recovery.c` | native (all rows) | 1,2,6,7 | deferred |
| B5 | Operation-ownership lease + coordinator integration | `protocol_coordinator.c` hooks, `pool_session_task.c` | native + QEMU | 8,9 | deferred |
| B6 | Scheduler task: apply target, verify, run timer | `pool_session_task.c`, reuse `pool-verify` logic | native + QEMU mocked stratum | 3,4,10 | deferred |
| B7 | Deadline handling + restore apply/verify | `pool_session_task.c` | native + QEMU | 5,10,11 | deferred |
| B8 | API handlers (create/get/restore/cancel/ack/recover) + sanitized status + error codes | `http_server.c`, `system_api_json.c`, `openapi.yaml` | native + frontend contract | 12,13 | deferred |
| B9 | Frontend contract stubs / read-only monitor boundary (2M.1C hand-off) | `axe-os` generated + pool-strategy monitor | Karma | 14 | deferred |
| B10 | Release + real-device validation | docs + validation run | full gate + QEMU + real Gamma plan (§18) | all | **required** |

Rollback for each gate: additive, feature-flag-gated (`CONFIG_NX_TIMED_SESSIONS`), no change to existing pool-switch path until B8; reverting a gate is a clean removal of its files.

---

## 18. Test Architecture & Real-Device Plan (Stage 15)

**Test layers `[REC]`** (all deterministic, authored before implementation):
- **Pure FSM tests:** every legal + illegal transition, idempotent/duplicate events, terminal states, timeout behavior.
- **Fake-clock tests:** normal expiry, reboot before/after deadline, deadline-passed, clock unavailable, forward/backward jump, monotonic continuity within a boot.
- **Persistence tests:** clean load, no record, each-state reload, truncated record, CRC mismatch, unsupported version, power-loss before/after commit, generation rollback, cleanup failure.
- **Secret tests:** password absent from GET, logs, history, errors, exports, provenance, test snapshots; (encrypted-at-rest test only if/when Policy 2 is enabled).
- **Pool-verification tests:** primary active, fallback active, auth failure, jobs-but-no-share, hashing resumed, stale telemetry, restore mismatch.
- **Concurrency tests:** duplicate create, manual PATCH conflict (409), OTA conflict (409), Restore-Now during target apply, cancel at every state, restart during every state.
- **QEMU tests:** boot recovery, NVS persistence across restart boundaries, API status, deterministic mocked Stratum events where feasible — extend the existing `test-ci` harness (currently **83 tests**).

**Existing tests to extend:** frontend `pool-strategy/*.spec.ts` (Karma) for the read-only monitor + API models; native `test-ci/main/unit_test_all.c` + `components/*/test` for FSM/store/recovery; QEMU `test-ci` for boot-recovery + persistence. **New native/QEMU** required for the record store, boot recovery, and mocked Stratum verification.

**Real-hardware plan (designed, NOT executed in this phase):** 15-min BTC→BCH→BTC session; browser closed after target verification; device restores at deadline while powered; reboot during target-active; power-cycle with deadline in future; power-cycle after deadline passed; target unavailable → auto-restore; restore-primary-unavailable-but-fallback-available; manual Restore Now; no secret in history/export; thermal & ASIC health remain normal. **No deliberate thermal/voltage/sensor-failure testing.**

---

## 19. Answers to the 25 Primary Audit Questions (index)

1 §3.1/4 · 2 §3.2 · 3 §3.1 · 4 §4 · 5 §4/5 · 6 §4 · 7 §5 · 8 §5 · 9 §5 · 10 §5 · 11 §5 · 12 §6 · 13 §7 · 14 §7 · 15 §7/11 · 16 §3.4 · 17 §7 · 18 §9.1 · 19 §9.1 · 20 §10 · 21 §10 · 22 §10 · 23 §10 · 24 §8 · 25 §18.

---

## 20. Unresolved Questions `[OPEN]`

1. **Keep-current across different hosts.** Confirm at implementation that `passwordMode:"keep"` with a different target host is genuinely Case A (the persisted secret authenticates on the target). The frontend already treats keep this way; firmware must validate no pool-secret write is requested and document that pool acceptance is the only proof.
2. **SNTP synchronization window sizing.** The bounded **SNTP** trusted-time acquisition window (max wait before fail-safe restore) needs a concrete value validated on real hardware (proposed default 10 min, hard max 15 min; must be short enough that mining the target a few extra minutes past a reboot is acceptable). Also confirm the NTP server list / DHCP-NTP behavior on the owner's network shape.
3. **Coordinator config reload vs full restart.** MVP reuses full `esp_restart()` for apply/restore. A future optimization (reload `SYSTEM_MODULE.pool_*` from NVS + coordinator-driven stratum restart, no reboot) is possible but out of MVP scope and needs its own concurrency review.
4. **Policy 2 enablement.** Turning on NVS encryption requires an `nvs_keys` partition + flash-encryption decision + a migration plan for already-deployed plaintext NVS; a separate hardware-security phase, not 2M.1B.
5. **Fallback-as-restore semantics.** Decide whether "operational restore" (source reached only via fallback) counts as success or partial for history — proposed: success, clearly labelled "operational (fallback)".

---

## 21. Verdict

**CONDITIONAL GO for Phase 2M.1B**, conditioned on:
- MVP is **Keep-current-password only** (different-password sessions rejected until encrypted storage is proven).
- A **dedicated SNTP trusted-time provider** (existing ESP-IDF networking stack) is added; **Stratum `ntime` stays explicitly untrusted for deadlines**. Deadline persisted only when the SNTP clock is trusted; monotonic-within-boot governs the active session; after reboot, a **bounded SNTP wait then fail-safe restore** to the captured source if trusted time is unobtainable (§7).
- Persistent record is **dual-slot A/B + CRC + generation** for **crash-consistency**, committed via a single-entry active-slot pointer; **no password in the record**.
- Boot recovery **never mutates pools on a corrupt record** and **always drives expiry toward restore**.
- **One owner** (scheduler lease); manual pool PATCH and OTA **blocked (409)** while active except Restore Now.
- Board **601/BM1370 only**.

**Acknowledged residual (not a blocker for the local MVP):** without Secure Boot / flash- / NVS-encryption / a secure monotonic counter, **malicious offline record rollback OR forgery is not cryptographically preventable** — the `crc32` is accidental-corruption detection, **not authenticity**, so generation+CRC give crash-consistency, not anti-rollback or anti-forgery. Record authenticity would require a MAC/signature, not a CRC. Reported honestly in §15 and the Security report.

All non-negotiable safety invariants (§ project brief 1–17) are satisfiable under this design. See the companion **State-Machine** and **Security** reports for the exhaustive transition, power-loss, and threat detail.
