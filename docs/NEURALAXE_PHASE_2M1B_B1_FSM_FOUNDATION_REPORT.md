# NeuralAxe OS — Phase 2M.1B, Gate B1

## Timed Pool Sessions — Pure Models & Deterministic FSM Foundation Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions` · **Commit:** `e72db15` — `feat: add timed pool session FSM with restore obligation` (**v2.14.2-46-ge72db15**, parent `daf25bb` = the committed Phase 2M.1A audit)
**Scope:** pure firmware-domain foundation only — **no runtime wiring** (no NVS, SNTP, clock, tasks, queues, Stratum, HTTP, OTA, restart or frontend integration). No production runtime behavior changed.

Architectural source of truth: the committed Phase 2M.1A reports
(`NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_AUDIT.md`, `…_STATE_MACHINE.md`, `…_SECURITY.md`).

---

## 1. Verdict

**Gate B1 PASS (Phase A implemented + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 8 approved B1 files, 2536 insertions (see §7) |
| Feature flag | `CONFIG_NX_TIMED_SESSIONS` **default n**; referenced **only** in `main/Kconfig.projbuild` — gates no code in B1 |
| Runtime wiring | **none** — no file outside `components/pool_session/` includes `pool_session.h`; `main` does not require the component (linker-GC'd from the app) |
| Frontend `npm run test:gate` | **1052 / 1052, exit 0** |
| Main app, flag OFF | configure OK (`# CONFIG_NX_TIMED_SESSIONS is not set`) + `pool_session` compiles in the main graph |
| Main app, flag ON | configure OK (`CONFIG_NX_TIMED_SESSIONS=y`) + compiles — no code references the flag, so both builds are equivalent |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0; regenerated sdkconfig carries `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` |
| QEMU | **140 Tests, 0 Failures, 0 Ignored — exit 0** (83 baseline + **57** `[pool_session]`) |
| Module warnings | compiles `-Wall -Wextra -Werror` clean |
| Secret / local-path scan | clean (synthetic `*.example` fixtures only; no secrets, private IPs, wallets, or absolute paths) |
| Tree after verification | clean; `test-ci/CMakeLists.txt` symlink restored byte-exact; no `report.xml` or release artifacts |

---

## 2. What Gate B1 delivers

A new self-contained component **`components/pool_session/`** (modeled on the tested `thermal_control` component precedent):

- `include/pool_session.h` — bounded domain models, enums, machine codes, side-effect intents, static asserts, pure API.
- `pool_session.c` — pure validation, total invalid-safe state classification, and the deterministic transition engine. No heap, no globals, no I/O, no logging; the input session is never mutated (`out_next` may safely alias the input — the engine copies first).
- `test/test_pool_session.c` — **57 exhaustive Unity tests** (`[pool_session]`), registered via `TEST_COMPONENTS` in `test/CMakeLists.txt` (symlinked by `test-ci`).

**Models (model version 1, `PoolSession` = 992 bytes, fixed/bounded, no heap pointers):** `PoolEndpoint` (host[80], port, user[128], protocol, tls) · `PoolConfigIdentity` (primary + fallback + `fallback_enabled` + **explicit chain** + profile_id[24]) · `PoolSessionRequest` · `PoolSession`. **No password field exists anywhere** — Keep-current-password only; `POOL_SESSION_PW_REPLACE` is a rejectable tag carrying no bytes (`ERR_PW_MODE_UNSUPPORTED`).

**FSM:** 17 states (audit state model incl. `CANCELLED`), 28 events, 28 stable machine codes, 12 abstract side-effect intents (descriptions only — B1 executes none). Duration validated 900–86400 s with an overflow-safe minutes→seconds helper. Board/ASIC eligibility validated against `"601"` / `"BM1370"` from explicit request inputs. Same-chain different-pool sessions valid; exact source==target rejected; chain never inferred from hostname.

**Verification gates:** `TARGET_ACTIVE` requires connection + mining + identity observed (accepted share NOT required); `COMPLETE` requires restore connection + mining + **source** identity verified. Retries: six bounded saturating counters (max 3 each). Duplicate CREATE (same id) idempotent; different id → conflict; duplicate observes are no-ops; every state × event sweep is deterministic and input-immutable (property tests).

---

## 3. The restore obligation (owner-directed safety correction)

`PoolSession.restore_required` — a **monotonic safety fact**, persistence-facing for Gate B3 / boot recovery:

- **Set true together with the first `APPLY_TARGET_CONFIGURATION` intent** (centralized in the engine's finalize step, so the returned session already carries the obligation before a caller could execute the intent).
- **Never cleared** by target apply/restart/verify failure, identity mismatch, retry exhaustion, cancel, interruption or device restart — the engine has exactly two writers: the setter above and the clear on entering **`COMPLETE`** (verified source restoration + mining resumed).
- **Terminal vs acknowledgeable are separated:** result states = `COMPLETE`, `CANCELLED`, `RESTORE_FAILED`, `RECOVERY_REQUIRED`; **acknowledgeable** (`pool_session_is_acknowledgeable_terminal`) = result state **AND** `!restore_required` — in practice only `COMPLETE` and pre-mutation `CANCELLED`/pre-mutation `RECOVERY_REQUIRED`. `ACKNOWLEDGE_TERMINAL` elsewhere returns deterministic `ERR_STATE_CONFLICT` and changes nothing.
- **Post-mutation `TARGET_FAILED`** is a persistent non-terminal diagnostic state whose only exits are restoration (`RESTORE_APPLY_REQUESTED` / `RESTORE_NOW_REQUESTED`). `CANCELLED` is reachable **only before** target mutation; cancel afterwards converts to `RESTORE_DUE`.
- **`RESTORE_FAILED` / `RECOVERY_REQUIRED`** retain the immutable source snapshot, target identity, obligation and retry metadata; a manual `Restore Now` re-attempts restoration with a reset (bounded) budget. **No destructive "abandon restore" exists.**
- OTA / manual-pool-change blocking is obligation-aware: `pool_session_blocks_ota/blocks_manual_pool_change(session)` block whenever a session is unresolved — in progress **or** a result state still owing a restore.
- Property tests prove: the obligation is only ever established by the apply-target intent; while owed, **no event reaches IDLE, CANCELLED or an acknowledgeable terminal**; it clears **only** by reaching `COMPLETE`; the source identity stays byte-identical through every failure and recovery path.

---

## 4. Test suite (57 `[pool_session]` tests → QEMU 83 ⇒ 140)

Model init/bounds/ports/chains/schema · request validation (duration edges incl. overflow, board/ASIC, password mode, hosts/fallback, source==target, Custom/Unknown transitions) · total state-classification incl. invalid enum safety and obligation-aware blocking · full happy path (create → snapshot → apply → restart → verify → active → deadline → restore → complete → ack) · target failures (apply/restart/verify-timeout incl. host-mismatch-as-timeout; bounded retries; exhaustion → `TARGET_FAILED` → auto-restore path) · restore failures (bounded retries → `RESTORE_FAILED`; manual re-attempt resets budget) · cancel semantics (pre-mutation → `CANCELLED`; during apply/verify/active → `RESTORE_DUE`; duplicates; during restore = no-op; terminal = conflict) · Restore-Now semantics (idempotent; never clears source; invalid in IDLE; no-op after COMPLETE; pre-apply behaves as cancel) · idempotency (duplicate/conflicting CREATE, duplicate observes, ack/re-ack) · recovery safety (corrupt/unsupported record → `RECOVERY_REQUIRED` with **no pool-mutation intent**, source/target preserved; device-restart in every state emits no mutation) · privacy (planted secret never appears in diagnostic strings; dot-free ALL-CAPS tokens) · property sweeps (every state × event: valid outputs, illegal ⇒ unchanged, no-op ⇒ byte-identical, retry bounds, obligation monotonicity, source/target immutability, per-state input immutability) · representative-pair determinism · the 12 obligation tests of §3.

---

## 5. Engineering notes & gotchas (recorded for future gates)

1. **Test-task stack.** The Unity tests run in the main task. `test-ci` previously inherited the ESP-IDF default `CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584` **with both watchdogs disabled** — so a marginal stack overflow in a test **silently corrupts memory instead of resetting**, and surfaced here as a later, unrelated heap-using stratum test hanging in QEMU (deterministically, with every executed test still "PASS"). Diagnosis: without `pool_session` the 83-test suite passed; with a 32 KB stack the full 140 passed. Fix: (a) `test-ci/sdkconfig.defaults` now sets **8192** (matching the main firmware) and (b) the test helpers were made stack-light (file-static request scratch; `step_evt`/`do_create` transition **in place**, which the engine's aliasing contract explicitly permits).
2. **`test-ci/sdkconfig` is generated and gitignored** — a stale local copy masks `sdkconfig.defaults` changes; delete it locally to force regeneration (CI checkouts are fresh, so CI always picks the defaults up).
3. **Struct sizes are part of the safety envelope.** `PoolSession`/`PoolSessionRequest` are 992 bytes; helper layers that stack several by value can overflow a small task. `pool_session_transition` intentionally supports `out_next == current` so callers never need a second session on the stack.
4. Comment hygiene: an `APPLY_*/…` glob inside a C block comment contains `*/` and terminates it — spell such lists without the trailing `*/` sequence.

---

## 6. Validation summary (Phase B, against commit `e72db15`)

- Branch/ancestry verified read-only; working tree clean before and after; no Git write operations.
- Frontend `npm run test:gate`: **1052 / 1052, exit 0**. (Host note: the Windows Edge browser auto-updated to 150 during this phase and its headless mode began disconnecting mid-run regardless of sandbox flags — with zero test failures in every partial run. The gate was completed green by pointing Karma's `CHROME_BIN` at the locally installed Brave (Chromium). This is a host/browser environment matter only; CI uses its own Chromium and the frontend is untouched by B1.)
- Main app configure + `pool_session` compile: flag OFF and flag ON both green (temp build dirs; tracked `sdkconfig` untouched).
- Fresh `test-ci` build + QEMU: **140 / 0 / 0, exit 0**.
- Secret/local-path scan on all committed files: clean. `test-ci/CMakeLists.txt` restored to its exact 22-byte tracked form after container builds; no `report.xml`, no release artifacts.
- No hardware, COM/USB, real BTC/BCH pools, pool accounts, wallets, real credentials, owner LAN, or the private TCH recovery dump were accessed.

---

## 7. Committed files (8)

| File | Change |
|---|---|
| `components/pool_session/CMakeLists.txt` | new — component registration |
| `components/pool_session/include/pool_session.h` | new — models/enums/codes/API (+static asserts) |
| `components/pool_session/pool_session.c` | new — pure validation + classification + transition engine |
| `components/pool_session/test/CMakeLists.txt` | new — test registration (`REQUIRES cmock pool_session`) |
| `components/pool_session/test/test_pool_session.c` | new — 57 Unity tests |
| `main/Kconfig.projbuild` | +`config NX_TIMED_SESSIONS` (bool, default n, experimental; wires nothing in B1) |
| `test/CMakeLists.txt` | `TEST_COMPONENTS` += `pool_session` |
| `test-ci/sdkconfig.defaults` | +`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` (test app stack; see §5.1) |

---

## 8. Gate B2 prerequisites

1. Branch from this committed B1 head (`e72db15`).
2. **B2 = abstract clock / trusted-time provider** per audit §7: injectable **monotonic** source + **SNTP-backed trusted-time predicate** (`SNTP_SYNC_STATUS_COMPLETED` this boot + sanity band + ≥ `verified_start_epoch`) + the bounded synchronization window (default 10 min / max 15) — as a pure, unit-tested interface with **fake-clock tests** (normal expiry, reboot before/after deadline, deadline-passed-at-boot, clock unavailable, forward/backward jump, monotonic continuity). Still **no runtime wiring**, no real SNTP task, no NVS.
3. Stratum `ntime` remains explicitly untrusted for deadlines; fail-safe restore needs no clock.
4. Preserve: `CONFIG_NX_TIMED_SESSIONS` default n; Keep-current-password-only; bounded retries; one session; board 601/BM1370; the restore-obligation invariants of §3; QEMU ≥ 140 green.
