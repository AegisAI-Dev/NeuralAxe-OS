# NeuralAxe OS — Phase 2D.1 Real-Device Polish Report

**Phase:** 2D.1 — Real Device Polish, Clean Build Identity, Live Data Formatting, Shell Consistency
**Date (UTC):** 2026-07-17
**Author:** Frontend/embedded UI engineering (automated session)
**Pilot input:** PASS WITH UI FIXES — Command Deck live on the tuned Gamma 601 (625 MHz, ~1.14 V, ~1.29 TH/s, four real hash domains, NVS/Wi-Fi preserved after OTA)

---

## 1. Verdict

## **PASS**

Clean builds now contain no `-dirty` **in the firmware binary itself** (proven, previously false-dirty in every phase); dirty builds are still detected honestly at three layers; pool latency renders as 107 ms; the fan gauge no longer overlaps; every deck value is guarded against invalid data; System Status labels are factual; insights are deterministic with safe copy (spec-enforced); all secondary pages share the NeuralAxe shell; 80/80 frontend and 61/61 firmware tests pass; production, firmware and merged builds succeed; no protected firmware behavior changed; no hardware or private dump accessed.

## 2. Source State

Branch `neuralaxe-v0.1-dashboard`; built at **`c630e1a503621810a27e63ade21c5e733bac5d23`** ("chore: untrack generated karma report", parent `43d605e` "fix: polish live dashboard and enforce honest build identity"). **Clean version string `v2.14.2-9-gc630e1a`** — verified in `version.txt` **and** in the `esp_app_desc_t` of `esp-miner.bin` (`--check-bin`: APP DESC OK).

## 3. Dirty-Version Root Cause & Exact Fix

**Observed on hardware:** `v2.14.2-7-g9456916-dirty` from a clean commit.

**Root cause (proven in-container):** ESP-IDF computes `PROJECT_VER` via `git describe --dirty` *inside the Linux build container*. The Windows CRLF checkout appears there as "every tracked file modified" (container git lacks `autocrlf`), yielding a false `-dirty`. Retroactive audit: **every** container-built binary since Phase 1 carried a false `-dirty` app-descriptor (`v2.14.2-dirty` → `-7-g9456916-dirty`); prior clean-build attestations had only checked the host-generated `version.txt` — that verification gap is closed. A second, real taint was found during this phase: a generated `report.xml` had accidentally become **tracked**, dirtying even host builds after any test run.

**Fixes:**
1. Container builds set `git config --global core.autocrlf true` + `core.filemode false` (normalizes line-ending/mode comparison only — genuine modifications still produce an honest `-dirty`). Documented as mandatory in the versioning policy; Linux CI is unaffected.
2. `report.xml` deleted from tracking and added to `axe-os/.gitignore`.
3. **Gate:** `tools/release/export_release.py` now parses the `esp_app_desc_t` version (offset 0x30) of `esp-miner.bin` and fails on `-dirty`, on `version.txt` mismatch, or on a non-IDF image; `--check-bin` runs standalone. Battery: synthetic clean/dirty/non-IDF 3/3; all previous phases' real binaries correctly rejected. Nothing strips `-dirty` — detection is honest in both directions.

## 4. Live-Data Formatting (findings 2–4)

New `deck-format.ts` (display-only; data never altered): invalid input (null/undefined/NaN/±Infinity/non-number) always renders an em dash; latency = integer ms (observed `106.8960037` → **107 ms**), one decimal below 1 ms, neutral fallback for negatives; controlled precision for power/efficiency/percentages/temps; grouped integers for shares/RPM/height; volts null-safe; heap via ByteSuffix; difficulties via DiffSuffix; pool URL ellipsis + tooltip; worker stays privacy-truncated without a full-value tooltip. Full table: artifact `FORMATTING_RULES.txt`. Battery: `deck-format.spec.ts` + rendered specs with live-device values.

## 5. Fan Gauge Fix (finding 3)

Stacked read (`RPM` value / "RPM" / cyan "Fan N%"), capped value font with ellipsis, explicit line heights and gaps. A rendered spec mounts the deck with `fanrpm: 15,230` and asserts by bounding box that the percentage line starts below the RPM value. Verified visually at 375 px and 1440 px.

## 6. System Status & Insights (findings 5–6)

Labels now factual: **"Free Memory (Heap)"** with byte units, **"System Load (CPU)"**, firmware row = productVersion + git-describe string with **no** unproven "Up to date" claim. Insights unchanged in mechanics, boundary-tested at 26/34 J/TH, 150 ms, 15% spread; "below par" copy no longer mentions tuning; a spec fails if any insight copy ever contains "tuning"/"AI"/"automatic"; warning vs informational severities distinct; display-only, никогда hardware actions.

## 7. Shell Consistency

Global `nx-` normalization applied to the shared shell (no page internals rewritten): all secondary pages (Classic, Scoreboard, Swarm, Logs, System, Pools, Network, Theme, Settings, Update, Whitepaper link) now render on NeuralAxe panel surfaces with the same border/accent system, small-caps cyan page-title treatment, same topbar/sidebar/spacing/responsive behavior; nested cards darken consistently. Verified on Settings/System/Update/Pools desktop + Settings mobile screenshots; all controls preserved; menu-route spec locks the shell routes.

## 8. Files Changed

Commits `43d605e` + `c630e1a`: 7 modified, 3 created, 1 removed-from-tracking — full list in artifact `CHANGED_FILES.txt` (component TS/HTML/SCSS + specs, `deck-format.ts(+spec)`, menu spec, `export_release.py`, policy doc, `.gitignore`, deleted `report.xml`). This report is the only post-build addition. **Zero firmware C changes; no protected behavior touched** (partition-table and otadata remain hash-identical to the v2.14.2 baseline).

## 9. Test & Build Results

| Gate | Result |
|---|---|
| Frontend tests | **80/80 PASS, exit 0** (60 → 80) |
| Reliability runs | 3/3 exit 0 |
| Production build | OK — `v2.14.2-9-gc630e1a`, no `-dirty` |
| Firmware build | OK — 0 errors; 235 pre-existing header warnings, none from NeuralAxe files |
| app_desc gate | **APP DESC OK — clean, matches sourceRevision** |
| Merged image | OK — 0xf12000 bytes |
| QEMU firmware tests | **61/61 PASS** (serial log preserved; container console log unavailable because Docker Desktop shut down post-run — noted in `logs/firmware-test-full.log`) |
| Release export + manifest | EXPORT OK + VALIDATION OK (board 601 enforced) |
| Secrets/path scan | clean |

**Binaries:** `esp-miner.bin` 1,645,472 B `04657f66…02add`; `www.bin` 3,145,728 B `1dbab37b…98ae7`; merged 15,802,368 B `cead1d79…19d7b`; 61% app-partition headroom. Screenshots (7, desktop+mobile incl. Block Signal state): artifact `screenshots/`.

## 10. Remaining Issues

None blocking. Notes: the stopped `qemutest7` container awaits cleanup at next Docker start; hero trend renders flat against static dev mocks (expected); Phase 2C's deferred hardware validations (recovery-page reachability, factory-wipe confirmation) remain owner-gated and unchanged by this phase.

## 11. Hardware Confirmation

No COM/USB access, no flashing, no esptool/bitaxetool against hardware, no live miner IPs contacted, private flashdump never accessed or referenced beyond documentation. QEMU and containers only.

## 12. Recommendation — Next Pilot Update

**Recommended: controlled OTA update of the tuned Gamma 601** with this build's `www.bin` + `esp-miner.bin` (or the release-export `…-ota.bin`), owner-executed via the Update page:
1. Verify hashes against `SHA256SUMS.txt` before upload.
2. Upload `esp-miner.bin` first (OTA slot swap), confirm boot and that **System → Firmware Version now reads `v2.14.2-9-gc630e1a` without `-dirty`** — this is the on-device confirmation of the identity fix and simultaneously the Phase 2C "OTA retention" hardware validation (check Wi-Fi/pool/tuning survive).
3. Upload `www.bin`, confirm the polished deck (107 ms latency formatting, stacked fan gauge, unified shell).
4. Keep the previous known-good pair and the v2.14.2 baseline image as rollback (paths A/B); the private TCH dump remains last-resort C.
