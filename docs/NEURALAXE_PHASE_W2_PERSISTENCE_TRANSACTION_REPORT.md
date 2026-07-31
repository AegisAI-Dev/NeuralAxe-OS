# NeuralAxe OS — Gate W2: Persistence and Profile Transaction Model — Phase B Report

Phase: Weather-Aware Tuning, Gate W2 · Branch: `neuralaxe-v0.1-weather-aware-tuning-policy`
Committed implementation: **b03c5ac** ("feat: add crash-safe weather-tuning policy store and boot recovery") = **v2.14.2-58-gb03c5ac**, parent 3be1f56 (v2.14.2-57, Gate W1 closed).
Phase B verification date: 2026-07-31 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gate W2 FULLY GREEN.** The committed component `components/tuning_store/`
delivers the W2 contract — versioned weather-policy settings, the versioned
profile transaction record, the last-known-safe profile, crash-safe
pending/active/rollback states, the total boot-recovery decision, manual
override and cooldown persistence, dual-slot commit semantics, fake
power-loss tests and isolated QEMU NVS tests — under all four owner
corrections (finalize-not-clear, total per-state boot table, provider
UNCONFIGURED, and the durable cross-reboot rollback-attempt budget). All
Phase B checks ran against the exact committed HEAD (clean tree before and
after every run). No weather network request, no hardware apply, no
API/frontend change anywhere in the gate.

## 2. Committed diff (verified)

`git diff --stat 3be1f56..b03c5ac` — exactly 13 files, 5,253 insertions,
1 deletion; all committed blobs byte-verified LF:

| File | Lines |
|---|---|
| `components/tuning_store/include/tuning_record.h` | 448 |
| `components/tuning_store/include/tuning_store.h` | 230 |
| `components/tuning_store/include/tuning_recovery.h` | 222 |
| `components/tuning_store/tuning_record.c` | 1237 |
| `components/tuning_store/tuning_store.c` | 486 |
| `components/tuning_store/tuning_store_nvs.c` | 128 |
| `components/tuning_store/tuning_recovery.c` | 246 |
| `components/tuning_store/test/test_tuning_record.c` | 858 |
| `components/tuning_store/test/test_tuning_store.c` | 778 |
| `components/tuning_store/test/test_tuning_recovery.c` | 600 |
| `components/tuning_store/CMakeLists.txt` / `test/CMakeLists.txt` | 16 / 3 |
| `test/CMakeLists.txt` (root) | 1 line: `tuning_store` added to TEST_COMPONENTS |

No file under `main/` changed; no existing component changed; no report
document was included in the code commit (two-gate workflow).

## 3. Phase B verification results (all against b03c5ac)

| Check | Result |
|---|---|
| QEMU unit suite (esp32s3, IDF v5.5.3, QEMU 9.2.2) | **520 Tests, 0 Failures, 0 Ignored** — 67 `tuning_store` cases (22 record, 23 store, 22 recovery) + intact 453-test baseline; 0 non-title FAIL lines |
| Frontend gate (`npm run test:ci`, Brave headless) | **1052 / 1052 SUCCESS**, exit 0 |
| Default firmware build | **OK** — `esp-miner.bin` 1,658,272 B (byte-count unchanged; the component links nothing into `main/`) |
| Feature-enabled build (`CONFIG_NX_TIMED_SESSIONS=y`) | **OK** — `#define CONFIG_NX_TIMED_SESSIONS 1` confirmed |
| Strict warnings (`gcc -std=c11 -Wall -Wextra -Werror`, three pure sources) | **Clean** (the NVS adapter is IDF-only, compiled by the QEMU/firmware builds — B3 precedent) |
| Production tombstone-caller scan | **Zero callers** — `tuning_store_admin_reset` / `tuning_record_init_tombstone` are referenced only by the store implementation and tests |
| Secret / path / URL / endpoint scan | **Clean** — no credentials, wallets, hostnames, URLs, endpoints or machine paths (only the privacy-invariant doc comment and `.commit` struct members match the patterns) |
| Line endings | All 13 committed blobs **LF** (byte-level check; matches the repository convention) |
| Git writes by the assistant | **None** — the commit was made by the owner via GitHub Desktop |
| External systems | **None** — no weather request, no hardware, no owner LAN, no NTP, no tuning application |

Runner mirror of `unittest.yml`: repo mounted read-only into
`espressif/idf:v5.5.3`, tree copied in-container, `test-ci/CMakeLists.txt`
symlink restored, stale generated `test-ci/sdkconfig` dropped, 16 MB merged
flash image, `qemu-system-xtensa -machine esp32s3`.

## 4. Architecture of the committed component

Mirrors the proven B3/B4 pattern in a NEW record family (the B3 `nx_tps`
namespace and schema are deliberately not reused):

1. **`tuning_record.h/.c`** — ONE combined `TuningPolicyRecord` so a single
   crash-safe commit keeps every durable policy fact mutually consistent:
   versioned settings, the persisted W1 climate hysteresis stance, the
   profile transaction, the last-known-safe reference, manual override
   (trusted epochs only — the per-boot monotonic anchor is validated to
   never persist), cooldown, the monotonically-advancing trusted-epoch
   floor and bounded counters. Field-by-field little-endian codec; magic
   `NXWR`/`NXWP`; schema v1 + the W1 profile-model version gate; CRC-32 as
   accidental-corruption detection only; reject-to-recovery on any unknown
   schema/model — never silent migration. Bounded enums throughout — no
   URL, endpoint, API key or free-text host exists anywhere in the schema
   or component.
2. **`tuning_store.h/.c` + `tuning_store_nvs.c`** — the B3 dual-slot
   algorithm verbatim: pointer-write-last as the logical commit point,
   byte-exact + semantic read-back, previous slot never erased, loader
   never guesses, staged-newer ignored, generation overflow refused.
   Namespace **`nx_wtp`**, keys `rec_a`/`rec_b`/`active`. The real NVS
   adapter opens only that namespace, never erases, never logs payloads,
   and is wired nowhere (Gate W4 owns wiring under the B5 lease).
3. **`tuning_recovery.h/.c`** — the pure, total boot-recovery decision
   (§5/§6 below) plus the RAM-only boot context (§7).

## 5. Transaction finalization vs administrative reset (Correction 1)

- **Normal completion NEVER clears the policy record.** After a durably
  verified COMMITTED transaction, `tuning_record_finalize_transaction`
  canonicalizes ONLY the transaction subrecord to IDLE (all tx fields to
  canonical zero) and preserves settings, provider/location configuration,
  climate state, the newly verified last-known-safe, trusted-epoch floor,
  cooldown/override facts and record-level counters — proven byte-for-byte
  by tests, including a store-level round trip whose reload returns
  STORE_OK (never CLEARED). Rollback completion follows the same path.
- **`tuning_store_admin_reset`** is the only full-store tombstone: an
  EXPLICIT COMPLETE POLICY RESET with **zero production callers in W2**
  (scan-verified) and a documented prohibition against being reached from
  transaction completion, boot recovery, rollback completion, automatic
  error recovery, cooldown/override expiration, profile verification or
  routine policy disablement. Even it refuses every pending/rollback/
  recovery transaction state (all eight loop-tested → STATE_CONFLICT); no
  boot-recovery plan can express a tombstone (proposals are transaction
  states only, asserted for all ten states).

## 6. Total boot-recovery table (Correction 2)

`tuning_boot_plan` is an explicit switch over all ten `TUNING_TX_*` states
with a fail-closed default, pinned by `_Static_assert(TUNING_TX__COUNT ==
10)` — adding a state breaks the build until the table and tests are
re-audited. Per state (each row exercised by test):

| State | Decision |
|---|---|
| IDLE | NORMAL_RESUME — no rollback, no apply, no counter use, state preserved |
| COMMITTED | NORMAL_RESUME + `finalize_transaction` — already verified & durable; last-known-safe preserved; only the canonicalize-to-IDLE proposal; never re-applied in W2; zero budget consumed |
| INTENT_PERSISTED / APPLY_PENDING / APPLYING / RESTART_PENDING / VERIFYING | apply success unknown, never assumed: ROLLBACK to last-known-safe (upgrade inhibit + persist-before-action) or, without one, RETAIN_CURRENT_UNVERIFIED + operator recovery — no invented safe profile |
| ROLLBACK_PENDING / ROLLING_BACK | rollback obligation preserved and resumed (recorded target, else last-known-safe); never a second rollback transaction; counter never reset; each real boot reserves the next attempt (§7) |
| RECOVERY_REQUIRED | operator recovery only — no apply, no retry, no tombstone, no IDLE transition, counters preserved |
| unknown/future | fail closed to RECOVERY_REQUIRED, evidence preserved |

Store failures plan RECOVERY_REQUIRED with **no record mutation**; EMPTY/
CLEARED start clean (feature disabled by default).

## 7. Durable cross-reboot attempt budget (final blocker)

**Design: Option A — a bounded, RAM-only `TuningBootContext`**
(`attempt_committed_this_boot`, `committed_record_generation`,
`committed_attempt_count`):

- **Boot identity is the context's RAM lifetime** — it is never persisted,
  so a real boot starts clear by construction; no wall clock, Stratum
  ntime or reset-reason value is consulted. It cannot suppress an
  increment after a reboot and cannot survive one.
- **The increment becomes durable** only when the persist-before-action
  proposal is committed via `tuning_store_commit_record` (read-back
  verified, pointer written last); the integrator then notes it with
  `tuning_boot_context_note_committed`. `STORE_COMMIT_UNCERTAIN`,
  `READBACK_MISMATCH` or any non-OK result must never be noted.
- **Rollback execution eligibility** is the plan output
  `rollback_action_eligible` — true only when the noted (generation,
  count) pair matches the observed committed record and a real rollback
  target exists. Fresh proposals, unnoted commits and stale contexts are
  all ineligible (tested).
- **Semantics per rollback state**: ROLLBACK_PENDING — a reservation covers
  only the boot that made it; a NEW boot must reserve the next attempt
  before action. ROLLING_BACK — a reboot means the previous attempt never
  reached durable completion; it is treated as interrupted and the next
  attempt must be reserved. Same-boot task wakeups resume the reserved
  attempt with no further increment (five-reevaluation test: exactly one
  durable unit). The exhaustion check applies to NEW reservations only, so
  a granted third attempt stays executable within its boot.
- **Three consecutive crash/reboot cycles deterministically reach
  RECOVERY_REQUIRED** (chained test: reserve 1, crash; reserve 2, crash;
  reserve 3, crash; fourth boot → BOOT_BUDGET_EXHAUSTED + operator).
  Boundaries: 0→1, 1→2, 2→3 rollback; 3 → recovery; storage-max 10 and
  corrupted 255 → recovery with the saturating increment clamped (never
  wraps); invalid encodings are rejected at decode. IDLE/COMMITTED consume
  no budget; a missing last-known-safe consumes at most one unit before
  the terminal RECOVERY proposal.
- Integrator contract note: during an unresolved rollback obligation, no
  unrelated record commits may be interleaved before executing the
  reserved attempt (a generation mismatch conservatively demands a fresh
  reservation).

## 8. Provider default (Correction 3)

`TUNING_PROVIDER_UNCONFIGURED` (wire value 0) is the default in the zero
record and in `tuning_settings_defaults` — **Open-Meteo is never an
implicit operational default** (the Gate W0 commercial/distribution
decision is unresolved); selecting `OPEN_METEO` is an explicit future
configuration action. Defaults: disabled, coordinates unset (0/0 blocks
enablement), Europe/Brussels as a bounded product timezone default that
implies no provider. `tuning_settings_enable_check` fails on UNCONFIGURED
provider, unset coordinates, missing schedule, and any role profile that
is absent or not auto-eligible — **with the production registry (all
profiles UNVALIDATED) enablement always fails today** (tested); the
success path exists only with synthetic validated fixtures.

## 9. Engineering notes for later gates

1. The W4 integrator sequence for boot is: load → `tuning_boot_plan` →
   (if store OK) commit the proposal → `tuning_boot_context_note_committed`
   → re-plan → act only when `rollback_action_eligible`; for COMMITTED,
   run `tuning_record_finalize_transaction` + a normal commit instead.
2. Flash-wear contract: commit on state transitions only (never per
   telemetry sample or scheduler wakeup); the 24 KB shared `nvs` partition
   has a destructive full-erase failure mode when full (Gate W0 §10).
3. `TUNING_TX_RESTART_PENDING` is retained though the audited tuning apply
   path is live (no restart) — reserved for future restart-requiring
   fields.
4. CRC-32 equals `esp_rom_crc32_le(0, ...)` (same nibble table as the
   QEMU-proven B3 codec); `tuning_record_crc32` is public for tests.
5. Verification gotcha: piping `npm run test:ci` through a short `tail`
   can trim the Karma TOTAL line — the JUnit `report.xml` is the reliable
   count source. Line-ending audits on this machine must use byte-level
   checks (PowerShell `ReadAllBytes` / `git ls-files --eol`); MSYS
   `grep -c $'\r'` pipelines report phantom CRs.

## 10. Scope confirmations

No weather network request, no hardware apply, no API/frontend change, no
runtime wiring, no tuning application, no frequency/voltage path change,
no edits outside the component + one test-registry line, no Git write
operations by the assistant in either phase, no hardware/owner-LAN/real
pool/NTP/OTA access, and the private TCH recovery dump was not touched.
All production profiles remain UNVALIDATED and the feature cannot be
enabled against the production registry.

## 11. Owner action (second gate)

Commit this report (suggested: `docs: add Phase W2 persistence and profile
transaction report`). That closes Gate W2; reply `continue W3` to begin
Gate W3 — the replaceable weather-provider interface, Open-Meteo adapter,
strict HTTPS/parser design, trusted-time scheduling with the
Europe/Brussels pure UTC rule, retries/rate limiting and cache rules —
fake network only, profile-selection intents only, no tuning application.
