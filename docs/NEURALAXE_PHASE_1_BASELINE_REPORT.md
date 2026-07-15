# NeuralAxe OS — Phase 1 Baseline Report (Corrected, true v2.14.2)

**Phase:** Phase 1 — Reproducible Upstream Baseline Build (baseline-corrected rebuild)
**Date (UTC):** 2026-07-15
**Author:** Firmware build & baseline engineering (automated session)

---

## 1. Phase Verdict

## **PASS**

All Phase 1 success criteria are met on the correct baseline: the previous mislabeled build is archived under an honest name with its binaries marked NOT APPROVED FOR FLASHING; the current branch resolves exactly to the official v2.14.2 commit; source and dependencies are complete; frontend tests pass 33/33; firmware tests pass 61/61 in QEMU; AxeOS, the firmware, and the merged image all build successfully; every artifact has a recorded size and SHA-256; the only repository change is this report; and no physical hardware or private flashdump was touched.

---

## 2. Baseline Identity

| Item | Value |
|---|---|
| Actual repository path | `D:\Companys\Neuralshield\Software\NeuralAxe-OS` |
| Repository | `AegisAI-Dev/NeuralAxe-OS` |
| Branch | `neuralaxe-v0.1-board601-v2142` |
| Resolved commit | **`64680f8a4da0b9a3b532051f0aa18429fcf04e82`** |
| Official baseline declaration | ESP-Miner **v2.14.2** (upstream tag `v2.14.2` = `64680f8a`, verified via GitHub API) |
| Hardware target | Bitaxe **Gamma**, board **601**, ASIC **BM1370** (ESP32-S3-WROOM-1 N16R8) |

Branch and commit were verified by reading `.git\HEAD` and `.git\refs\heads\neuralaxe-v0.1-board601-v2142` only — no git command was executed. The branch itself was created by the owner in GitHub Desktop. Corroborating evidence that the tree is v2.14.2: the repository's own `generate-version.js` produced `version.txt` = **v2.14.2** during the frontend build, and the firmware test count (61) differs from the later master snapshot (72).

**This report explicitly distinguishes two builds:**

| | Historical build | Corrected build (this report) |
|---|---|---|
| Commit | `9f18b7db1d24594bf8231384798c3cb0dba57303` | `64680f8a4da0b9a3b532051f0aa18429fcf04e82` |
| Identity | upstream master snapshot, post-v2.14.2 (24 ahead / 6 behind the tag) | official v2.14.2 release baseline |
| Status | archived; **NOT APPROVED FOR PHYSICAL FLASHING** | current Phase 1 baseline |
| Location | `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\9f18b7d-post-v2.14.2-board601` | `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\v2.14.2-board601` |

---

## 3. Toolchain Versions (declared by this exact tree, then used)

Declared at `64680f8a` (all four declaration sources re-read on this checkout, not carried over):

- CI (`.github/workflows/build.yml`, `unittest.yml`): ESP-IDF **v5.5.3** (esp-idf-ci-action), target esp32s3, Node **24.14.0**, pip `esptool`, tests via `bitaxeorg/esp32-qemu-test-action`
- `.devcontainer/Dockerfile`: base **`espressif/idf:v5.5.2`** + Node 22.x (byte-identical to the file at 9f18b7d, so the existing `espminer-build` image is exactly this baseline's declared environment)
- `AGENTS.md` / `sdkconfig.defaults` header: ESP-IDF v5.5.1 (informational)
- `axe-os/package.json`: engines Node **>=22**, Angular **18.2.13** (CLI 18.2.19), PrimeNG 17.18.15, Karma 6.4.4, TypeScript 5.5.4

Used for this build:

| Tool | Version |
|---|---|
| ESP-IDF | v5.5.2 (devcontainer image `espminer-build`; repository-supported isolated workflow per README) |
| Compiler | xtensa-esp-elf-gcc 14.2.0 (crosstool-NG esp-14.2.0_20251107) |
| esptool | 4.11.dev1 (bundled in IDF; image generation + merge) |
| CMake / Ninja | 3.30.2 / 1.11.1 |
| Python (container) | 3.12.3 |
| QEMU | 9.2.2 (esp_develop_9.2.2_20250817) via `idf_tools.py install qemu-xtensa`, `-machine esp32s3` |
| Node / npm (host) | v22.16.0 / 10.9.2 (satisfies engines >=22; CI pins 24.14.0 — noted deviation) |
| Angular | 18.2.13 (from this tree's lockfile via `npm ci`, 999 packages) |
| Karma browser | **Microsoft Edge 150 headless via `CHROME_BIN`** — Chrome is not installed on this host. The baseline's `ChromeHeadlessCI` launcher (karma.conf.js) is `base: ChromeHeadless` + `--no-sandbox`, which karma-chrome-launcher resolves through `CHROME_BIN`; Edge is Chromium and is compatible. This substitution affects the browser binary only. |
| Docker | Engine 28.1.1 (linux), Docker Desktop 4.41.2, Windows 11 |

Known version-pin spread (upstream as-is, unchanged by us): CI v5.5.3 vs devcontainer v5.5.2 vs docs v5.5.1. The devcontainer was used because it is the repository's supported local isolated workflow.

---

## 4. Dependency and Submodule State

- Submodule `components/libsecp256k1/libsecp256k1` checked out at **`0cdc758a56360bf58a851fe91085a327ec97685a`** (read from `.git\modules\...\HEAD`), which **matches the pin recorded in the upstream v2.14.2 tree** (verified via GitHub API). Content present (e.g. `src/secp256k1.c`).
- All source directories present: `components/{asic, connect, dns_server, libsecp256k1, stratum, stratum_v2}`, `main/` (+ `http_server/axe-os`), `test/`, `test-ci/`, `tools/`.
- npm: `npm ci` → 999 packages, clean install from this tree's `package-lock.json` (deprecation warnings only: `inflight`, `rimraf@3`, `glob@7`, `critters`).
- IDF managed components resolved by the component manager during the build (`managed_components/`, git-ignored).
- Windows checkout artifacts handled without modifying the repo: `test-ci/CMakeLists.txt` (git symlink materialized as a text file) and CRLF line endings in `merge_bin.sh` — see §6.

---

## 5. Commands Executed

Stage 1 (archival, outside the repo except the report removal):
```
Rename-Item "...\NeuralAxe Build Artifacts\v2.14.2-board601" "9f18b7d-post-v2.14.2-board601"
Copy-Item docs\NEURALAXE_PHASE_1_BASELINE_REPORT.md "...\9f18b7d-post-v2.14.2-board601\NEURALAXE_PHASE_1_BASELINE_REPORT_9f18b7d.md"
# + created BASELINE_CORRECTION_NOTICE.md in the archive
Remove-Item docs\NEURALAXE_PHASE_1_BASELINE_REPORT.md   # old report, after verified preservation
```

Stage 5 (clean + corrected build):
```
# 1. Removed generated state only: build/, managed_components/, sdkconfig, dependencies.lock,
#    axe-os/{node_modules, dist, .angular, src/app/generated}
cd main/http_server/axe-os
npm ci
$env:CHROME_BIN = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
npm run test:ci          # 33/33 PASS; report.xml preserved to artifacts, then deleted from tree
npm run build            # production + gzip + version.txt (v2.14.2)

docker run -d --name fwbuild2 -v <repo>:/workspace -e GITHUB_ACTIONS=true espminer-build \
  /bin/bash -c "git config --global --add safe.directory '*' ; cd /workspace && idf.py build"

docker run --rm -v <repo>:/workspace -v <artifacts>:/artifacts espminer-build \
  /bin/bash -c "cd /workspace && tr -d '\r' < merge_bin.sh > /tmp/merge_bin.sh && bash /tmp/merge_bin.sh /artifacts/esp-miner-merged.bin"

# Firmware tests (mirrors this baseline's own unittest.yml → bitaxeorg/esp32-qemu-test-action):
#   container-side copy of the tree (repo untouched, test-ci symlink restored)
cd /tmp/src/test-ci && idf.py build
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa
esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o flash_image.bin @flash_args
timeout 5m qemu-system-xtensa -machine esp32s3 -nographic -no-reboot \
  -drive file=flash_image.bin,if=mtd,format=raw -serial file:<log>
```

Stage 6 (validation): `Get-FileHash -Algorithm SHA256` over every artifact; wrote `SHA256SUMS.txt`, `BUILD_INFO.txt`, `ARTIFACT_MANIFEST.txt`.

Notes: `git config --global --add safe.directory` ran **inside the ephemeral container only** (required by the README devcontainer procedure; does not touch the repository). The repository's own build scripts internally invoke git for version stamping. No git command was run against the repository by this session.

---

## 6. Test Results

### 6.1 Frontend (`npm run test:ci`) — **PASS, 33/33**
Karma 6.4.4 / Jasmine on Edge 150 headless (documented `CHROME_BIN` substitution, §3). JUnit `report.xml` was preserved to `logs/frontend-test-report.xml` in the artifact directory and **removed from the working tree** so it never appears as a repository change. Full console log: `logs/frontend-test-full.log`.

### 6.2 Firmware (`test-ci` in QEMU) — **PASS, 61 Tests, 0 Failures, 0 Ignored**
Reproduces this baseline's own CI (`unittest.yml` → `esp32-qemu-test-action`): same flash-merge command, same `-machine esp32s3` boot, same Unity summary criterion; QEMU exit code 0. Components under test: `stratum` and `asic` (the `TEST_COMPONENTS` set of `test/CMakeLists.txt`). The v2.14.2 tree contains the same QEMU test project as the later snapshot, so no tooling was imported from another commit; the only local differences vs CI: QEMU 9.2.2_20250817 from IDF tools (action uses a newer 9.2.2 build on IDF v5.5.4), and the JUnit conversion (Ruby Unity parser) was skipped — the raw serial log (`logs/qemu-serial-output.log`) and full run log (`logs/firmware-test-full.log`) are preserved instead. Note the 61-test count vs 72 on `9f18b7d` — consistent with the older baseline.

Benign expected output: "Result failed: Job not found/Stale/Above target" lines are negative-path test inputs; `W rtcinit: o_code calibration fail` is a known QEMU emulation warning.

---

## 7. Build Results

### 7.1 AxeOS frontend — **SUCCESS**
Angular 18.2 production build: initial total 2.20 MB raw / ~404 kB transfer; gzipped (`gzipper`), non-gzip originals pruned (`only-gzip.js`); `version.txt` = **v2.14.2**. Full log: `logs/frontend-build-full.log`.

### 7.2 Firmware (`idf.py build`, ESP-IDF v5.5.2, esp32s3) — **SUCCESS**
`esp-miner.bin` = 0x191930 bytes; smallest app partition 0x400000 → **61% free**. `www.bin` packaged from the prebuilt `dist/axe-os` via the `GITHUB_ACTIONS=true` path — exactly what upstream CI does. 0 errors. Full log: `logs/fwbuild-full.log`.

### 7.3 Merged factory image (`merge_bin.sh`) — **SUCCESS**
esptool 4.11.dev1 `merge_bin` (esp32s3, dio, 16MB, 80m): bootloader @0x0, partition table @0x8000, app @0x10000, www @0x410000, otadata @0xf10000 → **0xf12000 bytes**. Executed from a CRLF-stripped temp copy inside the container (`tr -d '\r'`) because the Windows checkout's CRLF endings break the shebang; the repo file was not modified. Output written directly to the artifact directory (never into the repo, where it would be untracked). Log: `logs/merge-full.log`.

---

## 8. Warnings (grouped by origin and severity)

| Origin | Count | Severity | Detail |
|---|---:|---|---|
| libsecp256k1 headers (submodule) | ~226 | benign | `-Wunused-function`: static declarations never defined in `field.h`/`group.h` etc. — upstream-known header pattern |
| ESP-IDF FreeRTOS `atomic.h` | ~9 | benign | `-Wunused-function`: unused `Atomic_*` helpers |
| ESP-Miner application code | **0** | — | no compiler warnings in `main/` or non-submodule components |
| Angular build | 1 | low | initial bundle 2.20 MB exceeded 2.10 MB budget by 106 kB (upstream budget setting) |
| npm ci | 4 | informational | deprecated transitive deps: `inflight`, `rimraf@3`, `glob@7`, `critters` |
| QEMU runtime | 1 | benign | `rtcinit: o_code calibration fail` (emulation artifact) |

Firmware compile warning total: 235 lines, all `-Wunused-function`. Errors: **0** everywhere.

---

## 9. Artifacts (fresh directory, no reuse from the historical build)

Location: `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\v2.14.2-board601\`

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `esp-miner-merged.bin` | 15,802,368 | `a9032fa0fb1e755392c4d10c3c46cd8e96cca3743846e56a16ecc24027e2b18b` |
| `esp-miner.bin` | 1,644,848 | `b9d79e862b30f9a56e3580ae114871489e991102c79048ba2f2ffaa0fb3a5a50` |
| `www.bin` | 3,145,728 | `c70f20eb70471e0244b34579cebc17741443dd7e1683f5c28b9615c101c75910` |
| `bootloader.bin` | 22,432 | `9334fa49c466422176ae44e56ddbf22ed50535ed03f12514614fa15eef91fd01` |
| `partition-table.bin` | 3,072 | `392125fd8d91d3fbf8f4b5dde20032f047492e2415c339b4bf222eaf14732dc3` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `config-601.cvs` | 975 | `3c0f28f6112cf9e12ade941e3f82aa750fa3554a7e84b73340d665a571a55f22` |

Plus `SHA256SUMS.txt`, `BUILD_INFO.txt`, `ARTIFACT_MANIFEST.txt`, and `logs/` (frontend test log + JUnit XML, frontend build log, firmware build log, merge log, firmware test log, QEMU serial output). All binaries verified present and non-zero. `partition-table.bin` and `ota_data_initial.bin` are byte-identical to the historical build (same partition layout / standard OTA seed); every other binary differs, as expected for the different source tree.

**Historical archive:** `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\9f18b7d-post-v2.14.2-board601\` — contains the complete 9f18b7d build, its original report (`NEURALAXE_PHASE_1_BASELINE_REPORT_9f18b7d.md`) and `BASELINE_CORRECTION_NOTICE.md` marking those binaries **NOT APPROVED FOR PHYSICAL FLASHING**.

---

## 10. Exact Files Created / Removed / Modified During This Correction

Inside the repository:
- **Created:** `docs/NEURALAXE_PHASE_1_BASELINE_REPORT.md` (this file) — the only repository change.
- **Removed:** `docs/NEURALAXE_PHASE_1_BASELINE_REPORT.md` (the previous, 9f18b7d-based report) — removed only after verified preservation in the historical archive. The transient generated `main/http_server/axe-os/report.xml` from the test run was preserved to the artifact directory and deleted from the tree (the copy the owner discarded earlier in GitHub Desktop was an identical earlier instance).
- **Modified:** none. No tracked source file was edited. `.gitignore` was not modified.
- Generated, git-ignored build state was removed and regenerated as part of the clean build (`build/`, `sdkconfig`, `dependencies.lock`, `managed_components/`, axe-os `node_modules/`, `dist/`, `.angular/`, `src/app/generated/`). None of these are repository changes.

Outside the repository: historical archive rename + 2 files added to it (report copy, correction notice); fresh artifact directory with binaries, manifests and logs; session scratchpad files.

---

## 11. Compliance Confirmations

- **No physical device was accessed or modified.** No COM port, no USB serial device, no flashing, no `erase_flash`/`write_flash`/`flash`/`monitor`, no esptool or bitaxetool against hardware (esptool operated on files inside Docker only), no live miner contacted over IP. The pilot (v2.11.4-TCH) and reference (v2.14.1) Gammas were untouched.
- **The private flashdump directory (`C:\Users\Blind\Desktop\NeuralAxe-TCH-Flashdump`) was never accessed.**
- **No firmware behavior was changed:** mining logic, ASIC drivers/initialization, frequency/voltage, power management, fan control, thermal protection, self-test, Stratum V1/V2, NVS, OTA, Wi-Fi, pool configuration and partition layout are byte-for-byte as checked out at `64680f8a`. No NeuralAxe branding was introduced. No secrets, Wi-Fi data, pool credentials or wallet addresses were added.
- **No git command was run against the repository**; branch creation was performed by the owner in GitHub Desktop; verification used read-only `.git` file reads and the GitHub API.

---

## 12. Recommendation

**Phase 2 (NeuralAxe branding) may begin** from branch `neuralaxe-v0.1-board601-v2142` at commit `64680f8a` (verdict: PASS). The baseline is now correctly identified, reproducible in the repository's own devcontainer environment, fully tested, and archived with honest provenance on both sides. Recommended Phase 2 hygiene (no action taken now): branch from this verified baseline, keep the historical `9f18b7d` archive quarantined from flashing workflows, and re-run this Phase 1 procedure as a regression gate whenever the baseline moves.
