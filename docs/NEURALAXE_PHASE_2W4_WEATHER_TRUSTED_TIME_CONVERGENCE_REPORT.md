# NeuralAxe OS — Gate W4: Weather / Trusted-Time Convergence and Recommendation-Only Runtime — Phase B Report

Phase: Weather-Aware Tuning, Gate W4 · Branch: `neuralaxe-v0.1-weather-trusted-time-convergence`
Committed implementation: **74a6178** ("feat: converge weather-aware tuning onto trusted time with a default-off recommendation runtime") = **v2.14.2-73-g74a6178**, parent `6de57d6` (v2.14.2-72). 64 files changed, 17,603 insertions, 1 deletion.
Phase B verification date: 2026-08-03 · This report is the only uncommitted file (second owner commit gate).

## 1. Verdict

**Gate W4 FULLY GREEN.** The commit joins the committed W1 profile/climate policy, the W2 crash-safe persistence model and the W3 weather client/scheduler onto the committed Gate B2/B10 trusted-time foundation, through one new pure component `components/weather_runtime/`, behind `CONFIG_NX_WEATHER_AWARE_TUNING` (**default n**).

The runtime is **recommendation-only and authorizes nothing**: no frequency, voltage, fan or thermal write; no pool or protocol change; no session, lease or B5/B7 mutation; no SNTP start; no second trusted-time anchor or epoch floor; no raw wall-clock or Stratum-time trust; no NVS namespace collision; no HTTP route; no task, queue or timer; no restart; no OTA; no hardware access. This is proven at the **binary level**, not only by source inspection (§5, §6).

| Check | Result |
|---|---|
| Committed files | exactly **64** — 60 added + 4 modified |
| `test-ci/` touched by the commit | **0 files** (the rejected partition change stayed out) |
| `docs/` or frontend touched | **0 files** |
| W1–W3 provenance | **50 files byte-exact** vs `neuralaxe-v0.1-weather-aware-tuning-weather-client`; 10 W4-authored |
| Split QEMU — baseline (B1–B10.2) | **851 Tests, 0 Failures, 0 Ignored** |
| Split QEMU — weather (W1–W4) | **254 Tests, 0 Failures, 0 Ignored** |
| **Deduplicated total** | **1105 / 0 failures / 0 ignored** |
| Frontend `npm run test:gate` | **1270 / 1270, exit 0** |
| Posture A (all flags off) | build exit 0 · **0 W4 symbols** · 0 SNTP initializers |
| Posture D (weather ON) | build exit 0 · 31 W4 symbols · **0 SNTP initializers** · **0 static RAM** |
| Posture E (weather + timed sessions) | build exit 0 · **0 weather SNTP symbols, 0 weather anchor writers** |
| Strict compile | **0 failures** across 19 translation units |
| Static RAM impact | **0 bytes** (§6) |
| Privacy / secret / owner-path scan | clean |
| Working tree after verification | clean; `test-ci/` byte-exact; no build output, `dist`, `sdkconfig`, `report.xml`, `__pycache__` or `.pyc` |
| Access | no hardware, pool, network, OTA, restart or external system; all builds and QEMU offline |

## 2. What Gate W4 delivers

**`components/weather_runtime/` (10 new files)** in three parts:

- **`weather_time_view.c`** — the ONE pure projection of committed trusted time. It never constructs a time provider; it *receives* an injectable `PoolTimeClock` ops table and reads `pool_time_snapshot()`. It adds a bounded ceiling on anchor age, and may only ever **refuse** what B2 accepted (`trusted(weather) ⇒ trusted(B2)`). `trusted_time_available` is assigned in exactly one place.
- **`weather_runtime.c`** — the recommendation-only convergence runtime: a bounded state machine over trusted time, the W3 schedule, the W3 provider seam and the W1 climate policy. `WeatherRecommendation` is a value type containing **no pointer of any kind**, so it cannot carry or become an executable command.
- **`weather_runtime_boot.c`** — the flag-gated boot notice and the only logging site.

**Integration (4 modified files)** — `main/Kconfig.projbuild` (new top-level `CONFIG_NX_WEATHER_AWARE_TUNING`, default n, depending on nothing and selecting nothing), `main/CMakeLists.txt` (unconditional `weather_runtime` in `PRIV_REQUIRES`, because ESP-IDF resolves component requirements before any `CONFIG_*` symbol exists), `main/main.c` (a 3-line guarded include plus a 14-line guarded call), and `test/CMakeLists.txt` (the union test list plus the split-run procedure, §4).

## 3. Provenance — W1–W3 ported byte-exact, zero overlap

The weather line forked at Gate B5 (`a31975a`, an ancestor of this branch), so the two histories share exactly one file: `test/CMakeLists.txt`, where the weather branch had *replaced* the timed-session `TEST_COMPONENTS` list. Taking either side would silently drop a whole suite, so W4 resolves it as a **union**. No merge, rebase or cherry-pick was used; every component file was ported by content and re-verified in Phase B by blob comparison:

| Origin | Gate | Files | Source + test |
|---|---|---|---|
| `397f636` | W1 `tuning_profile` | 8 | 5 + 3 |
| `b03c5ac` | W2 `tuning_store` | 12 | 8 + 4 |
| `d3e1869` | W3 `local_schedule` + `weather_client` | 30 | 20 + 10 |
| — | **W4-authored** `weather_runtime` | **10** | 7 + 3 |

**50 byte-exact + 10 new = 60**, plus 4 modified = **64**. Zero W1–W3 source lines modified.

## 4. Split QEMU strategy (owner-directed)

`test-ci/sdkconfig.defaults` must remain byte-exact, so the earlier `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` change was reverted. The union image (17 components) does exceed the stock 1 MB factory partition, so validation runs as **two bounded images** using the `TEST_COMPONENTS` **`CACHE STRING` override the file already documents**, with the build directory *and* sdkconfig outside the repository — no repository file changes at all:

```
idf.py -B <outside-repo> -DSDKCONFIG=<outside-repo> -DTEST_COMPONENTS="<list>" build
```

| Run | Components | Image | Headroom (stock 1 MB) | Result |
|---|---|---|---|---|
| baseline | 12 | 745,712 B (0xb5ef0) | 29% free | **851 / 0 / 0** |
| weather | 5 | 721,536 B (0xb0480) | 31% free | **254 / 0 / 0** |
| **deduplicated** | **17 = the union** | — | — | **1105 / 0 / 0** |

The split is machine-verified equal to the union with an **empty intersection** (`baseline ∪ weather == union`, nothing missing or extra), so the per-run totals add with no double counting. Per-gate attribution of the 254: **W1 80** (`test_tuning_policy` 50 + `test_tuning_profile` 30), **W2 67** (23 + 22 + 22), **W3 68** (`weather_client` 47 + `local_schedule` 21), **W4 39** (`test_weather_runtime` 25 + `test_weather_time_view` 14). All 17 imported test files are byte-exact and every one of the 254 executes — no test is removed, weakened or tag-excluded. `test-ci/CMakeLists.txt` is a committed symlink (mode 120000) that Windows materialises as a 22-byte stub; the build resolves it transiently and restores it verbatim.

## 5. Binary-level isolation

| Posture | Flags | Image | W4 symbols | SNTP init | Static RAM |
|---|---|---|---|---|---|
| **A** | all off (shipped default) | 1,658,416 B | **0** | 0 | 600,784 B |
| **D** | `NX_WEATHER_AWARE_TUNING=y` | 1,668,816 B | 31 | **0** | 600,784 B |
| **E** | weather + timed sessions | 1,703,920 B | — | committed provider only | — |

In posture D the **entire provider/transport/cache/retry stack — including `weather_runtime_fetch` and `weather_open_meteo_http` — is not linked at all** (garbage-collected because nothing reaches it). In posture E, where the committed B2/B10 provider *is* linked, weather contributes **0 SNTP symbols and 0 anchor/epoch-floor writers**; the only NeuralAxe time provider is the committed `pool_time_sntp_*` (15 symbols). W4 sources contain no `xTaskCreate`, `xQueueCreate`, `xTimerCreate`, `esp_timer_create` or `httpd_register_uri`, and the store uses NVS namespace **`nx_wtp`** (static-asserted ≤ 16 chars), distinct from the committed B3 `nx_tps`.

**Posture A — structurally and functionally equivalent, but not bit-identical.** Built against a pristine pre-W4 tree with `PROJECT_VER` pinned identically on both sides, the measurements are:

- **image size and every measured section size are identical** — 1,658,416 B, with `.flash.text` 1,167,160 · `.flash.rodata` 364,652 · `.iram0.text` 99,759 · `.dram0.data` 25,416 · `.dram0.bss` 35,928 (delta 0 in every case);
- **posture A contains zero W4-exclusive symbols, objects or execution paths** — no W4 function, no W4 RAM-resident object and no reachable W4 code path is present;
- **a small number of bytes differ** — exactly 73 — because source-line immediates, build timestamps and derived image hashes changed;
- these are **non-functional metadata and source-location differences**;
- therefore **posture A is structurally and functionally equivalent to the pre-W4 baseline, but not bit-identical**.

| Bytes | Location | Cause |
|---|---|---|
| 2 | `app_main` | `ESP_ERROR_CHECK`'s embedded `__LINE__` source-line immediates for `i2c_bitaxe_init()` (68→71) and `asic_hold_reset_low()` (72→75) |
| 6 | `app_desc.time` | build timestamp |
| 32 | `app_desc.app_elf_sha256` | derived hash — follows from the above |
| 33 | image tail | esptool validation hash — derived |

The source-line residue is unavoidable: the 3-line `#ifdef` / `#include "weather_runtime_boot.h"` / `#endif` block at `main/main.c:38-40` occupies physical lines, so `__LINE__` shifts for every later `ESP_ERROR_CHECK` even though the preprocessor deletes the block itself. The changed immediates carry a diagnostic source-line number used only in an error-reporting path; they alter no control flow, no computation and no stored program datum, and the section sizes above confirm no code or data was added. (An unpinned comparison shows a spurious 48-byte delta that is purely `PROJECT_VER` string length.)

## 6. Exact static RAM impact — 0 bytes

Total linked static RAM is **identical** in postures A and D at **600,784 B**: `.dram0.bss` 35,928 + `.dram0.data` 25,416 + `.ext_ram.bss` 15,136 + `.ext_ram_noinit` 524,304; `.iram0.bss`, `.iram0.data`, `.noinit`, `.rtc_noinit` all 0. Posture D adds **6,943 B of code across 31 W4 symbols** and **zero RAM-resident W4 objects**; the image grows 10,400 B. The 4 KB `WeatherHttpResponse` buffer in the fetch path costs nothing today because that path is not linked.

## 7. Endpoint and source-unconfigured proof

The single `api.open-meteo.com` literal is a compile-time constant inside a **replaceable provider adapter**, not an automatic selection: `weather_provider_get()` returns `NULL` for `UNCONFIGURED`; the shipped `weather_runtime_config_defaults()` sets `expected_provider = UNCONFIGURED` with coordinates 0/0; and `weather_provider_eligible()` keeps the public endpoint ineligible under the default `UNSPECIFIED` distribution (its free tier is non-commercial by the provider's official terms). The boot notice injects **no** clock, transport or store.

With the provider/source unconfigured:

- **zero weather network requests** — `weather_runtime_fetch()` returns before any transport pointer is dereferenced, and in the shipped flag-on image the fetching code is not linked at all;
- **zero policy evaluation** — `weather_runtime_step()` refuses at gate 3 (`source_configured`) before the schedule, profile or climate policy is consulted;
- **zero recommendation** — `refuse()` publishes `present = false` with `provider_result = ERR_UNCONFIGURED`;
- **no location or hostname logging** — the sole log site emits `state=/recommendation=/reason=` machine tokens plus a fixed `authority=none hardware=unchanged pool=unchanged session=none sntp=not_started` line.

A committed privacy test additionally asserts the built request URL contains none of `key, apikey, api_key, token, auth, secret, password, user, worker, wallet, serial, mac, device, uuid, hostname, session, client, hashrate, asic, firmware`.

## 8. Known limits

- **Recommendation-only by design.** Applying a weather recommendation to hardware requires a separate, explicitly scoped future execution gate that does not exist yet. Nothing in W4 authorizes it.
- **No physical validation.** No Gamma hardware, pool, live DNS, real NTP or network was accessed; the weather client is exercised with fake transports only.
- **The public Open-Meteo endpoint stays ineligible** under any distribution mode except an explicit personal/non-commercial one; commercial or self-hosted deployments require their own adapter.
- **Bounded verification only.** QEMU + host tooling; the boot notice reports `WAITING_FOR_TRUSTED_TIME` by construction because no clock is injected.

## 9. Phase B verification appendix

All commands ran offline against the committed tree (`74a6178`, clean status), `espressif/idf:v5.5.3` for builds and the `nx-qemu-action` image for QEMU only. Git was used read-only throughout; the owner performed the commit.

1. `git show --name-status HEAD` — 64 files, 60 A + 4 M; `test-ci/`, `docs/` and frontend untouched.
2. Blob comparison vs the origin weather branch — 50 byte-exact, 10 new.
3. Two bounded builds via the documented `TEST_COMPONENTS` override, build dir and sdkconfig outside the repo — both exit 0, both inside the stock 1 MB partition, `SINGLE_APP_LARGE` absent from both generated sdkconfigs.
4. Split QEMU — 851/0/0 and 254/0/0; deduplicated 1105; per-file attribution reproduced exactly.
5. `npm run test:gate` — 1270/1270, exit 0.
6. Postures A, D and E from pristine sdkconfig copies (tracked `sdkconfig` untouched) — exit 0 each; symbol and SNTP counts as tabled.
7. Exact static-RAM accounting via `size -A` + `nm -S` — delta 0.
8. Byte-identity comparison against a pristine pre-W4 tree with `PROJECT_VER` pinned — 73 differing bytes, fully attributed.
9. Strict compile of 19 W4-touched translation units — 0 failures.
10. Privacy, secret, owner-path and namespace scans — clean.
11. `test-ci/` restored byte-exact; all build artifacts removed; tree clean.
