# NeuralAxe OS — Versioning, Update-Channel and Release Policy

**Scope:** NeuralAxe OS 0.1.0-dev release line (development channel), Bitaxe Gamma / board 601 / BM1370 / ESP32-S3 N16R8 only.
**Foundation:** ESP-Miner / AxeOS v2.14.2 (GPL-3.0). NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects.

---

## 1. Versioning policy

Two version identities exist and must never be merged or falsified:

| Identity | Field(s) | Source of truth | Example |
|---|---|---|---|
| **NeuralAxe product version** | `productVersion` (+ `productName`, `buildChannel`, `vendor`) | `main/neuralaxe_identity.h` (firmware) = `src/app/neuralaxe.ts` (frontend); export tooling fails if they diverge | `0.1.0-dev` |
| **Upstream foundation version** | `upstreamProject`, `upstreamVersion` | same identity sources | `ESP-Miner / AxeOS v2.14.2` |
| **Source revision** | firmware `version` (esp_app_desc), frontend `version.txt`, manifest `sourceRevision` | `git describe --tags --always --dirty` at build time (upstream mechanism, deterministic for a committed tree) | `v2.14.2-3-geec4ead` |

Rules:
- Upstream fields `version`, `axeOSVersion`, `firmwareVersion` keep their upstream semantics; NeuralAxe identity is additive only (nine read-only fields + `vendor` in `/api/system/info`).
- A release build **must not** contain `-dirty` in its source revision; `tools/release/export_release.py` refuses to export one.
- No wall-clock build timestamps are embedded in the application. (The ESP-IDF second-stage bootloader embeds its own `__DATE__/__TIME__`; that is upstream baseline behavior and is documented, not extended.)

## 2. Update-channel policy

- NeuralAxe updates come from **`AegisAI-Dev/NeuralAxe-OS`** only (`NEURALAXE.updateRepository`, used by `GithubUpdateService`). Upstream ESP-Miner releases are **never** presented as NeuralAxe updates and there is **no fallback** to them.
- Release checks are **manual only** — triggered by the user from the Update page behind a privacy notice. Development builds perform **no background update checks**, no telemetry, no analytics; no credentials or tokens are used.
- If the NeuralAxe repository has no suitable (non-prerelease) release, the UI states that clearly. Offline/rate-limit failures produce a non-destructive "nothing was changed" message.
- Automatic download/installation of updates is not implemented and is out of scope.
- Manual local uploads of `www.bin` and `esp-miner.bin` on the Update page remain fully behavior-compatible with upstream (`/api/system/OTAWWW`, `/api/system/OTA` unchanged). Manually uploading an official upstream image is allowed and will replace NeuralAxe branding (stated on the Update page).

## 3. Board compatibility

This release line supports **exactly one** target: Gamma / board **601** / BM1370. Release manifests declare `supportedBoards: ["601"]`; the validator rejects any other board token (including 702) anywhere in a manifest. SupraHex board 702 / BM1368 are explicitly out of scope.

## 4. Artifact types

| Artifact | Type | Written via | Settings preserved | Destructive |
|---|---|---|---|---|
| `…-www.bin` | Frontend-only web update | Update page / `POST /api/system/OTAWWW` | **Yes** (writes only the `www` partition) | No |
| `…-ota.bin` (internally `esp-miner.bin`) | OTA application firmware | Update page / `POST /api/system/OTA` | **Yes** (writes inactive OTA slot + otadata) | No |
| `…-factory.bin` (merged) | Full-device recovery / first install | esptool/bitaxetool full flash @0x0 | **No** (NVS overwritten with 0xFF padding) | **Yes** |
| `config-601.cvs` | Board provisioning config (not a flashable binary) | `bitaxetool --config` (NVS partition generator) | Replaces NVS with provisioning defaults | Yes (NVS) |
| Private full flashdump | Owner-only recovery snapshot with private NVS data | esptool full restore | Restores the complete captured state | Overwrites everything |

**A private full flashdump is never a distributable release artifact.** The manifest validator rejects dump-like filenames outright.

## 5. Release artifact naming

Deterministic scheme (staging/export only — internal build outputs are never renamed):

```
NeuralAxe-OS-v<productVersion>-<Device>-<Board>-www.bin
NeuralAxe-OS-v<productVersion>-<Device>-<Board>-ota.bin
NeuralAxe-OS-v<productVersion>-<Device>-<Board>-factory.bin
NeuralAxe-OS-v<productVersion>-<Device>-<Board>-manifest.json
NeuralAxe-OS-v<productVersion>-<Device>-<Board>-SHA256SUMS.txt
config-601.cvs   (kept under its upstream name, clearly separate)
```

Produced by `tools/release/export_release.py`; validated by `tools/release/validate_manifest.py` (sizes, SHA-256, board identity, no unsupported boards, no private dumps, no `-dirty`).

## 6. Partition map and write behavior (traced from source at v2.14.2 + partitions.csv)

| Partition | Offset | Size | Purpose | www update writes | app OTA writes | factory flash writes | private data possible |
|---|---|---|---|---|---|---|---|
| (bootloader) | 0x0 | ~0x8000 region | 2nd-stage bootloader | no | no | **yes** | no |
| (partition table) | 0x8000 | 0x1000 | partition table | no | no | **yes** | no |
| `nvs` | 0x9000 | 0x6000 | all settings: Wi-Fi credentials, pools/stratum users+passwords, tuning (frequency/voltage), fan/thermal prefs, theme, board identity, best-diff | no | no | **yes — 0xFF padding (ERASES NVS)**, or provisioning values with `merge_bin.sh -c` | **YES** |
| `phy_init` | 0xf000 | 0x1000 | RF calibration | no | no | yes (0xFF) | no |
| `factory` | 0x10000 | 4 MiB | factory app slot | no | no | **yes** (app image) | no |
| `www` | 0x410000 | 3 MiB | AxeOS/NeuralAxe web UI | **yes — only this** | no | yes | no |
| `ota_0` | 0x710000 | 4 MiB | OTA app slot A | no | **yes (if inactive)** | yes (0xFF wipe) | no |
| `ota_1` | 0xb10000 | 4 MiB | OTA app slot B | no | **yes (if inactive)** | yes (0xFF wipe) | no |
| `otadata` | 0xf10000 | 8 KiB | active-slot selection | no | **yes** (`esp_ota_set_boot_partition`) | yes (reset to initial → factory boots) | no |
| `coredump` | after otadata | 64 KiB | crash dumps | no | no | no (merged image ends at 0xf12000) | unlikely (crash memory) |

Code anchors: `POST_WWW_update` erases/writes only the `www` partition (http_server.c); `POST_OTA_update` uses `esp_ota_get_next_update_partition(NULL)` → never the factory slot, then `esp_ota_end` + `esp_ota_set_boot_partition`; the merged image (merge_bin.sh) spans 0x0–0xf12000 with 0xFF gap-fill, which covers `nvs`/`phy_init` and both OTA slots.

**Settings preservation summary (source-traced):**
- `www.bin` update preserves firmware and all NVS settings. ✔ proven from source
- Application OTA preserves NVS (Wi-Fi, pools, tuning, theme). No migration or reset code exists in the v2.14.2 update path. ✔ proven from source; on-device confirmation: REQUIRES CONTROLLED HARDWARE VALIDATION
- Factory/merged flashing is destructive: NVS erased (or replaced by provisioning with `-c`), both OTA slots wiped, otadata reset → device boots the factory app and needs re-provisioning. ✔ proven from image layout
- Restoring a private full 16 MiB flashdump restores the complete captured device state including private NVS data. (Conceptual; the dump itself is never inspected.)

## 7. Recovery and rollback

Recovery mode: the application (factory or OTA) embeds `recovery_page.html`; `/recovery` always serves it, and if the `www` partition content is unusable the default route serves the recovery page, from which a new `www.bin` can be uploaded. ✔ traced (`rest_recovery_handler`)

Rollback hierarchy (ordered, least → most destructive):

| Path | Mechanism | Preserves | Destroys | Status |
|---|---|---|---|---|
| **A. NeuralAxe OTA re-upload** | Upload previous known-good `esp-miner.bin` (and matching `www.bin`) via Update page/recovery | NVS, Wi-Fi, pools, tuning | nothing (inactive slot + otadata rewritten) | source-traced; on-device swap: REQUIRES CONTROLLED HARDWARE VALIDATION |
| **B. Official ESP-Miner v2.14.2 board-601 recovery image** | Full factory flash of upstream `esp-miner-factory-601-v2.14.2.bin` (or our archived baseline merged image) via esptool/bitaxetool | nothing on-device (with `-c`/config: provisioning defaults) | NVS (settings/credentials), OTA slots, NeuralAxe branding | destructiveness proven from layout |
| **C. Private owner-only TCH flashdump restore** | esptool full 16 MiB restore of the owner's captured image | the exact captured state: v2.11.4-TCH firmware, tuning (625 MHz/1100 mV), Wi-Fi, pool credentials | everything currently on flash | owner-only; never distributed; never inspected by tooling |

## 8. Release procedure (development channel)

1. All changes committed; working tree clean (git describe contains no `-dirty`).
2. Frontend tests green (42+), firmware/QEMU tests green (61), production build + `idf.py build` + `merge_bin.sh` from the supported devcontainer/CI toolchain.
3. `tools/release/export_release.py` stages board-601 artifacts, manifest, SHA256SUMS; `tools/release/validate_manifest.py` must pass.
4. The manual GitHub workflow `neuralaxe-release.yml` (workflow_dispatch only) reproduces this pipeline in CI and attaches the export as a **workflow artifact only** — publishing a GitHub release remains a separate, owner-executed decision, out of scope in Phase 2C.
5. Private dumps, credentials and local paths are excluded by validation; GPL-3.0 and upstream attribution ship in the manifest.
