# NeuralAxe OS — Phase 2C Firmware Foundation Report

**Phase:** 2C — Firmware Foundation, Versioning, Update Contract, Recovery Contract and Release Readiness
**Date (UTC):** 2026-07-16
**Author:** Firmware/release engineering (automated session)

---

## 1. Verdict

## **CONDITIONAL PASS**

Every gate that can be proven without hardware **passes**: branch verified; product and upstream versions remain strictly separate; all identity values agree across firmware, OpenAPI, mocks, frontend and tests (enforced by tooling that fails on divergence); the clean build carries no `-dirty`; update checks target only `AegisAI-Dev/NeuralAxe-OS` with no upstream fallback and no background checks; board 601 is enforced in release metadata; artifact types/naming are unambiguous; the release manifest validates sizes and hashes (8/8 negative tests); NVS/OTA/factory behavior is traced from source; rollback paths are documented; private dumps are excluded by validation; Karma exit behavior is now reliable (5/5 green = 0, deliberate failure = 1); all tests pass (frontend 52/52, firmware 61/61); frontend, firmware, merged image and release-export dry run all build/pass; no protected behavior changed; no hardware touched.

**Conditional** solely because three properties are physically unprovable off-device and are formally deferred with exact test definitions (§12): on-device OTA settings retention, recovery-page reachability with a corrupted `www` partition, and factory-flash NVS wipe confirmation.

## 2. Context

| Item | Value |
|---|---|
| Branch | `neuralaxe-v0.1-firmware-foundation` |
| Resolved commit (built) | `e14c0b030eb5f20035d68353a675d57e68dc79c9` — "feat: add NeuralAxe update channel and release foundation" |
| Branch base | `491e3e1` (= branding branch tip incl. Phase 2A validation) |
| Version string | **`v2.14.2-5-ge14c0b0`** (no `-dirty`) |
| Target | Bitaxe Gamma / board 601 / BM1370 / ESP32-S3 N16R8 (only) |

## 3. Change Map / Exact Files Changed

**12 files in commit `e14c0b0` (8 modified + 4 created) + this report (created post-build):**

| File | Purpose | Behavior impact | Risk |
|---|---|---|---|
| `neuralaxe.ts` | + `updateRepository` | none (data) | low |
| `neuralaxe.spec.ts` | pin update repo | test-only | none |
| `github-update.service.ts` | release check → NeuralAxe repo | HTTP URL only | low |
| `github-update.service.spec.ts` | repo target / no-fallback / prerelease / error specs | test-only | none |
| `update.component.ts` | `ReleaseCheckResult` states + `catchError` | UI states; uploads untouched | low |
| `update.component.html` | no-release + offline messages; privacy copy | copy/rendering | low |
| `update.component.spec.ts` | no-background-check, upload-controls, state specs | test-only | none |
| `karma.conf.js` | `transports: ['polling']` | test infra only | low |
| `tools/release/export_release.py` (new) | deterministic export + manifest + SHA256SUMS; rejects `-dirty`, cross-checks firmware↔frontend identity | release tooling | none |
| `tools/release/validate_manifest.py` (new) | board/hash/size/dump/dirty validation | release tooling | none |
| `.github/workflows/neuralaxe-release.yml` (new) | manual-only release build, artifacts-only | CI, `workflow_dispatch` only | none |
| `docs/NEURALAXE_VERSIONING_AND_RELEASE_POLICY.md` (new) | policy documentation | docs | none |

**Zero firmware C changes.** OTA/upload endpoints, NVS, partition table, merge scripts, upstream workflows and all protected mining/hardware code untouched.

## 4. Version / Build Contract

Authoritative identity: `main/neuralaxe_identity.h` (firmware) ≡ `src/app/neuralaxe.ts` (frontend) — nine values (productName **NeuralAxe OS**, productVersion **0.1.0-dev**, buildChannel **development**, vendor **NeuralShield**, upstreamProject **ESP-Miner / AxeOS**, upstreamVersion **v2.14.2**, targetDevice **Gamma**, targetBoard **601**, targetAsic **BM1370**) plus `updateRepository`. `export_release.py` parses the header, cross-checks the frontend const and **fails on any divergence**; specs assert the same values, and `system.service.spec.ts` ties the API mock to them. `sourceRevision` = `git describe` (deterministic for a committed tree; upstream mechanism) — surfaced as firmware `version`, frontend `version.txt` and manifest `sourceRevision`; the export tool **rejects any `-dirty`** revision. Upstream fields `version`/`axeOSVersion` keep upstream semantics; no timestamps added (bootloader's upstream `__DATE__` noted as pre-existing). Full policy: `docs/NEURALAXE_VERSIONING_AND_RELEASE_POLICY.md`.

## 5. Update-Channel Contract (outcome)

- Manual, user-consented release checks only — verified by spec that component construction issues **no** HTTP request.
- Target pinned to `AegisAI-Dev/NeuralAxe-OS` (`GithubUpdateService.RELEASES_URL` built from the const); specs assert the URL and prove **no upstream request occurs** (`HttpTestingController.verify`).
- No suitable release → "No NeuralAxe OS release is available yet…" (no upstream fallback). Offline/rate-limit → "…Nothing was changed on this device." Both rendered states are spec-covered.
- Manual local `www.bin`/`esp-miner.bin` uploads unchanged (endpoints untouched; controls presence spec-covered). Upstream images remain manually uploadable and are labeled as replacing NeuralAxe branding.
- No automatic download/installation, no telemetry, no tokens.

## 6. API Compatibility

No API surface changed in 2C. The nine additive optional SystemInfo fields from 2A are unchanged; `/api/system/OTA`, `/api/system/OTAWWW`, `/recovery` and all other routes byte-identical. The frontend update page consumes the same endpoints.

## 7. Board-601 Compatibility Contract

`supportedBoards: ["601"]` in the manifest; validator rejects missing board identity, filename/board disagreement, and **any** unsupported board token (702 included) anywhere in the manifest; release filenames carry `Gamma-601`; identity constants pin the target. Full matrix in artifact `COMPATIBILITY_MATRIX.txt`.

## 8. Artifact Naming & Release-Manifest Schema

Names (staging/export only; internal outputs never renamed): `NeuralAxe-OS-v0.1.0-dev-Gamma-601-{www.bin, ota.bin, factory.bin, manifest.json, SHA256SUMS.txt}` + `config-601.cvs` kept separate. Manifest schema v1 fields: schemaVersion, productName, productVersion, buildChannel, vendor, sourceRevision, upstreamProject, upstreamVersion, targetDevice/Board/Asic, supportedBoards, license, attribution, privateArtifactsExcluded, artifacts[] {filename, artifactType, sizeBytes, sha256, flashMethod, settingsPreserved, destructive, minimumCompatible}. Validation battery: **8/8** (valid accepted; missing board, 702 mention, sha mismatch, size mismatch, dump listed, `-dirty`, dump-file-in-dir all rejected).

## 9. Partition Map & NVS/Settings Preservation (traced)

See artifact `PARTITION_MAP.txt` / `NVS_OTA_PRESERVATION.txt` and policy doc §6. Summary: `www.bin` writes only the `www` partition (settings preserved — proven); app OTA writes inactive slot + otadata only, no migration/reset code exists (settings preserved — proven from source); factory/merged image spans 0x0–0xf12000 with 0xFF fill → **NVS erased, OTA slots wiped, otadata reset — destructive** (proven from layout); private 16 MiB dump restore returns the complete captured state (conceptual). Partition table byte-identical to baseline.

## 10. Rollback Matrix

A. **NeuralAxe OTA re-upload** — preserves all NVS; destroys nothing (source-traced; on-device swap deferred).
B. **Official v2.14.2 board-601 recovery image** — full factory flash; destroys settings/credentials/branding; provisioning defaults with `-c`.
C. **Private owner-only TCH flashdump** — restores the exact captured state incl. private data; owner-only, never distributable, never inspected.
Full detail: artifact `ROLLBACK_MATRIX.txt`.

## 11. Karma / CI Reliability (root cause & outcome)

**Root cause:** after single-run completion on Windows, Edge/Chromium tears down the karma socket.io **websocket** with a TCP RST; karma-server records `UncaughtException: read ECONNRESET` *after* emitting final results and exits non-zero despite a green run (observed on 3 of 4 green runs).
**Fix:** `transports: ['polling']` — HTTP long-polling closes gracefully, eliminating the RST path while leaving result-driven exit codes untouched. No JUnit-only authority, no forced exits, no sleeps, no coverage change; compatible with local Edge and CI Chromium.
**Proof:** before — intermittent exit 1 on green; after — **5/5 repeated green runs exit 0**, official run 52/52 exit 0, deliberate-failure run (1 FAILED) exits **1**. Log: artifact `logs/karma-reliability.log`.

## 12. Release Workflow Design

`.github/workflows/neuralaxe-release.yml`: **`workflow_dispatch` only** (validation form; no push/tag/release triggers, nothing published — outputs land as workflow artifacts). Steps: repo guard (`AegisAI-Dev/NeuralAxe-OS` only) → identity/version agreement check (header vs frontend vs dispatch input; board must be 601) → dirty-tree and `-dirty` describe rejection → frontend tests → QEMU firmware tests (fails on any failure) → web + IDF v5.5.3 build → merge → `export_release.py` → `validate_manifest.py` → artifact upload. GPL/attribution ship in the manifest; private dumps impossible by validation. Runner execution was not performed (would require pushing); YAML mirrors the locally proven pipeline commands 1:1 — first dispatch run is an owner action.

## 13. Test & Build Totals

| Gate | Result |
|---|---|
| Frontend tests | **52/52 PASS, exit 0** (42 → 52; +10 update-channel/identity specs) |
| Karma reliability | 5×green=0, failure=1 |
| Firmware/QEMU | **61/61 PASS** (unchanged, as required) |
| Frontend build | OK — `v2.14.2-5-ge14c0b0`, no `-dirty`; pre-existing budget warning only |
| Firmware build | OK — 0 errors; 235 warnings, all pre-existing header noise, **none from NeuralAxe code** (no firmware source changed) |
| Merged image | OK — 0xf12000 bytes |
| Release export dry run | **EXPORT OK** (4 artifacts) |
| Manifest validation | **VALIDATION OK** + 8/8 negative battery |
| Secrets/path scan | clean (no local paths, usernames, credentials, wallet/Wi-Fi/pool data) |

**Binaries:** `esp-miner.bin` 1,645,472 B `47c7581945cb012e66772932e02c78aaff0ecbce6efb3db26f5ba8e692c030fc`; `www.bin` 3,145,728 B `7f90e5aaee36c0a3fc2aa67d0792ef236c5165159c23062060d47cbc0ab73d06`; merged 15,802,368 B `935cd5ce9c14d5cefb6791eca34214c42ca517bb634ea9ae077ae0f2b44fba58`; partition-table/otadata/config byte-identical to the v2.14.2 baseline; app headroom 61%.

## 14. Unresolved Issues / Hardware-Validation Requirements

No unresolved software issues. Deferred to a future controlled hardware session (exact tests, not performed):
1. **OTA retention:** on the pilot Gamma, record NVS values → upload `…-ota.bin` → verify slot swap boots and all settings survive (REQUIRES CONTROLLED HARDWARE VALIDATION).
2. **Recovery reachability:** corrupt/erase `www` partition → confirm `/` serves the recovery page and a `www.bin` upload restores UI without settings loss.
3. **Factory wipe confirmation:** flash `…-factory.bin` on a sacrificial/pilot device → confirm NVS reset behavior matches the traced layout.
4. First `neuralaxe-release.yml` dispatch run on GitHub runners (owner-triggered).

## 15. Compliance Confirmations

No COM/USB access, no flashing, no esptool/bitaxetool against hardware, no live miner contacted; the private flashdump was referenced conceptually only — never accessed, hashed or copied. No protected firmware behavior changed (zero firmware source modifications; hash-identical partition table/otadata/config). No git commands run; branch/commit verified via `.git` file reads.

## 16. Recommendation

**The dashboard redesign may begin.** The foundation contracts (identity, update channel, artifacts, preservation, rollback, CI reliability) are in place and enforced by failing tests/tooling. Recommended order: owner reviews/commits this report, optionally dispatches the release workflow once for CI validation, then Phase 2D (dashboard) can proceed on a new branch from this foundation — with the pilot flash remaining a separate, owner-gated milestone using the Phase 2A pilot-readiness procedure plus §14's hardware validations.
