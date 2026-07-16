# NeuralAxe OS — Phase 2A Branding Report

**Phase:** Phase 2A — Product Identity, Branding and Read-Only Build Metadata
**Date (UTC):** 2026-07-15
**Author:** Firmware identity & frontend branding engineering (automated session)

---

## 1. Verdict

## **PASS**

NeuralAxe OS identity is visible and coherent across the web UI, firmware log, and system-info API; NeuralAxe and upstream versions are represented separately with upstream semantics intact; all API additions are additive, optional and read-only; GPL-3.0 licensing and upstream attribution are preserved; all tests pass (frontend 42/42, firmware 61/61); AxeOS, the firmware and the merged image build successfully; partition headroom is unchanged at 61%; no generated file remains as a repository change; no secrets entered the artifacts; no physical hardware was accessed; and no frozen firmware behavior was changed.

---

## 2. Context

| Item | Value |
|---|---|
| Repository / path | `AegisAI-Dev/NeuralAxe-OS` at `D:\Companys\Neuralshield\Software\NeuralAxe-OS` |
| Branch | `neuralaxe-v0.1-branding` (verified via `.git\HEAD`; no git commands run) |
| Resolved commit before modifications | `6cd795d0b611e80d52dd4a54936d403d5d45774e` (v2.14.2 baseline `64680f8a` + Phase 1 report commit) |
| Verified upstream baseline | ESP-Miner / AxeOS **v2.14.2** = `64680f8a4da0b9a3b532051f0aa18429fcf04e82` |
| Target | Bitaxe **Gamma**, board **601**, **BM1370**, ESP32-S3 N16R8 |
| Product | **NeuralAxe OS 0.1.0-dev**, development channel, NeuralShield |

**Scope (exact):** user-facing identity (title, topbar, footer, About rows, update-page copy, favicon/mark, README), one theme accent variable, additive read-only build metadata (frontend const, firmware header, 8 optional system-info JSON fields + OpenAPI schema + dev mocks), startup-log identity lines, and tests for all of the above. Nothing else.

---

## 3. Change Map / Exact Files Changed

**Modified (13):**

| File | Purpose | Behavior impact |
|---|---|---|
| `axe-os/src/index.html` | Title "NeuralAxe OS", app-name/description meta, SVG icon link (ico fallback kept) | cosmetic |
| `axe-os/.../app.topbar.component.html` | NeuralAxe mark + wordmark + "0.1.0-dev · Development Build" tag (upstream AxeOS SVG removed from brand slot) | cosmetic; all controls/hostname kept |
| `axe-os/.../app.footer.component.html` | Footer enabled: identity + upstream attribution + GPL note + target | cosmetic (was empty) |
| `axe-os/.../app.footer.component.ts` | Exposes `NEURALAXE` const to template | none |
| `axe-os/.../system.component.ts` | 4 additive About rows (Product, Build Channel, Based On, NeuralAxe Target) | additive rows only |
| `axe-os/.../update.component.html` | Informational dev-build/upstream note | copy only; update behavior untouched |
| `axe-os/.../themes/vela/bitaxe/_variables.scss` | `$primaryColor` `#f80421` → `#16c784` (green accent; base was already dark navy `#070D17`) | cosmetic accent |
| `axe-os/src/app/services/system.service.ts` | Dev-mock parity for 8 additive fields | dev mocks only |
| `axe-os/src/app/services/system.service.spec.ts` | +2 specs: system-info regression, additive metadata | test only |
| `main/http_server/openapi.yaml` | 8 additive **optional** SystemInfo properties (none added to `required`) | schema doc |
| `main/http_server/system_api_json.c` | 8 additive `cJSON_AddStringToObject` fields in `/api/system/info` | additive JSON |
| `main/main.c` | 2 identity `ESP_LOGI` lines added **after** the retained upstream banner | log text |
| `readme.md` | NeuralAxe intro + attribution + dev-build pointer; full upstream README retained below | docs |

**Created (6):** `axe-os/src/app/neuralaxe.ts` (frozen metadata const), `axe-os/src/app/neuralaxe.spec.ts`, `axe-os/src/app/layout/app.footer.component.spec.ts`, `axe-os/src/assets/neuralaxe-mark.svg` (original vector mark), `main/neuralaxe_identity.h` (deterministic `#define`s, no timestamps), `docs/NEURALAXE_PHASE_2A_BRANDING_REPORT.md` (this file).

**Removed:** none. **Renamed:** none. No file under `components/` (asic/stratum/power/thermal), no NVS/OTA/partition/Wi-Fi/self-test/display code, and no build-system file was touched.

---

## 4. Product Identity Implementation

- **Browser title:** "NeuralAxe OS"; SVG favicon (`assets/neuralaxe-mark.svg`) with `.ico` fallback.
- **Topbar:** original NeuralAxe mark (stylized axe head + haft doubling as an N stroke, cyan circuit-node motif, dark navy tile — hand-authored SVG, no external artwork, no rasters, legible at 16 px) + "NeuralAxe OS" wordmark + dev tag.
- **Footer:** "NeuralAxe OS 0.1.0-dev (Development Build) — NeuralShield · NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects. Upstream: ESP-Miner / AxeOS v2.14.2 (GPL-3.0). Target: Gamma 601 / BM1370."
- **System/About:** 4 additive rows below the untouched Firmware/AxeOS/ESP-IDF version rows.
- **Update page:** informational note that upstream-format images are installed and an official upstream release will replace NeuralAxe branding; upload/OTA behavior untouched.
- **Startup log:** upstream "Welcome to the bitaxe - FOSS || GTFO!" retained, followed by `NeuralAxe OS 0.1.0-dev (development) - NeuralShield` and `Based on ESP-Miner / AxeOS v2.14.2 | Target: Gamma 601 / BM1370`.
- **Visual direction:** base surfaces were already very dark navy (`#070D17`/`#0B1219`); the only color change is the primary accent red→green `#16c784`, with cyan `#22d3ee` as a subtle secondary in the mark. No animations added, no fonts added (local Nippo retained), no external resources of any kind.
- **Web manifest:** none exists upstream; none added (documented as N/A).

## 5. Firmware Metadata Implementation

`main/neuralaxe_identity.h` defines: productName "NeuralAxe OS", productVersion "0.1.0-dev", buildChannel "development", vendor "NeuralShield", upstreamProject "ESP-Miner / AxeOS", upstreamVersion "v2.14.2", targetBoard "601", targetDevice "Gamma", targetAsic "BM1370". All values are compile-time string literals — deterministic, no timestamps (the baseline embeds none either).

`GET /api/system/info` now additionally returns these 8 fields (vendor appears in UI only). `version`, `axeOSVersion`, `idfVersion`, `boardVersion` are emitted by the same unchanged code lines as before. The `/api/ws/live` partial stream and every other route are untouched.

## 6. API Compatibility Analysis

- All 8 new SystemInfo properties are **optional** (not added to the OpenAPI `required` list) and **read-only** (GET response only; no new endpoints, no mutation paths, no auth, no telemetry).
- No existing field, route, NVS key, config field or OTA version field was renamed, removed or retyped.
- Clients that ignore unknown JSON fields (all existing clients) are unaffected; the regenerated TypeScript client compiles with the fields as optional strings.
- `targetBoard/targetDevice/targetAsic` are **declared build targets**, distinct from the runtime-detected `boardVersion`/`ASICModel` fields, and documented as such in the schema.

## 7. Licensing and Attribution Analysis

GPL-3.0 `LICENSE` untouched. Upstream copyright and acknowledgements untouched (README retains the full original ESP-Miner README below the NeuralAxe introduction; the display-font attribution and all upstream links remain). The required statement — "NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects." — appears in the README, the UI footer, `neuralaxe.ts`, and `neuralaxe_identity.h`. The NeuralAxe mark is original hand-authored geometry; no commercial logo copied, nothing downloaded. Nothing misrepresents the upstream work as original.

## 8. Test and Build Results

| Gate | Result |
|---|---|
| Frontend tests | **42/42 PASS** (33 baseline + 9 new: identity const, footer attribution, mock/API metadata, system-info regression) — Karma/Jasmine, Edge 150 headless via `CHROME_BIN` (Chrome not installed; same Chromium-based substitution as Phase 1, documented) |
| Firmware tests | **61 Tests, 0 Failures, 0 Ignored** in QEMU 9.2.2 `-machine esp32s3` — identical suite and totals as the v2.14.2 baseline run |
| AxeOS build | SUCCESS — 2.20 MB initial / 402.75 kB transfer; `version.txt` = `v2.14.2-1-g6cd795d-dirty` (upstream git-describe semantics untouched, honestly reflecting the branding working tree) |
| Firmware build | SUCCESS — ESP-IDF v5.5.2 devcontainer, 0 errors, 0 new warnings in modified files |
| Merged image | SUCCESS — 0xf12000 (15,802,368) bytes via unmodified `merge_bin.sh` (CRLF-normalized temp copy, as in Phase 1) |

**Warnings grouped:** libsecp256k1 headers ~226 / FreeRTOS `atomic.h` ~9 — benign `-Wunused-function`, unchanged from baseline (not rebuilt in the incremental run, none introduced); Angular bundle budget 1 warning, pre-existing, marginally improved (104.13 kB over vs 106.00 kB); npm transitive deprecations only. **Errors: 0.**

## 9. Artifacts and Baseline Comparison

New artifacts: `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\v0.1.0-dev-board601\` (binaries, `config-601.cvs`, 7 logs, `SHA256SUMS.txt`, `BUILD_INFO.txt`, `ARTIFACT_MANIFEST.txt`, `BASELINE_COMPARISON.txt`). Marked **development build — not approved for physical flashing**.

| File | Size (bytes) | Δ vs baseline | SHA-256 (new) |
|---|---:|---:|---|
| `esp-miner-merged.bin` | 15,802,368 | 0 | `c1cb393fbbae16d7f41e12b0a2a847f1931a398bebabf8f527b20f853c3bcf65` |
| `esp-miner.bin` | 1,645,440 | **+592** | `259b5eeaf0b10bbe97a055d2de9f3477f55a2a52c7a6fb6add4521fc39974d77` |
| `www.bin` | 3,145,728 | 0 (fixed 3M image) | `4ff45eaef2e0f64947147399a1ceab5b13c6a0e729433683222338508f4b9878` |
| `bootloader.bin` | 22,432 | byte-identical | `9334fa49…fd01` (= baseline) |
| `partition-table.bin` | 3,072 | byte-identical | `392125fd…2dc3` (= baseline) |
| `ota_data_initial.bin` | 8,192 | byte-identical | `7d2c7ac4…c62f` (= baseline) |
| `config-601.cvs` | 975 | byte-identical | `3c0f28f6…5f22` (= baseline) |

App partition headroom: **61% free**, unchanged (0x26e480 vs baseline 0x26e6d0). The byte-identical bootloader, partition table, OTA data and NVS config are direct evidence that flash layout, OTA selection and NVS provisioning are untouched. Baseline reference: `…\NeuralAxe Build Artifacts\v2.14.2-board601`; historical 9f18b7d archive unmodified at `…\9f18b7d-post-v2.14.2-board601`.

**Secrets scan:** binaries scanned for local paths/credentials; only hits are the upstream NVS key-name literals (`wifipass`, `stratumpass`), verified present in the untouched baseline binary. `report.xml`, `node_modules`, `dist`, `build/` outputs all remain outside git tracking (preserved to the artifact directory where relevant, then removed from the tree).

## 10. Compliance Confirmations

- **No physical hardware was accessed or modified**: no COM/USB, no flashing, no esptool/bitaxetool against hardware, no live miner IP contacted. QEMU only. Development binaries are not to be flashed before Phase 2B review.
- **The private flashdump directory was never accessed.**
- **No frozen behavior changed**: BM1370 init/drivers, frequency, core voltage, power management, fan control (auto and manual), temperature targets/limits, thermal shutdown, self-test, Stratum V1/V2, pool logic, Wi-Fi, NVS keys/behavior, OTA logic/partition selection, partition table/flash layout, recovery, display hardware, mining calculations, share validation and error calculations are all byte-for-byte or line-for-line untouched (see change map — no file in those subsystems was edited; bootloader/partition/OTA/NVS artifacts are hash-identical to baseline).
- **No git command** was run; branch verified by reading `.git` files only.

## 11. Recommendation — Phase 2B Pilot-Flash Readiness

Phase 2A is complete and green. Before a Phase 2B pilot flash of the tuned Gamma, recommend: (1) owner commits the branding changes in GitHub Desktop so the build is reproducible from a clean commit (current binaries were built from a `-dirty` tree — rebuild from the commit for release-grade provenance); (2) a human visual pass of the branded UI in a browser (`npm run start` dev server) — automated tests verify text identity, not aesthetics; (3) keep the v2.14.2 baseline merged image on hand as the tested rollback; (4) pilot-flash procedure, when authorized, should target the tuned Gamma only, never the stock reference device. From the firmware side there is no known blocker: images are upstream-format, layout-identical, and behavior-frozen.
