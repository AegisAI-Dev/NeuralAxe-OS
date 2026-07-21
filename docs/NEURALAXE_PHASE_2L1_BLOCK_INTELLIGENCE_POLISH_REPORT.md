# NeuralAxe OS — Phase 2L.1 Block Intelligence Polish Report

**Product:** NeuralAxe OS 0.1.0-dev
**Branch:** `neuralaxe-v0.1-block-intelligence-polish`
**Commit:** `b3a16002cc2693e6f0ad0b5c3fb2d7f24b018954` — *"feat: polish block-height terminology and coinbase evidence"*
**Release pair:** **v2.14.2-39-gb3a16002** (firmware app-desc == embedded www == version.txt)
**Primary target:** Gamma · board 601 · BM1370
**Verdict:** ✅ **PASS** — Phase A (implementation) and Phase B (build/release pipeline) both green. No firmware source changed; no hardware or live network accessed.

---

## 1. Objective

A narrow, real-device polish of Phase 2L Block Intelligence:

1. **Block-height terminology** — real-device validation showed Block Intelligence reporting the latest *mined* network block (e.g. 958983) while the miner dashboard showed the miner's *current work/candidate* height (958984). Correct Bitcoin behavior (after block N is found, miners work on N+1), but the label "Block Height" was ambiguous.
2. **Coinbase evidence rendering** — the Block Detail drawer showed provider coinbase evidence containing non-printable and Unicode replacement (U+FFFD �) characters as visual garbage.

No new features, providers, autonomous behavior or firmware networking were added.

## 2. Terminology contract

| Label | Meaning | Where |
|---|---|---|
| **Current Work Height** | the next candidate block height the miner is hashing toward (`info.blockHeight`) | Command Deck · Pool/Block/Network (was "Block Height") + tooltip |
| **Latest Mined Block** | the latest completed canonical block reported by the provider | Block Intelligence deck glance + workspace summary (was "Latest block") + tooltip |

The device's own `blockFound` "Block Signal" card (a separate, protected feature) is unchanged.

## 3. Height-relationship model

New pure helper `services/block-intelligence/height-relationship.ts` classifies the latest-mined vs current-work relationship, honestly and without alarms:

- **expected-next** (`work == mined + 1`) — neutral/healthy (severity `ok`);
- **same-height** (`work == mined`) — informational, not an error;
- **miner-behind** (`work < mined`) — restrained info;
- **miner-ahead-by-more-than-one** (`work > mined + 1`) — restrained info, explicitly *not a reorg diagnosis*;
- **unavailable** — either value missing/invalid (rejects NaN / Infinity / negative / non-integer).

The Command Deck renders this as a green line — *"Your miner — Current Work Height #N+1: Working on the expected next block. After network block N was found, miners normally work on candidate height N+1"* — and hides it when unavailable. The normal one-block difference never generates a stale/provider warning.

## 4. Coinbase evidence sanitization

New pure helper `services/block-intelligence/coinbase-sanitize.ts` — `sanitizeCoinbase({ascii, hex})` — **prefers the raw `coinbaseRaw` hex bytes** (the source of truth) over the provider's lossy ASCII, so it never inherits or emits replacement-character noise. Bounded outputs:

- `readable` — printable ASCII kept, other bytes → space, whitespace collapsed (≤ 80 chars);
- `escaped` — printable ASCII kept, other bytes → `\xNN`, backslash → `\\` (≤ 220 chars);
- `hex` — bounded lowercase hex from the source bytes (≤ 64 bytes), collapsed by default;
- `originalLength`, `truncated`, `status` (`clean` / `sanitized` / `binary` / `empty`), `hasReadable`.

It never parses HTML, never uses innerHTML, and never executes control sequences — all outputs are plain, bounded strings. The drawer shows a **Coinbase evidence** section (Readable tag / Escaped / Length / collapsed **"Show bounded hex"** toggle) with the copy *"Coinbase transactions may contain binary and non-printable data. NeuralAxe shows a sanitized preview while preserving bounded evidence for attribution."* The disclaimer that attribution does not prove this device found the block remains visible.

## 5. Attribution integrity

Sanitization does not weaken attribution. `deriveAttribution` sanitizes the coinbase **once** and matching uses **only the readable form** — the escaped and hex display views are never fed to matching and cannot create a stronger match. `AttributionInput` was updated (`coinbaseTagAscii` → `coinbaseAscii`, added `coinbaseHex`, dropped the always-null `coinbaseTagId`); `sanitizeTag` now delegates to `sanitizeCoinbase`. All confidence states are unchanged: provider-reported / strong (coinbase name token) / probable (domain-alias or downgraded conflict) / unknown / unattributed; `confirmed` remains reserved for a future authoritative source. Verified by integrity tests over NUL-prefixed binary, mixed binary/ASCII, replacement-character, HTML/script bytes, ANSI/control sequences, no-readable, very-long, and binary-only-never-matches inputs.

## 6. Capability / firmware conclusion

**No firmware or backend change.** Block-height *calculations* are unchanged (only labels and a derived, honest relationship were added); coinbase text is sanitized for display only. Every protected behavior (provider abstraction, mempool/Esplora adapters, confidence model, outbound-privacy contract, active/fallback matching, cache/polling/backoff, mining/Stratum, pools, Stability Lab, Fleet, thermal, OTA, pair logic) is preserved. Verified: zero `.c/.h/CMakeLists` modified; the firmware QEMU unit suite passes **83/0/0 unchanged**.

## 7. Phase A verification

| Check | Result |
|---|---|
| Frontend tests (`npm run test:gate`) | **865 / 865, exit 0** (was 822 → +43) |
| Production build (AOT / strictTemplates) | exit 0 (pre-existing NG8102 warnings only) |
| Visual sanity check (Puppeteer + Edge) | 11 screens (no large run required) |
| Screenshot privacy scan | **PASS** — 0 leaks |
| Bundle (initial) | ~2.61 MB < 3 MB error budget (unchanged category) |
| Firmware sources | unchanged |

Deck shows **Current Work Height 870,001** / **Latest Mined Block 870000** with the green "expected next block (N+1)" line; the drawer shows the sanitized coinbase `\x03\x87\x9A\x0E\x00/Foundry USA Pool/\x00\xFF` with the bounded-hex toggle.

## 8. Phase B verification (build & release pipeline)

Owner committed the change set as `b3a16002`. Verified via `.git` metadata only; no git commands were run.

| Step | Result |
|---|---|
| Clean commit | `version.txt` = `v2.14.2-39-gb3a16002`, no `-dirty` |
| `npm ci` | exit 0 |
| Frontend tests ×3 (`test:gate`) | 865 / 865 each, exit 0 |
| Production frontend build | exit 0 |
| ESP-IDF v5.5.3 build (`GITHUB_ACTIONS=true idf.py build`) | exit 0; `esp-miner.bin` 1,658,272 B, `www.bin` 3,145,728 B |
| App-descriptor validation (`--check-bin`) | **APP DESC OK** — v2.14.2-39-gb3a16002 |
| Firmware/web pair (`--check-pair`) | **PAIR OK** — both v2.14.2-39-gb3a16002 |
| Merged factory image (`merge_bin.sh`) | 15,802,368 B |
| Release export (`export_release.py`) | **EXPORT OK** — 4 artifacts + manifest + SHA256SUMS |
| Manifest validation (`validate_manifest.py`) | **VALIDATION OK** — Gamma/601/BM1370 |
| Release-gate battery (`test_release_gates.py`) | **13 passed, 0 failed** |
| QEMU (`esp32s3`, Unity) | **83 Tests / 0 Failures / 0 Ignored** |
| Secrets / local-path scan | clean (only env-overridable dev-tool path defaults in the screenshot harness) |
| Screenshot privacy scan | **PASS** (11 screens) |

Build-environment notes (unchanged from Phase 2L, none touched a committed file): built in the `espressif/idf:v5.5.3` container mounted at `/workspace` with `core.autocrlf true` / `core.filemode false` / `safe.directory`; the **7-vs-8-char git-abbrev pair** recurred (app_desc 8-char `…gb3a16002` vs `git describe` 7-char `…gb3a1600`) and was reconciled by rewriting the gitignored `dist/version.txt` to the device app_desc value and repacking `www.bin` via `spiffsgen.py`; the Windows `test-ci/CMakeLists.txt` symlink was overlaid read-only for the QEMU build.

Release artifacts (`sourceRevision == firmwareRevision == webRevision == v2.14.2-39-gb3a16002`):
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` (3,145,728 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` (1,658,272 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` (15,802,368 B)
- `config-601.cvs` (975 B)
- `…-manifest.json`, `…-SHA256SUMS.txt`

## 9. Change set (19 files, all under `main/http_server/axe-os/`)

**New (4):** `services/block-intelligence/height-relationship.ts` (+spec), `coinbase-sanitize.ts` (+spec).

**Modified (15):** services — `block-intelligence.model.ts`, `attribution.ts` (+spec), `mempool-provider.ts`, `esplora-provider.ts`, `block-fixtures.ts`; components/block-intelligence — `.component.ts`, `.html`, `.styles.scss` (+spec); components/command-deck — `.component.ts`, `.html`, `.styles.scss` (+spec); `scripts/capture-block-intelligence.mjs`.

## 10. Real-hardware pilot plan (owner-run, post-merge)

On the Gamma 601: confirm the deck reads **Current Work Height** = latest-mined + 1 with the green "expected next block" note (no false stale/provider warning); open a real block whose coinbase carries binary and confirm the drawer shows a clean readable tag + `\xNN` escaped evidence + bounded hex (no `�`/garbage); confirm attribution strength (provider-reported / strong / probable) is unchanged.

## 11. Limitations

Terminology and coinbase-display polish only — no change to block-height calculations, attribution logic, providers or firmware. The pool-identity registry remains curated and heuristic. The pre-existing NG8102 template warnings and the ~2 MB bundle-size warning are unchanged and out of scope.

---

*Evidence (11 screenshots, `privacy-scan.json`, ESP-IDF + QEMU build logs, release artifacts, manifest, SHA256SUMS) archived outside git at `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\block-intelligence-polish-v0.1.0-dev-board601`. No hardware, COM/USB, live network beyond the permitted read-only public-provider validation, esptool/bitaxetool-against-hardware, or private TCH dump was accessed.*
