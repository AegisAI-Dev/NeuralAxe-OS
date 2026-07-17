# NeuralAxe OS — Phase 2E Product Polish Report

**Phase:** 2E — Product Polish, Secondary Pages, Update Experience, Release-Candidate Preparation
**Date (UTC):** 2026-07-17
**Author:** Product/frontend engineering (automated session)
**Baseline:** Phase 2D.1 PASS (Command Deck proven on the tuned Gamma 601)

---

## 1. Verdict

## **PASS**

Every page now reads as one NeuralAxe product: the secondary pages (Classic, Scoreboard, Swarm, Logs, System, Pools, Network, Theme, Settings, Update, new About) share the NeuralAxe shell with small-caps page titles, descriptions, grouped sections, empty states and guarded live values; the Update page is fully NeuralAxe-specific (single repository, manual-only checks, distinct offline/rate-limit/no-release states, explicit board-601 compatibility with board-702 claims rejected); all 106 frontend tests pass (4 consecutive green runs); firmware tests remain 61/61; the firmware binary identity is clean (`v2.14.2-13-g388287da`, no `-dirty`, verified in the app descriptor); production, firmware, merged and release-export builds all pass; no protected firmware behavior changed; no hardware was accessed.

## 2. Source State

Branch `neuralaxe-v0.1-product-polish` (parent `neuralaxe-v0.1-dashboard` at `c3c0a30`). Three commits this phase:

| Commit | Subject |
|---|---|
| `e081f98` | feat: polish secondary pages into one NeuralAxe product |
| `2b8c935` | chore: rebrand dev-mock hostname to neuralaxe-dev |
| `388287d` | fix: clear NG0100 on Settings/Network and align Logs page title |

Final build identity **`v2.14.2-13-g388287da`** — clean, verified in `version.txt` **and** in the `esp_app_desc_t` of `esp-miner.bin` (export gate `APP DESC OK`). (The container git abbreviates commit `388287da…` to 8 hex digits; the host's `git describe` shows the same commit as `-g388287d`.)

## 3. Files Changed (exact)

34 frontend files (33 modified + About component ×3 created); **zero firmware C sources; zero protected behavior**. Full stat in artifact `CHANGED_FILES.txt`.

- `app-routing.module.ts`, `app.module.ts`, `layout/app.menu.component.ts(+spec)` — About route/menu entry
- `components/about/about.component.{ts,html,spec.ts}` — new About page
- `components/update/update.component.{ts,html,spec.ts}`, `services/github-update.service.ts` — Update rework
- `components/system/system.component.{ts,html,spec.ts}` — grouped device info
- `components/settings/settings.component.{ts,html}`, `components/edit/edit.component.html` — settings grouping/units
- `components/logs/logs.component.{ts,html,scss}` — logs polish + `neuralaxe-logs.txt`
- `components/pool/pool.component.html`, `components/network/network.component.{ts,html}` — headers/descriptions
- `components/design/{design.component.html,theme-config.component.ts}` — theme page polish
- `components/swarm/swarm.component.html`, `components/scoreboard/scoreboard.component.html` — descriptions/empty state
- `components/home/home.component.ts` — version-mismatch wording (web interface)
- `components/command-deck/command-deck.styles.scss` — shared page-desc/card-title/section/kv/note primitives
- `pipes/{hash,diff,byte}-suffix.pipe.ts(+specs)` — NaN/±Infinity guards
- `services/system.service.ts` — dev-mock hostname only

## 4. Page-by-Page Improvements

| Page | What changed |
|---|---|
| **Update** | Rebuilt as NeuralAxe update center: This-Device identity block (product, channel, firmware, web interface, OTA partition, target board — with pending-data fallback), NeuralAxe Releases card (manual check, privacy modal, board-601 compatibility pill: confirmed/unknown/**mismatch — downloads withheld for board-702-only claims**), distinct offline vs rate-limit vs no-release states, renamed upload cards ("Install Web Interface"/"Install Firmware") with plain-language explanations, image-type explainer (www.bin / esp-miner.bin / factory image), upstream-replaces-NeuralAxe warning. OTA endpoints, upload mechanics and cold (user-triggered-only) release check unchanged. |
| **System** | Grouped device information: NeuralAxe Product / Firmware & Software / Hardware / Runtime / Memory / Network cards + Diagnostics card (fault rows only when firmware reports one, identify-device with explanation). Every value guarded (em dash for missing/invalid); Running Partition, hostname, ASIC/VR temperature added from existing telemetry only. |
| **Settings** | Grouped into Mining — Frequency & Voltage / Thermal & Fan / Display & Device / Statistics with units (MHz, mV, °C, %, °); overclock warning stays beside frequency/voltage; overheat + low-fan warnings kept adjacent; page description points to Pools/Network pages. Same form controls, validators, values, API calls and save/restart behavior. |
| **Pools** | Page header + privacy-conscious description; Primary/Fallback section headings; default-address warning, advanced options, SV2/TLS controls unchanged. |
| **Network** | Page header + description (password stored on device, never displayed); NG0100 fixed. |
| **Logs** | Page title aligned to shell; description; waiting-for-output empty state; softened readable ANSI palette (pure blue was illegible on dark); rounded padded container; download renamed `neuralaxe-logs.txt`; ₿ prefix → neutral ›. |
| **Theme** | Description (green = NeuralAxe default, saved choice wins); "Green (Default)" label; mobile-friendly swatch grid (col-4/sm:col-2). All 5 themes + dark/light functional. |
| **Swarm / Scoreboard** | Descriptions; scoreboard no-shares empty state; existing scan/add/sort/refresh controls untouched. |
| **Classic** | Left intact (proven layout); only the version-mismatch banner now says "web interface" instead of AxeOS. |
| **About (new)** | NeuralAxe OS product/status/project/target; GPL-3.0 + ESP-Miner/AxeOS attribution with corresponding-source note; explicit "not an official Bitaxe product" statement; locally served Bitcoin whitepaper link. Whitepaper menu item preserved. |

## 5. Update Channel Outcome

`GithubUpdateService` still queries **only** `https://api.github.com/repos/AegisAI-Dev/NeuralAxe-OS/releases` (no upstream fallback, prereleases filtered, no tokens, no telemetry, never in background — spec-locked). New: HTTP 403/429 → rate-limit message, status 0 → offline message, other → generic; all non-destructive ("Nothing was changed on this device"). Board compatibility derives from explicit `board-NNN` markers in tag/title/notes/asset names: 601 declared → green pill; no marker → caution pill + verify note; other boards only (e.g. board-702) → red pill, downloads **not** rendered. A live check against the real repository returned the clean no-release state.

## 6. Real-Data Robustness Findings

- `HashSuffix`/`DiffSuffix`/`ByteSuffix` pipes rendered `NaN`/`Infinity` text for non-finite input (real-device glitch class) — now guarded to their safe zero forms; regression specs added.
- System page previously crashed-prone patterns (`cpuUsage.toFixed`, raw RSSI) replaced with guarded formatting via `deck-format` (em dash); spec asserts no NaN/undefined/null/Infinity ever renders.
- Update identity block: fallback rows while `info$` is pending; long version strings wrap (`overflow-wrap: anywhere`); release name falls back to tag; release notes parser handles missing body.
- Swarm totals already `|| 0`-guarded; Angular DatePipe self-guards NaN (scoreboard).
- DOM overflow audit at true 375 px viewport: `scrollWidth == innerWidth` on all 12 routes — no horizontal scrolling or clipped controls.

## 7. Test & Build Results

| Gate | Result |
|---|---|
| Frontend tests | **106/106 PASS, exit 0** (80 → 106; +26 specs: update states/compat, system sections/robustness, about, menu, pipe guards) |
| Reliability runs | **4/4 exit 0** (3 at `e081f98`, final at `388287d`) |
| Production build | OK — `version.txt` **v2.14.2-13-g388287da**, no `-dirty`; 16 pre-existing NG8102 template warnings (command-deck, byte-identical set to Phase 2D.1) — **no new warnings from Phase 2E code** |
| Firmware build | OK — exit 0, **0 compiler warnings in log**, `App "esp-miner" version: v2.14.2-13-g388287da`, `Project build complete` |
| app_desc gate | **APP DESC OK — v2.14.2-13-g388287da, matches sourceRevision** |
| Merged image | OK — 15,802,368 B (0xf12000) |
| QEMU firmware tests | **61 Tests, 0 Failures, 0 Ignored** (container `qemutest8`; serial log in artifacts) |
| Release export + manifest | **EXPORT OK: 4 artifacts, sourceRevision=v2.14.2-13-g388287da** + **VALIDATION OK (4 artifacts, board Gamma/601/BM1370)** — board 601 the only supported target |
| Secrets/local-path scan | **SECRETS SCAN: CLEAN, exit 0** — no host paths/usernames/credentials/private-dump names or PEM key material in any staged binary or manifest (the two bare `BEGIN … PRIVATE KEY` strings inside `esp-miner.bin` are mbedTLS pem.c parser constants with zero base64 payload, identical to the Phase 2D.1 baseline binary — analysis in `logs/secrets-scan.log`) |

**Binaries (full list incl. bootloader/partition-table/otadata in artifact `SHA256SUMS.txt`):**

| File | Size (B) | SHA-256 |
|---|---|---|
| `esp-miner.bin` | 1,645,472 | `30d7c3f8fe35cb124d47dd026b5353446b49f14f41542ce5d16549a8d5506aba` |
| `www.bin` | 3,145,728 | `d418b3ea84f188439699b84fb05fbf521f4092ed2aad6e7750b4ed7bf0110954` |
| `esp-miner-merged.bin` | 15,802,368 | `8a2789d8886301872a301602628663cd149fcf160ab1a67b2bc328f54021b8f5` |

`partition-table.bin` (`392125fd…732dc3`) and `ota_data_initial.bin` (`7d2c7ac4…82c62f`) remain **byte-identical to the v2.14.2 baseline** — direct evidence the partition table and OTA-selection mechanics are untouched.

## 8. Visual Review

12 desktop (1440×900) + 4 mobile screenshots + capture note in artifact `screenshots/`; findings in `VISUAL_REVIEW.txt`. Result: coherent NeuralAxe identity everywhere, no old primary branding, no overlap/clipping, no raw interpolation, **zero console errors** on a full route sweep (the pre-existing NG0100 on Settings/Network was found by this review and fixed), no lost controls, empty states verified (logs, swarm, live no-release check). Known capture artifact: headless-Edge mobile PNGs are missing ~28 px at the right edge (tooling, not layout — DOM audit proves zero overflow; documented in `screenshots/README-mobile-capture-note.txt`).

## 9. Remaining Issues

None blocking. Notes: (1) mobile PNGs carry the documented capture artifact; (2) the 16 NG8102 dev-template warnings in the command deck predate this phase (cosmetic, dev-only); (3) Phase 2C's owner-gated hardware validations (recovery-page reachability, factory-wipe confirmation, on-device OTA retention) remain open and unchanged; (4) the live release check currently returns "no release yet" — expected until a first NeuralAxe release is published.

## 10. Release-Candidate Recommendation

**Recommended: promote this build to the next controlled OTA pilot on the tuned Gamma 601** (owner-executed, Update page, hashes verified against `SHA256SUMS.txt`; previous known-good pair + v2.14.2 baseline retained as rollback paths A/B). The UI now satisfies the product-coherence bar for a `v0.1.0` release candidate; actual RC tagging should wait for the on-device pilot of this build plus the still-open owner-gated hardware validations. **This build is a development build and is not labeled or approved as a final public release.**

## 11. Hardware Confirmation

No physical flash was performed; no COM/USB access; no miner IPs contacted; the private flashdump was never accessed or referenced. Containers, QEMU, the dev server and one user-triggered read-only query to the public GitHub releases API (the Update page's own manual check, exercised once for state verification) were the only externally visible operations.
