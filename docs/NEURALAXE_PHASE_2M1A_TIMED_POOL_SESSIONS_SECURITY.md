# NeuralAxe OS — Phase 2M.1A

## Timed Pool Sessions — Security Audit & Secret Model

Companion to `NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_AUDIT.md`. **Audit only — no firmware written.**

> Legend: **[FACT]** observed in repo · **[IDF]** framework guarantee · **[INFER]** reasoned · **[REC]** recommendation · **[OPEN]** unresolved.
> No secrets, wallet/account values, private IPs, or owner-local paths appear in this document.

---

## 1. Encryption & Secure-Boot Evidence (validated board-601 build)

From the tracked `sdkconfig` and `partitions.csv`:

| Control | State | Evidence |
|---|---|---|
| **NVS encryption** | ❌ **disabled / not provisioned** | `# CONFIG_NVS_ENCRYPTION is not set`; `partitions.csv` has **no `nvs_keys` partition**; only `nvs, data, nvs, 0x9000, 0x6000` |
| **Flash encryption** | ❌ **disabled** | `# CONFIG_SECURE_FLASH_ENC_ENABLED is not set`, `# CONFIG_FLASH_ENCRYPTION_ENABLED is not set` |
| **Secure Boot** | ❌ **disabled** | `# CONFIG_SECURE_BOOT is not set` (the `_V2_PREFERRED` / `_V2_RSA_SUPPORTED` lines are default-preference / SoC-capability, not enablement) |
| Coredump to flash | ❌ disabled (mitigates dump leakage) | `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y` (a 64 KB `coredump` partition exists but is unused) |
| **Record authenticity / anti-rollback** | ❌ **none** | Secure Boot off ⇒ no anti-rollback eFuse / secure version scheme; the record's CRC is **error-detection, not a MAC** ⇒ **no authenticity and no rollback/forgery protection** |
| SoC capability flags | present but irrelevant to enablement | `CONFIG_SOC_FLASH_ENC_SUPPORTED=y`, `CONFIG_SOC_SECURE_BOOT_SUPPORTED=y`, `CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES*` |

**[INFER] At-rest posture:** NVS is **plaintext in flash**. Anyone with physical read/write access to the module's flash can recover any NVS value, including the pool password, **and can substitute an older-but-valid stored record or forge an arbitrary one** — the record's CRC is error-detection, **not authenticity**, so it is trivially recomputed (no encryption, no Secure Boot, no secure monotonic counter to detect tampering). Secure Boot being off also means firmware images are not authenticated at boot.

---

## 2. Existing Password Handling — what is (and isn't) protected

**[FACT]** The pool password is stored plaintext in NVS key `stratumpass` (fallback `fbstratumpass`), and is loaded into RAM (`SYSTEM_MODULE.pool_pass`) at boot (`main/system.c` `SYSTEM_init_system`, ~L83). The firmware **can and does read it back**.

**[FACT]** The **only** protection today is a **GET/UI convention**: `main/http_server/system_api_json.c` (L155–219) serializes `stratumUser`/`URL`/`Port`/`Cert`/`TLS` but **never** the password. The frontend Pool Strategy treats the password as write-only and session-only (`pool-strategy.component.ts`, `pool-verify.ts` explicitly note "the password itself is never read back from the device").

**[INFER] Critical distinction (must be stated honestly):**
- *"Encrypted at rest"* — **FALSE** for this build.
- *"The frontend cannot read it back"* — **TRUE** (GET omits it).
- *"The firmware cannot read it"* — **FALSE** (firmware holds cleartext).

The product's password security today rests on (a) LAN-scoped API access and (b) GET omission — **not** on cryptographic storage. A timed-session design must not claim more.

---

## 3. Secret Lifecycle for Timed Sessions

`[REC]` Under the MVP (**Keep-current password only**, Policy 1), the session introduces **no new persisted secret**:

```
Create request:      NO password field accepted (create rejects any pool-secret field).
Session record:      NO password field exists in the schema (audit §11).
Target apply:        pool NVS password key is UNCHANGED (keep-current) — the persisted
                     stratumpass already authenticates the target profile (Case A).
Timer / active:      no secret in RAM beyond the pre-existing SYSTEM_MODULE.pool_pass.
Restore:             source config reapplied; password key still UNCHANGED.
History / GET / export: state, hosts, users, timestamps, verification — NEVER a password.
Logs / errors / provenance: NEVER a password (existing convention preserved).
```

**Minimum in-memory secret lifetime:** unchanged from today — the pool password lives in `SYSTEM_MODULE.pool_pass` for the process lifetime (as it already does). The scheduler **does not** copy it, log it, or place it in the record.

**Zeroization `[REC]`:** because the MVP persists no new secret and copies none, there is no new secret buffer to zeroize. If Policy 2 (encrypted different-password) is ever enabled, any transient plaintext restore-secret buffer must be `explicit_bzero`-style wiped immediately after the encrypted write, and never logged — `[OPEN]` to be specified in that future phase.

---

## 4. Same-Password vs Different-Password Decision

| Policy | Description | Verdict |
|---|---|---|
| **1 — Keep-current only** | Session never changes the password; persisted `stratumpass` used throughout | ✅ **MVP CHOICE** — introduces no plaintext secret, safe at rest |
| 2 — Different-pw with proven encrypted storage | Persist encrypted restore secret in encrypted NVS | ⏸ **Not available** — NVS/flash encryption disabled (§1). Allowed only after a dedicated hardware-security phase proves it enabled |
| 3 — Different-pw only while browser connected | Browser holds the secret; not unattended | ❌ **Not an unattended session** — must never be labelled device-autonomous; excluded from 2M.1B |
| 4 — Other | — | not needed |

**Decision `[REC]`:** **Policy 1 for Phase 2M.1B.** Different-password create requests are **rejected at the API** with stable `ERR_PW_MODE_UNSUPPORTED` (HTTP 422). This is **Security Blocker #1** made into an enforced guard rather than a warning.

**[OPEN]** Keep-current with a different *host* but same *password string* is Case A and allowed; implementation must validate no password write is requested and rely on pool acceptance as the only proof of correctness.

---

## 5. API Privacy Contract

`[REC]` The timed-session API (`/api/system/pool-session*`, audit §14) obeys the existing conventions plus explicit rules:

1. **No password accepted.** `POST` create rejects any pool-secret field (`stratumPassword`, `fallbackStratumPassword`, or any alias) → 422.
2. **No password returned.** `GET` exposes: `state`, `sessionId`, `sourceChain`/`targetChain`, `sourceHost`/`targetHost` (host + user only), timestamps, `remainingSeconds`, `deadline` (only if `deadline_epoch_valid`), `targetVerification`/`restoreVerification` flag sets, `failureCode`, `recovery`. **Never** a secret, never raw NVS, never internal stack error text.
3. **Account/worker** follow the existing GET convention (shown as today; not further masked, not further exposed).
4. **Auth:** inherits `is_network_allowed` (LAN + private-origin) — see §7 residual risk; the timed-session endpoints add no weaker path.
5. **Machine-readable error codes** are defined separately from user-facing text (§8), so error text carries no sensitive detail.

---

## 6. Threat Matrix

Assets: active pool identity, **original restore pool identity**, pool passwords, account/worker IDs, session deadline, persistent session record, operation ownership, history, firmware API.

| # | Threat | Exposure | Likelihood | Impact | Control `[REC]` | Residual | Blocks 2M.1B? |
|---|---|---|---|---|---|---|---|
| T1 | LAN attacker submits a timed session | API is LAN-scoped, no auth | med (shared LAN) | pool redirected for a bounded window, auto-restores | LAN trust (pre-existing); bounded duration; auto-restore; **one session** guard | LAN-trust residual (product-wide) | No |
| T2 | CSRF from a browser | `Origin:*` CORS + IP check | low | as T1 | origin-IP must be private (http_server.c:307–317); state-changing POST triggers Origin | weak (IP-based) CSRF mitigation | No |
| T3 | Malicious profile data / crafted chain label | create body | med | bad config applied, then restored | strict validation (types, duration bounds, enum chain, host/port limits) mirroring `check_settings_and_update` | low | No |
| T4 | Oversized strings / malformed port | create body | med | parse error | bounded fixed-size record fields; port ∈ u16; reject over-length | low | No |
| T5 | Integer overflow in duration | create body | low | wrong deadline | 64-bit compute; validate ∈ [900, 86400] before persist | none | No |
| T6 | Replayed create/cancel; duplicate idempotency key | API | med | duplicate/again | idempotencyKey + session_id/generation → no-op | none | No |
| T7 | Stale frontend overwrites current state | concurrent PATCH | med | restore contract lost | **PATCH pool fields 409 while lease held** (Inv 9); frontend is read-only monitor | none | No |
| T8 | NVS / flash extraction (physical) | plaintext NVS | low (needs physical) | pool password + config leak | **pre-existing**; no new secret added by MVP (Policy 1) | plaintext-at-rest residual (product-wide) | No (MVP adds no new secret) |
| T9 | Plaintext dual secret at rest (different-pw) | would add 2nd secret | — | 2nd credential leak | **rejected by Policy 1** | none (feature excluded) | **Would block; excluded** |
| T10 | Secrets in logs / exceptions / coredump / HTTP / history | logging paths | low | leak | no password in record/API/logs; coredump-to-flash off; existing convention | low | No |
| T11 | Secrets in browser persistence | frontend | low | leak | session-only frontend secret (existing Phase 2M behavior) preserved | low | No |
| T12 | Corrupted session record triggers bad mutation | boot | low | wrong pool applied | CRC + dual-slot; **corrupt → RECOVERY_REQUIRED, run `main`, no mutation** (Inv 7) | none | No |
| T13a | **Accidental** rollback to older record generation (torn write / interrupted transition / wrong-slot pick) | interrupted commit | low | stale deadline/config | monotonic `generation` + CRC; highest valid wins | none | No |
| T13b | **Malicious offline record rollback OR forgery** — attacker with physical/offline flash write access replays an **older, internally-valid** record, or **forges an arbitrary** record (any state / pool identity / deadline) with a valid CRC | plaintext flash; **no Secure Boot / flash-enc / NVS-enc / secure monotonic counter**; **CRC = error-detection, not authenticity** | low (needs physical access) | scheduler acts on an attacker-chosen session state / pool-restore snapshot | generation+CRC **do NOT prevent this** (any flash writer recomputes the CRC); blast radius bounded by fixed duration + auto-restore; only Secure Boot + encryption + a secure monotonic counter (record authenticity ⇒ MAC/signature, not CRC) would prevent it | **residual — documented, NOT mitigated** | No for local MVP, but must be reported honestly |
| T14 | Hostname-based chain spoofing | label inference | low | wrong chain shown | chain is **explicit from applied profile**, never hostname-inferred (Inv 14) | none | No |
| T15 | DoS by repeated restart requests | API | med | reboot churn | restart/apply gated by scheduler state + reboot-loop guard; API restart rate-limited by session lease | low | No |
| T16 | **Permanent temporary-pool mining after clock failure** | no trusted clock today | med | mines wrong pool indefinitely | **dedicated SNTP trusted-time provider** + monotonic-within-boot timer + bounded SNTP wait → **fail-safe restore** + boot forces restore (Inv 5) | low (bounded over-run) | **Must be implemented — see §9** |
| T17 | Pool `ntime` clock manipulation | pool-controlled clock | med | early/late restore | powered session uses monotonic time (immune); cross-reboot deadline uses the **SNTP-backed trusted clock only** — **`ntime` is never trusted for deadlines** (audit §7) | low | No |

---

## 7. Residual Risks (honest posture)

`[FACT/INFER]` These are **pre-existing product-wide** properties, not introduced by timed sessions, but must be acknowledged before unattended operation:

1. **No API authentication.** `is_network_allowed` grants any private-range LAN client full control (PATCH, restart, OTA). Timed sessions inherit this. `[OPEN]` A future auth/token phase would materially reduce T1/T2/T15.
2. **Plaintext secrets at rest.** Single pool password is plaintext in NVS today; MVP adds no second secret.
3. **Wide-open CORS** (`Access-Control-Allow-Origin: *`) with IP-based origin mitigation only.
4. **No Secure Boot** — firmware image authenticity is not enforced.
5. **Malicious offline session-record rollback or forgery (T13b).** The new persistent record's `generation`+CRC give **crash-consistency only**; the CRC is **accidental-corruption detection, not authenticity** (not a MAC/signature), so any attacker who can write flash can recompute it. With no Secure Boot, flash encryption, NVS encryption, or secure monotonic counter, an attacker with physical/offline flash access can substitute an **older, internally-valid** record **or forge an arbitrary one** and it will be accepted. This is **not cryptographically preventable on this build**; it is bounded by the fixed session duration and automatic restore, and must **not** be described as mitigated. Preventing it requires the same hardware-security controls as Policy 2.

None of these individually blocks the *Keep-current* MVP — the MVP adds no new secret, always drives expiry toward restore, and auto-restores within a bounded window — but they define the ceiling on how "secure" unattended operation can be truthfully called on this build, and item 5 must be reported honestly rather than folded into the crash-consistency guarantees.

---

## 8. Machine-Readable Error Codes (stable, secret-free)

`[REC]` Defined separately from user-facing text; none contains sensitive data.

| Code | Meaning |
|---|---|
| `ERR_SESSION_EXISTS` | a session is already active (409) |
| `ERR_PW_MODE_UNSUPPORTED` | different-password session rejected (422) |
| `ERR_DURATION_RANGE` | duration outside [900, 86400] (422) |
| `ERR_PROFILE_INVALID` | unknown/invalid target profile or chain (422) |
| `ERR_BOARD_UNSUPPORTED` | not Gamma/601/BM1370 (422) |
| `ERR_POOL_LOCKED` | manual pool PATCH/OTA blocked during session (409) |
| `ERR_TARGET_VERIFY_TIMEOUT` | target not verified within budget (internal → history) |
| `ERR_TARGET_AUTH` | target authorization failed |
| `ERR_TARGET_NO_JOBS` | connected but no jobs / no hashing |
| `ERR_RESTORE_VERIFY_TIMEOUT` | restore not verified after retries |
| `ERR_RECORD_CORRUPT` | session record failed CRC on both slots |
| `ERR_SCHEMA_UNSUPPORTED` | record schema version not understood |
| `ERR_TIME_UNTRUSTED_FAILSAFE` | fail-safe restore due to untrusted time |
| `ERR_REBOOT_LOOP` | auto-recovery stopped after repeated resets |

---

## 9. Security Blockers & Required Controls Before Unattended Operation

**Explicit blockers `[REC]` (must be enforced in 2M.1B):**

1. **Different-password unattended sessions are BLOCKED** until encrypted persistent storage is *proven enabled* (Policy 1; enforced via `ERR_PW_MODE_UNSUPPORTED`). Do not persist a plaintext second secret.
2. **Permanent temporary-pool mining after clock failure is BLOCKED** by a **mandatory dedicated SNTP trusted-time provider** (existing ESP-IDF networking stack) + monotonic-within-boot timing + bounded SNTP wait → **fail-safe restore** (Invariant 5 / T16). **Stratum `ntime` is never trusted for deadlines.** This control is **not optional**, and DNS/NTP unavailability must degrade to fail-safe restore, never to indefinite target mining.
3. **Corrupt-record pool mutation is BLOCKED** — a bad-CRC record must run `main` config with no mutation and surface RECOVERY_REQUIRED (Invariant 7 / T12).
4. **Accidental / crash generation rollback is PREVENTED** by monotonic `generation` + CRC (T13a) — this is **crash-consistency only**, and the CRC is error-detection, not authenticity. It does **NOT** cover **malicious offline record rollback or forgery** (T13b), which is a residual risk (below), not a blocker and **not mitigated** on this build.

**Required controls implemented by the design:**
- **Dedicated SNTP trusted-time provider** with a defined trusted predicate + bounded sync window (audit §7); `ntime` untrusted for timing.
- One operation owner (scheduler lease); manual pool PATCH + OTA return 409 while active (Invariants 8, 9).
- Bounded retries + reboot-loop guard using `esp_reset_reason()` (Invariants 15, 16).
- Explicit chain labels from the applied profile, never hostname (Invariant 14).
- No secret in record, API, logs, history, exports, provenance (Invariant 12).
- Board gate to 601/BM1370 (Invariant 17).

**Documented residual (honest, not mitigated on this build):**
- **Malicious offline session-record rollback or forgery (T13b)** — the CRC is accidental-corruption detection, not authenticity; bounded by fixed duration + auto-restore, but **not cryptographically preventable** without the hardware-security controls below. Must be reported, not folded into the crash-consistency guarantees.

**Recommended (not blocking, future phases):**
- API authentication / CSRF token (reduces T1/T2/T15).
- NVS + flash encryption + `nvs_keys` partition (enables Policy 2; unblocks different-password).
- **Secure Boot + encryption + a secure monotonic counter / anti-rollback scheme** (image authenticity **and** the only real defense against malicious offline record rollback or forgery, T13b — record authenticity requires a cryptographic MAC/signature, not a CRC).

**Security verdict:** the **Keep-current-password MVP is safe to implement** on the current build, provided the **mandatory SNTP trusted-time provider** and fail-safe restore are implemented. **Different-password unattended sessions must remain blocked** until a hardware-security phase enables and proves encrypted storage. **Malicious offline record rollback or forgery (T13b) is an accepted, documented residual** for the local board-601 MVP — the CRC is accidental-corruption detection, **not authenticity**, so it is not mitigated by generation+CRC and must not be presented as such; eliminating it requires Secure Boot + encryption + a secure monotonic counter (record authenticity needs a MAC/signature, not a CRC).
