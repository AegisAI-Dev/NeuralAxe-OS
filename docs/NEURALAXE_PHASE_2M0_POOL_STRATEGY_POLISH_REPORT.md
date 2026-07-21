# NeuralAxe OS — Phase 2M.0: Pool Strategy Real-Device Polish Report

**Feature:** Pool Strategy Center — faithful history / restore / chain-context, and consistent "Custom / Unknown" chain labelling.
**Branch:** `neuralaxe-v0.1-pool-strategy-polish`
**Commit:** `d333dc4810e181ba7d24119b4847ca2ccc9a4ba5` — _fix: faithful Pool Strategy history and restore context_
**Firmware/web pair:** `v2.14.2-43-gd333dc4`
**Parent release:** `v2.14.2-41-g8a13664`
**Target:** Gamma · board 601 · BM1370
**Verdict:** ✅ **PASS** — Phase A + Phase B release gate all green. Frontend-only; **zero firmware/native source changed**.

---

## 1. What this phase fixed

A real Gamma-601 round-trip (unlabelled BTC config → BCH-labelled profile → restore to BTC) worked functionally, but one defect and one wording gap were found:

1. **`BCH → BCH` history bug.** The first switch recorded `BCH → BCH` instead of the true `Custom / Unknown → BCH`. Root cause: `saveHistory()` derived the source from the **live, mutable** chain context, but `finalizeSuccess()` calls `setActiveRecord(target)` **before** `saveHistory()`, which synchronously recomputes the context to the target (BCH). **Fix: capture an immutable, non-secret provenance snapshot before any PATCH; derive history/restore from it.**
2. **Restore never reactivated a prior labelled profile** (every verified restore went to Custom/Unknown), and the restore wording didn't distinguish an exact restore from an operational one.
3. **Compact chain label inconsistency:** the unlabelled/custom case rendered as "Unknown"/"Custom" instead of the canonical **"Custom / Unknown"**.

---

## 2. Immutable provenance & faithful history

`pool-provenance.ts` (pure, unit-tested) captures a `SwitchProvenance` in `captureAndApply` (switch) / `beginManualRestore` (restore), **before any mutation**, from the honest pre-mutation chain context + active record. Non-secret only (operation id, timestamps, source/target profile id + name + chain, **masked** source hosts/ports, `passwordReplaced`, `hadPreviousLabelledProfile`) — no password, no full wallet/worker/account, no raw API response. History source/target derive from it, never from live state. Result: the first switch now records **`Custom / Unknown → BCH`**; legitimate `BTC → BTC` / `BCH → BCH` remain possible; a restore records `BCH → <previous chain>`.

## 3. Deterministic restore reactivation & wording

`resolveRestoreActivation` decides the post-restore active state from **live non-secret identity only** (host/port/account — passwords are write-only and never compared): **exact-profile** (previous labelled profile matches → reactivate + its chain), **unlabelled** → Custom/Unknown, **mismatch** (live ≠ captured original → nothing active), **ambiguous** (several match → none chosen; prefers the exact previous profile ID), **profile-missing**. `restoreOutcome` distinguishes **exact** / **operational** / **partial** / **failed**; the operational message states plainly that a write-only password "was verified operationally but cannot be proven byte-for-byte restored" — no alarming generic warning, no false claim.

## 4. Consistent "Custom / Unknown" labelling

Single source of truth, **no enum change**: `CHAIN_SHORT.custom = 'Custom / Unknown'` (was `'Custom'`) plus a new `chainShortLabel(chain: PoolChain | 'unknown' | null)` that renders the `'unknown'` context as the same `"Custom / Unknown"`. `pool-chain.ts` (`unknownContext.short`), `pool-history.ts` (`buildSwitchRecord`) and the component (`chainShortFn`) all use it. The Command Deck already used the full `chainLabel`. Verified everywhere: history card, Markdown/JSON export, profile badge, switch-progress pill, chain-context card and deck card all read **"Custom / Unknown"** (never collapsed).

## 5. Preserved behaviour

Session-only passwords, PATCH/restart, browser-supervised rollback, interruption recovery, active/fallback + mining-resumed verification, BTC/BCH/Custom labels, Block Intelligence BCH context, Command Deck integration, history/export privacy guard, and firmware/web pair logic — all unchanged. No pool-setting API, Stratum, or firmware behaviour changed.

---

## 6. Verification results

### Frontend (Phase A + Phase B)
- **`npm run test:gate` × 3 → 1052 / 1052, exit 0** each (parent 1020 → **+32**).
- `npm ci` exit 0; production `npm run build` exit 0 (regen API + AOT + gzip; `version.txt` = `v2.14.2-43-gd333dc4`; generated dir clean).
- Five visual sanity states (dev server + mock): `Custom / Unknown → BCH` history, `BCH → Custom / Unknown` restore, operational-restore wording, restored `BTC Solo [Active]` marker, Custom/Unknown state; Command Deck card renders full `Custom / Unknown`.

### Firmware / release (Phase B)
| Gate | Result |
| --- | --- |
| Commit verified clean | `d333dc4`, 11 files, **0 firmware/native sources** |
| Identity agreement | header = frontend = `0.1.0-dev`, board 601 |
| Dirty-source check | tree clean, `git describe` = `v2.14.2-43-gd333dc4` (not dirty) |
| Full ESP-IDF build (v5.5.3, Docker, clean) | **exit 0** — `esp-miner.bin` 0x194da0 B, 60% app-partition free |
| App descriptor | **OK** — `v2.14.2-43-gd333dc4` |
| Firmware/web pair | **OK** — firmware and web both `v2.14.2-43-gd333dc4` |
| Merged/factory image | **15,802,368 bytes** |
| QEMU (test-ci, esp32s3) | **83 Tests, 0 Failures, 0 Ignored** |
| Release export | **OK** — 4 artifacts + manifest + SHA256SUMS |
| Manifest validation | **OK** — Gamma/601/BM1370, coherent pair |
| Release-gate battery | **13 / 13 passed** |
| Secrets / local-path scan | clean (no local paths, SECRET sentinels test-only, 0 in shipped bundle) |
| Password/history privacy scan | clean — `FORBIDDEN_KEY_RE` guard present; provenance/history/export secret-free |

**Build-environment notes (Windows Docker Desktop):** frontend host-built + packed via `GITHUB_ACTIONS=true` (the ESP-IDF image has no node/npm); container git aligned to the host checkout (`core.autocrlf true`, `core.filemode false`, `core.abbrev 7`, `safe.directory`) to avoid the CRLF/filemode "dirty" misread and the 7-vs-8-char abbreviation; `merge_bin.sh` run CR-stripped; the `test-ci/CMakeLists.txt` git symlink (stored as text on a `core.symlinks=false` Windows checkout) recreated as a real symlink in-container for the QEMU build and reverted to its exact tracked text afterward (tree verified clean).

---

## 7. Release artifacts

Staged at `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\pool-strategy-polish-v0.1.0-dev-board601`:

| Artifact | Size (bytes) | SHA256 |
| --- | --- | --- |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` | 15,802,368 | `e39cb5d592965cb3be4f5b8839014b4630f6b6e7e2a68f1d0b151dc90c89c03c` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` | 1,658,272 | `83999e5a6de6280a47412f36a5a74fb8c90f8a6008cc4f35cb90a91562d71f18` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` | 3,145,728 | `00c1c5aa1b2f4f0bd93366f3555e3171f731fa4935eb948283df5bd7f40ff03a` |
| `config-601.cvs` | 975 | `3c0f28f6112cf9e12ade941e3f82aa750fa3554a7e84b73340d665a571a55f22` |

Plus `…-manifest.json`, `…-SHA256SUMS.txt`, and `esp-miner-merged.bin` (identical to the factory image).

---

## 8. Committed file set (11 files, commit `d333dc4`)

**New (2):** `components/pool-strategy/pool-provenance.ts` (+ `.spec.ts`).
**Modified (9):** `pool-strategy.component.ts` / `.html` / `.spec.ts`, `pool-history.ts` (+ `.spec.ts`), `pool-profile.ts` (+ `.spec.ts`), `pool-chain.ts`, `pool-deck.spec.ts`.
The changeset grew from the 7 provenance files by 4 label-normalization files because the compact chain label is produced by the source-of-truth constant (`pool-profile.ts`) and the unknown-context derivation (`pool-chain.ts`). **Firmware C/native sources: unchanged.**

---

## 9. Real-device validation plan (Gamma / 601 / BM1370)

1. From an unlabelled device, switch to a BCH profile → history shows **`Custom / Unknown → BCH`**. 2. Switch BTC-profile → BCH → **`BTC → BCH`**. 3. Restore → **`BCH → BTC`**, the BTC profile shows **Active**, chain context returns to BTC. 4. Restore an unlabelled original → **`BCH → Custom / Unknown`**, nothing marked active. 5. Replace-password switch then manual restore → confirm the **operational** restore wording. 6. Confirm the Custom-labelled profile badge and the Command Deck card read **"Custom / Unknown"**, and history export contains no wallet/worker/password.

## 10. Confirmation

No physical miner, COM/USB, real BTC/BCH pool, pool account, or owner network was contacted. No firmware was uploaded; the firmware build and QEMU ran in a local ESP-IDF v5.5.3 Docker container against the committed source. Chain labels describe the selected pool profiles; NeuralAxe does not cryptographically determine which chain a pool mines.
