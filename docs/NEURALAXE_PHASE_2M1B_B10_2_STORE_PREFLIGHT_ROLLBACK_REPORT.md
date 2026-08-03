# NeuralAxe OS — Phase 2M.1B, Gate B10.2
## Read-Only Store Preflight and Rollback Readiness — Committed-State Verification Report

**Scope:** NeuralAxe OS 0.1.0-dev, Bitaxe Gamma / board 601 / BM1370 / ESP32-S3 N16R8 only.
**Verified commit:** `60b6fc1c22a4e1c8ec0e53f68d8c2227a04e7b01` = `v2.14.2-68-g60b6fc1`
(`feat: add fail-closed timed-session store preflight and rollback verifier`).
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-store-preflight`.
**Nature:** read-only verification of the committed state. **No physical preflight has been
performed.** No hardware, COM/USB device, physical NVS, flash dump, LAN, OTA endpoint, NTP server,
DNS resolver or mining pool was contacted.

---

## 1. Result

**PASS.** Every Phase B check ran against the committed tree and passed. The three verdicts are
unchanged and restated in §12.

---

## 2. Committed state

| Item | Value |
|---|---|
| Branch | `neuralaxe-v0.1-timed-pool-sessions-store-preflight` ✔ |
| HEAD | `60b6fc1` = `v2.14.2-68-g60b6fc1` ✔ |
| Parent | `3bbfb07` (Gate B10.1), descending from `d256deb` (Gate B10) ✔ |
| Working tree at verification start | clean ✔ |
| Files in the commit | **17 — exactly the approved set**, no more ✔ |
| Diff shape | 4,044 insertions, 2 deletions (the 2 are the `test/CMakeLists.txt` TEST_COMPONENTS line and one `main/Kconfig.projbuild` line) |

**Committed files**

*New (11):* `components/pool_session_preflight/{CMakeLists.txt, include/pool_session_preflight.h,
pool_session_preflight.c, pool_session_preflight_nvs.c, pool_session_preflight_boot.c,
test/CMakeLists.txt, test/test_pool_session_preflight.c}`,
`docs/NEURALAXE_PHASE_2M1B_B10_2_STORE_PREFLIGHT_PREPARATION.md`,
`tools/pilot/{build_store_preflight.py, verify_rollback_readiness.py,
test_store_preflight_tools.py}`

*Modified (6):* `main/main.c` (+21), `main/Kconfig.projbuild` (+52/−1), `main/nvs_config.c` (+21),
`main/nvs_config.h` (+14), `main/CMakeLists.txt` (+1), `test/CMakeLists.txt` (+1/−1)

---

## 3. Feature flag — default n and mutually exclusive

Verified in the committed `main/Kconfig.projbuild`:

```
config NX_TIMED_SESSIONS_STORE_PREFLIGHT
    bool "Timed pool session READ-ONLY store preflight (EXPERIMENTAL, Gate B10.2)"
    depends on !NX_TIMED_SESSIONS && !NX_TIMED_SESSIONS_EXECUTION && \
               !NX_TIMED_SESSIONS_API && !NX_TIMED_SESSIONS_TIME_OBSERVE && \
               !NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    default n
```

`CONFIG_NX_TIMED_SESSIONS` is in the exclusion list — not only its children. The four sub-flags all
`depends on` the parent, so excluding them alone would still have permitted `NX_TIMED_SESSIONS=y`,
whose Gate B6 bootstrap opens `nx_tps` with `NVS_READWRITE` and **creates** the namespace this
posture exists to inspect without creating. The same five-term rule is enforced by a C-level
`#error` in `pool_session_preflight_boot.c`.

---

## 4. `NVS_READONLY` usage

The committed adapter contains exactly one `nvs_open` call:

```c
err = nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READONLY, &h);
```

`NVS_READWRITE` appears in that file only inside the header comment explaining why the committed
Gate B3 backend must **not** be reused. One namespace (`nx_tps`), three committed keys, no
enumeration, no full-partition read.

---

## 5. Zero `nvs_set` / `nvs_erase` / `nvs_commit` reachability

| Check | Result |
|---|---|
| ESP-IDF write API referenced in preflight **code** (comments stripped) | **0** in all three translation units |
| Only textual hit | a comment stating the absence (`pool_session_preflight_nvs.c:156`) |
| `pool_session_store_commit_record` / `commit_clear` linked in posture P | **0** |
| NVS-write symbol set, posture A vs posture P | **identical** — the preflight adds none |

The mutation ops in the read-only table exist solely because `pool_session_store_init()` requires a
complete table; they are unconditional refusals that count the attempt, and any non-zero counter
makes the classifier fail closed.

---

## 6. Destructive NVS recovery — verified absent

This is the property the Phase A blocker demanded, and it verifies at the strongest available level.

| Posture | `nvs_flash_erase` in the image | Direct call sites | Caller |
|---|---|---|---|
| **A** (shipped default) | linked | 1 | `nvs_config_init` — unchanged upstream behaviour |
| **B** (runtime) | linked | — | unchanged |
| **P** (preflight) | **NOT LINKED** | **n/a — the symbol is absent** | — |
| **D** (B10.1 pilot) | linked | — | unchanged |
| **E** (full stack) | linked | — | unchanged |

In the preflight image there is no call site to be reachable *from*, because there is no function.
This is objdump call-graph evidence, not symbol presence alone.

Committed boot order (`main/main.c`): `nx_tps_preflight_boot_gate()` at line 93 → `return;` at 95 on
failure → `nvs_config_init()` only at line 100. The gate calls `nvs_flash_init()` once with **no
recovery path**; `NO_FREE_PAGES`, `NEW_VERSION_FOUND` and every other error emit
`TPS_PREFLIGHT_BLOCKED_NVS_INIT` and halt boot before any configuration init, Wi-Fi, pool, protocol
or mining.

---

## 7. Fail-closed classification

Verified ordering in the committed classifier — each guard returns before the next:

1. NULL input / model mismatch → `INTERNAL_ERROR`
2. any write/erase/commit counter ≠ 0 → `INTERNAL_ERROR`
3. `nvs_init_failed` → `NVS_INIT_FAILED`
4. **`namespace_open_failed` → `IO_ERROR`** — checked *before* absence
5. `!namespace_present` → `EMPTY` (the only permitting early return)
6. loader-not-run / out-of-range → `INTERNAL_ERROR`
7. record refinement → `CORRUPT` / `TERMINAL_PENDING` / `RECORD_PRESENT`
8. unresolved `restore_required` downgrades any permitting outcome

`NX_TPS_PREFLIGHT_INTERNAL_ERROR = 0` with a `_Static_assert`, so a zeroed verdict blocks;
`_Static_assert(NX_TPS_PREFLIGHT__COUNT == 10)`.

Guard 4 is the fix for the fail-open that adversarial review of Phase A found: `pf_open` previously
collapsed "namespace unreadable" into "namespace absent", so an `nvs_open` failure other than
`NOT_FOUND` would have produced `EMPTY` + `permits_pilot=true` — a pilot authorized on a store
nobody read. `open_failed` is now a separate flag consulted first.

**Unknown / corrupt data never becomes EMPTY or CLEARED** — asserted across every untrustworthy
store result, truncated and corrupt payloads, invalid records, unknown kinds and states, and both
failure modes above.

---

## 8. No execution / API / SNTP reachability

Posture P links **0** symbols matching `pool_session_execution*`, `pool_exec_*`,
`nx_pool_execution_*`, `pool_time_sntp_*`, `pool_time_source_*` or `pool_pilot_*`, and **0**
Gate B3 store-mutation entry points. Postures B/D/E carry their expected counts (22 / 29 / 73),
confirming the audit discriminates rather than always reporting zero.

---

## 9. Default firmware unchanged

| Posture | `esp-miner.bin` | preflight syms |
|---|---|---|
| **A — shipped default** | **1,658,416 B** | **0** |
| B — runtime only | 1,694,160 B | 0 |
| **P — preflight** | **1,664,800 B** (+6,384 vs A) | **8** |
| D — B10.1 pilot | 1,698,656 B | 0 |
| E — full B7+B8 stack | 1,739,664 B | 0 |

Posture A is **byte-identical in size to the committed pre-B10.2 baseline**, and its
`nvs_flash_erase` call graph is unchanged. Every B10.2 change is behind the flag.

---

## 10. Test results (all against the committed tree)

| Suite | Result |
|---|---|
| QEMU firmware | **851 executed, 0 failed, 0 ignored** (806 baseline + **45** B10.2; all 45 `b102` PASS, all 54 `b101` PASS) |
| Frontend `npm run test:gate` | **1270 / 1270 executed, 1270 success, 0 failed** |
| B10.2 tooling battery | **98 / 98** |
| B10.1 tooling battery (regression) | **124 / 124** |
| Release gates (regression) | **13 / 13** |
| Strict `-Wall -Wextra -Werror` | **12 / 12 × 2 postures (P and A)** |

---

## 11. Rollback verifier — exercised against the real package

Run offline against the owner's actual known-good export
(`pool-strategy-polish-v0.1.0-dev-board601`, `v2.14.2-43-gd333dc4`):

| Input | Outcome |
|---|---|
| installed version = `v2.14.2-43-gd333dc4`, `--require-factory` | **`ROLLBACK_READY_EXACT_MATCH`** — 4 artifacts present with matching SHA-256, coherent app/web pair, factory image present and checksummed |
| installed version omitted | `ROLLBACK_BLOCKED_VERSION_UNKNOWN` |
| installed version = `v2.14.2-68-g60b6fc1`, no `--allow-downgrade` | `ROLLBACK_BLOCKED_VERSION_UNKNOWN` |
| same, **with** `--allow-downgrade` | `ROLLBACK_READY_EXPLICIT_DOWNGRADE` — never reported as an exact match |

**The `--installed-version` values above were supplied by hand for verification.** The tool cannot
read a device and does not pretend to; at H0/H1 the owner must supply the version actually read off
the board.

---

## 12. Artifact checksums

**No preflight artifact was built, so none exist to checksum** — building one was explicitly out of
scope for both Phase A and Phase B. What was verified is the *rollback* package (§11): all four of
its artifacts re-hashed on disk and matching the recorded manifest, with the pair confirmed coherent
at `v2.14.2-43-gd333dc4`. Nothing in that directory was written, renamed or overwritten.

---

## 13. Privacy, owner-path and no-network scans

| Scan | Result |
|---|---|
| Owner-local absolute paths across all 17 committed files | **0 hits** |
| `build_store_preflight.py` — networking / device imports, hardware literals | none / none / none |
| `verify_rollback_readiness.py` — networking / device imports, hardware literals | none / none / none (and no `subprocess`, so it cannot invoke esptool or Git) |
| Log-line privacy | swept across every outcome: no namespace, key name, record size, generation, session id, epoch, pool identity, credential or `ESP_ERR` text |

---

## 14. Housekeeping

`test-ci/CMakeLists.txt` (22 B), `test-ci/main/CMakeLists.txt` (84 B) and
`test-ci/main/unit_test_all.c` (638 B) restored byte-exact. `report.xml`, `test-ci/build`,
`test-ci/sdkconfig` and every `__pycache__` removed. All posture builds ran in a work tree outside
the repository. **Final `git status`: clean.**

---

## 15. Verdicts

| Question | Verdict |
|---|---|
| **Software** | **COMPLETE and VERIFIED at `60b6fc1`** |
| **Preflight artifact** | **NOT BUILT** — no binary, no checksum, nothing staged |
| **Physical preflight** | **NOT READY** — H0–H6 unexecuted; H1 needs an owner-supplied installed version |

---

## 16. What must be preserved by later gates

`CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT` default **n** and mutually exclusive with
`NX_TIMED_SESSIONS` **and** its four children; `NVS_READONLY` as the only open mode; the refusing,
counting mutation stubs; the compiled-out `nvs_flash_erase` branch and the non-destructive boot
gate; `INTERNAL_ERROR == 0`; **open-failure checked before absence**; exactly two permitting
outcomes; unknown or corrupt data never becoming `EMPTY`/`CLEARED`; one-shot inspection with no
polling; token-only reporting; and the rule that a preflight result authorizes nothing.

**A preflight build must never be shipped as a production default, and must never be the image a
timed session runs on.**

---

## 17. Open items carried forward

1. The preflight answers the store question **at first boot of the preflight image**, not before
   flashing anything. That is inherent — the only reader of `nx_tps` is firmware. The residual is
   now bounded to one NVS-preserving OTA of an image that provably cannot erase the partition.
2. Whether the rollback pair matches the installed firmware still needs an owner-supplied installed
   version (§11).
3. The Gate B3 loader keeps a decoded record in a file-static scratch inside
   `pool_session_store.c`; the preflight zeroes its own copy but cannot reach that pre-existing
   buffer without modifying B3, which was outside this gate's change boundary.
4. Gate B10.1 open items 1, 2 and 4 stand unchanged.
5. `erase_attempts` is a structural placeholder: `PoolStoreBackendOps` has no erase entry, so
   nothing in production can increment it. Recorded in the header so it is never mistaken for
   evidence that an erase was attempted and refused.
