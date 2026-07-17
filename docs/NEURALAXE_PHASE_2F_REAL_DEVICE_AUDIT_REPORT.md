# NeuralAxe OS — Phase 2F Real-Device Audit Report

**Phase:** 2F — Real-Device Product Audit, Navigation Architecture, Version-Pair Integrity, RC Hardening
**Date (UTC):** 2026-07-17
**Author:** Product/release engineering (automated session)
**Pilot input:** Live Gamma 601 running the Phase 2E pair; firmware `v2.14.2-13-g388287da`, reported web `v2.14.2-9-gc630e1a`, visible 2E UI; Settings 625 MHz / 1150 mV (Default); measured ~1.14 V.

---

## 1. Verdict

## **PASS**

The firmware/web mismatch root cause is proven from source and binaries (boot-time snapshot after a no-restart web OTA; the installed pair was correct); release tooling now rejects mismatched, dirty, stale or ambiguous pairs and the first pair-gated build passed (`PAIR OK: firmware and web both at v2.14.2-15-g723e61dc`); voltage semantics are resolved with no value changed (1150 mV is the genuine board-601 default and a genuine persisted setting; configured mV vs measured V now clearly separated); the navigation is a grouped NeuralAxe product architecture with Classic demoted to a clearly-labeled Legacy Dashboard; Pools and Tuning & Thermal are materially improved; board 702 is nowhere represented as supported; screenshots are privacy-safe (hidden-by-default fixed, scan CLEAN 12/12); frontend 134/134 across four runs; firmware 61/61; a clean matching build pair with no `-dirty`; no protected firmware behavior changed; no hardware accessed. The listed hardware-only confirmations remain as owner follow-ups and do not gate this phase's criteria.

## 2. Source State

Branch `neuralaxe-v0.1-real-device-audit`, created from `neuralaxe-v0.1-product-polish` @ `8856ccba`. Phase A was implemented uncommitted per the stage-gate workflow; the owner committed the reviewed change set in GitHub Desktop as **`723e61dc`** — *"feat: harden NeuralAxe version pairing and navigation"* — verified by reading `.git` metadata only (no git commands were run by the automation at any point). All Phase B builds are from this commit.

## 3. Firmware/Web Mismatch — Root Cause (proven)

`axeOSVersion` is a **boot-time snapshot**: `SYSTEM_init_versions` (main/system.c) reads `/version.txt` from the www SPIFFS partition once at boot and keeps it in RAM. A web-only OTA (`POST /api/system/OTAWWW`) rewrites the www partition and deliberately does not restart the device. The pilot's recommended two-step sequence therefore produced exactly the observed state:

1. `esp-miner.bin` uploaded → device restarts → boots 2E firmware while the www partition still holds 2D.1 → snapshot = `v2.14.2-9-gc630e1a`;
2. `www.bin` uploaded → new 2E UI serves immediately from flash → snapshot never refreshed.

Binary-level proof: the exported Phase 2E `www.bin` embeds exactly one revision string, `v2.14.2-13-g388287da` — the installed pair on the device is the correct matching 2E pair; only the displayed boot snapshot is stale. A plain restart reconciles it (on-device confirmation: REQUIRES CONTROLLED HARDWARE VALIDATION). Browser cache was ruled out (the stale value is delivered by the firmware API and matches the pre-update boot state exactly).

A second, latent defect was found during the trace: `version.txt` regeneration was stamp-gated on frontend-source mtimes, so a commit touching no frontend source could pack a stale web revision while the firmware `app_desc` embedded the new `git describe` — a genuinely mismatched pair at build time.

## 4. Version-Pair Solution

| Layer | Change |
|---|---|
| Build | `main/CMakeLists.txt`: `version.txt` is refreshed from git state on every build, ordered after the web build and before the SPIFFS image. |
| Export | `tools/release/export_release.py` extracts the revision **from inside the staged `www.bin`**; fails on firmware≠web, `-dirty` in either, stale `dist/version.txt` vs packed artifact, missing or ambiguous embedded revision; adds `--check-pair` and `--screenshot-scan`; manifest records `firmwareRevision` + `webRevision`. |
| Validate | `tools/release/validate_manifest.py` requires both revisions, equal and clean, and rejects machine-local path fragments. |
| UI | New `WebVersionService` fetches the live `/version.txt` (the installed artifact's own identity). Device Status shows Firmware Revision, Web Revision (installed), and — only when it disagrees — Web Revision (at boot) with an honest restart-pending note. The dashboard banner distinguishes a genuine artifact mismatch (warning) from web-updated-restart-pending (informational). Values are never substituted; without the live file the UI falls back to the boot snapshot without faking equality. |
| Tests | `tools/release/test_release_gates.py`: 13-case battery (matching pass; mismatch/dirty/stale/none/multi rejected; manifest gates) — validated against the real 2E and 2D.1 artifacts, including rejection of the exact live-device combination. Frontend: banner battery (5 cases), WebVersionService (4), Device Status version rows (3). |

## 5. Voltage Findings — 1100 vs 1150 mV Conclusion

The complete path was traced: NVS key `asicvoltage` (u16, mV) → `/api/system/info.coreVoltage` (configured) vs `coreVoltageActual` (measured telemetry); family default `ASIC_BM1370.default_voltage_mv = 1150`; provisioning `config-601.cvs` writes 1150/525; Kconfig NVS-absent fallbacks are **1400 mV / 250 MHz**.

**Conclusion:** the device's 625 MHz / 1150 mV are genuine persisted NVS values (neither matches the absent-key fallbacks), preserved across OTAs exactly as the update path promises. **1150 mV is the legitimate BM1370/board-601 default — the earlier "625/1100" expectation was an inaccurate assumption about current NVS state** (1100 was TCH-era tuning lore). Measured ~1.14 V is consistent with 1150 mV configured under load/regulation droop. Nothing was changed: no NVS write, no default change, no automatic tuning. Which historical write persisted 1150 (provisioning vs a Settings save) and whether 1100 was ever stored under upstream's key: **REQUIRES CONTROLLED HARDWARE VALIDATION**. The UI now separates **Core Voltage (mV) — configured** from **Measured ASIC Voltage (V)** everywhere (Tuning & Thermal shows both, plus a live measured line that never writes into the form; spec-proven, including 1100/1150/custom dropdown cases and NaN guards).

## 6. Navigation Architecture (before → after)

Upstream flat page list → grouped NeuralAxe IA; route paths unchanged (deep links stable); document titles renamed.

- **OVERVIEW** — Command Deck (`/`)
- **MINING** — Scoreboard (`/scoreboard`), Fleet (`/swarm`)
- **CONFIGURATION** — Pools (`/pool`), Network (`/network`), Tuning & Thermal (`/settings`), Display & Appearance (`/design`)
- **SYSTEM** — Device Status (`/system`), Logs (`/logs`), Updates (`/update`)
- **PRODUCT** — About NeuralAxe (`/about`)
- **ADVANCED** — Legacy Dashboard (`/classic`, titled "Original upstream-compatible monitoring interface")

Whitepaper: reachable from About; no top-level entry. Section headings are rendered by the existing menuitem root-text mechanism (small-caps, dim); active-route highlighting, keyboard tabbing and the mobile drawer are unchanged. Spec-locked (6 cases: grouping, route uniqueness, product names, legacy demotion, no whitepaper top-level, no upstream branding).

## 7. Pools & Settings Changes

**Pools:** live Current Status summary (Primary/Fallback cards, ACTIVE/STANDBY from `isUsingFallbackStratum`, response time attributed to the active pool only, masked compact endpoint/worker with the full value retained in the form for editing); side-by-side editors at xl; field-level validation messages for host/port/user; Unsaved-changes / Saved pills; explicit Save-vs-Restart consequence text. All API calls, values and Save/Restart semantics untouched.

**Tuning & Thermal (Settings):** two-column desktop (Mining Core | Thermal Control; Display & Device | Statistics), stacked on mobile; units on every control (MHz, mV, °C, %, °); overclock warning kept beside frequency/voltage; live measured lines (V, MHz, temps, RPM) clearly labeled telemetry; save/restart enablement unchanged.

## 8. Scoreboard, Fleet & Device Status Changes

**Scoreboard:** sticky header inside a bounded scroll area, hover/focus row states, restrained top-3 rank badges, per-column explanatory tooltips, mobile swipe hint; share data and sorting untouched. **Fleet:** renamed from Swarm; per-device classification badges — NeuralAxe-managed (additive `productName` present), compatible AxeOS device, unsupported NeuralAxe target (board ≠ 601; explicitly *not* implying board-702 support); aria-labels on all action icons; Remove separated and styled as an outlined destructive action with an honest tooltip (removes the list entry only). Discovery/control behavior exact. **Device Status:** version identities separated (product / firmware / installed web / boot web / upstream), copy actions on version rows, readable reset-reason labels with the raw firmware string in the tooltip (unknown values pass through), Measured ASIC Voltage row. No telemetry invented.

## 9. Privacy Solution

Option B (development-plus-production sensitive-data mode, capture discipline documented):

- **Defect fixed:** `SENSITIVE_DATA_HIDDEN` treated a missing preference as "visible" (`getBool` folds missing→false). Fresh profiles — exactly the screenshot scenario — started unmasked; Phase 2E's own dev captures prove it. Now: missing → **hidden**; an explicit stored user choice always wins; production owners see real values after one toggle, configuration is never altered.
- **Coverage extended:** Device Status IPv4; Command Deck pool URL + worker; Pools status endpoint/worker and Stratum Host inputs; Network SSID input; Fleet hostname/IP in both views (User inputs were already covered upstream).
- **Checklist:** `docs/NEURALAXE_SCREENSHOT_PRIVACY_CHECKLIST.md` (pre/post-capture steps, forbidden content, leak response).
- **Scan:** visible-DOM-text oracle per route (elements under `[sensitive-data]` excluded because they are pixel-redacted) for MAC/IPv4(strict-octet)/IPv6/wallet patterns — **CLEAN 12/12 routes**; one documented false positive (concatenated version strings) correctly rejected by strict octets. RC exports accept a screenshots gate via `--screenshot-scan`.

All Phase B visual-review captures were taken from the dev server (synthetic data) with hiding in its default ON state.

## 10. Tests

| Gate | Result |
|---|---|
| npm ci | exit 0 (clean install) |
| Frontend tests | **134/134 PASS, exit 0** (gate: >106; 106 → 134, +28 specs) |
| Reliability runs | **3/3 at the committed tree, 134/134 each** (+ Phase A final run = 4 consecutive green) |
| Release-gate battery | **13/13 PASS**, rerun at commit `723e61dc` |
| Firmware/QEMU tests | **61 Tests, 0 Failures, 0 Ignored** (container `qemutest9`; serial log preserved) |

## 11. Builds & Hashes

Clean identity **`v2.14.2-15-g723e61dc`** in all three places that matter: `version.txt` (container-refreshed), the `esp_app_desc_t` of `esp-miner.bin` (`APP DESC OK`), and the revision embedded inside `www.bin` (`PAIR OK`). The build log shows the new `[7/12] Refreshing web version.txt from git state` step running before the SPIFFS image — the build-order fix is active in the pipeline. Firmware build exit 0 with **0 compiler warnings**; production frontend build carries only the 16 pre-existing NG8102 template warnings (identical set since Phase 2D.1 — none from Phase 2F code). Export: **EXPORT OK** (4 artifacts) with `firmwareRevision = webRevision = sourceRevision` in the manifest; **VALIDATION OK** (board Gamma/601/BM1370); `--screenshot-scan` gate verified; secrets scan **CLEAN** (exit 0).

| File | Size (B) | SHA-256 |
|---|---|---|
| `esp-miner.bin` | 1,645,472 | `52336c3edc56406469c3c226d6a47f1522ee655f9764f60d548bb5bcd375a16f` |
| `www.bin` | 3,145,728 | `7d996645380c6be249aa2c6b18524ae86778a30872a7bd8a7d2e47a5b33dc8d5` |
| `esp-miner-merged.bin` | 15,802,368 | `6a5bd16dfc752eb5f62d3e191e6d9e82dc51867130526b5e85f5fb54669ac0f0` |

`bootloader.bin` (`b3d1001b…`), `partition-table.bin` (`392125fd…`) and `ota_data_initial.bin` (`7d2c7ac4…`) remain **byte-identical to the v2.14.2 baseline** — the partition layout and OTA-selection mechanics are untouched. Full sums in `SHA256SUMS.txt`; board-601-named copies + manifest in `release-export/`.

## 12. Visual Review

19 sanitized captures (12 desktop 1440×900, collapsed-nav 960px, 6 mobile) in the artifacts directory; full findings in `VISUAL_REVIEW.txt`. Result: grouped hierarchy and active states correct; Legacy Dashboard clearly demoted; two-column layouts remove the prior horizontal emptiness; no clipping/overlap; destructive actions separated; private data redacted by default; zero console errors on a full route sweep; measured-vs-configured clearly separated; the dev server's missing `/version.txt` exercises the honest boot-snapshot fallback path.

## 13. Unresolved Hardware-Only Validations

1. Restart the pilot Gamma 601 and confirm the boot snapshot reconciles (web reported = installed = `v2.14.2-13-g388287da`), and that the new UI then reports the restart-pending note gone.
2. NVS/API inspection to date the 1150 mV write provenance (owner-gated).
3. Phase 2C's still-open validations: recovery-page reachability, factory-wipe/provisioning confirmation.
4. On-device OTA of the Phase 2F pair (owner-executed) — also validates the new installed-web-revision display against real hardware.

## 14. RC-Readiness Recommendation

**READY FOR THE RC PILOT.** Recommend the owner OTA this pair (`www.bin` + `esp-miner.bin`, hashes above) to the tuned Gamma 601 and **restart once** — that single restart simultaneously validates the boot-snapshot reconciliation on hardware, the new Device Status version display, and settings retention (625 MHz / 1150 mV expected to persist). If the on-device checks pass and the remaining Phase 2C validations are closed, tag `v0.1.0-rc1` from this commit line and publish the first board-601-marked release to `AegisAI-Dev/NeuralAxe-OS`. Per phase instructions, **no RC was tagged or published in this phase**, and this build is not labeled a public release. Full assessment: artifact `RC_READINESS.txt`.

## 15. Hardware Confirmation

No COM/USB access, no flashing, no esptool/bitaxetool against hardware, no miner IPs contacted (192.168.50.227 never accessed), the private TCH flashdump never inspected or referenced beyond documentation. Source analysis, mock data, previously exported build artifacts, QEMU/containers and the local dev server only. Git state was only ever *read* from `.git` metadata; all commits were made by the owner in GitHub Desktop.
