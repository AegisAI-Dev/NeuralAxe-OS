# NeuralAxe OS — Phase 2J.1 Shell Polish Report

**Product:** NeuralAxe OS 0.1.0-dev · **Target:** Gamma / board 601 / BM1370
**Branch:** `neuralaxe-v0.1-shell-polish` · **Commit:** `1c411d52`
**Release pair:** `v2.14.2-31-g1c411d52` (firmware app_desc == `www.bin` == `version.txt`)

**Verdict:** **PASS** — a restrained shell polish. The accepted Phase 2J Neural
Command Rail identity is preserved; routing, page content, Fleet/thermal/mining/OTA
behaviour, privacy controls and all firmware sources are unchanged.

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Main content aligns naturally with the Command Rail | PASS — content top now equals the sidebar top (measured 0 px offset, was +77 px) |
| Excessive top whitespace removed | PASS — content moved 77 px up; only the intended 28 px gap below the fixed top bar remains |
| Rail labels materially more readable | PASS — wrapping cut 6 → 2 (only "Display & Appearance" / "Legacy Dashboard" wrap, cleanly) |
| Rail width + main offset share a token | PASS — `--sidebar-width`; content margin is `calc(--sidebar-width + 2rem)` |
| No desktop/mobile overflow introduced | PASS — verified 1920/1600/1440/1366/1280/tablet/mobile, no page horizontal overflow |
| Phase 2J navigation identity preserved | PASS — spine, nodes, cyan structural active marker, dock, brand all unchanged |
| Missing live web revision not shown as a mismatch | PASS — `boot-match` renders a calm green "Boot pair match" |
| Boot-time match communicated honestly | PASS — "Boot pair match" + "Live verification unavailable" |
| Live match and boot match remain distinct | PASS — distinct primary labels; live-verified vs boot-verified |
| Actual mismatches remain prominent | PASS — live mismatch = red; boot mismatch = amber (not hidden) |
| All frontend tests pass | PASS — **434/434** ×3 consecutive clean-`npm ci` runs (+14 over 420) |
| Firmware behaviour unchanged | PASS — zero firmware sources; QEMU **83 Tests 0 Failures 0 Ignored** |
| No hardware or live-network access | PASS — source, synthetic intercepted data, tests, local dev server only |

## 2. Audit findings

- **#1 (content too low) was a Phase 2J regression:** the Phase 2J
  `.layout-topbar { position: relative }` (added to anchor the hairline `::after`)
  overrode `_topbar.scss`'s `position: fixed`, pulling the bar into normal flow.
  Its 77 px height was then counted twice — once as flow space (container top 77 px)
  and once inside the main content `padding-top: calc(--topbar-height + 2rem)` —
  landing content at 182 px against the 105 px sidebar top.
- **#2 (wrapping):** `--sidebar-width: 12rem` left ~85 px of non-text inset, so
  6 labels wrapped.
- **#3 (pair status):** `deriveVersionState` collapsed every unavailable-live case
  into `'unverified'`, so a proven boot match rendered the alarming
  "Live web version unavailable".

## 3. Changed files (10, all frontend, zero firmware)

- `components/command-deck/command-deck.styles.scss` — removed the `.layout-topbar`
  `position: relative` override (the `::after` hairline still anchors to the fixed bar).
- `layout/styles/theme/themes/vela/_variables.scss` — `--sidebar-width: 12rem → 13.5rem`.
- `layout/styles/layout/_menu.scss` — tightened item/icon/section insets; tablet
  width override 12.75rem.
- `services/version-state.ts` (+`.spec.ts`) — new pure `derivePairStatus` (5-state);
  `deriveVersionState` untouched.
- `components/update/update.component.ts` / `.html` / `.scss` (+`.spec.ts`) —
  pair-status hierarchy + explicit field labels.
- `layout/app.sidebar.component.spec.ts` — navigation label-rendering test.

## 4. Shell geometry (measured, 1440 px, 14 px rem)

| | Before | After |
|---|---|---|
| `.layout-topbar` position | relative (in flow) | **fixed** |
| container top | 77 px | **0 px** |
| content top | 182 px | **105 px** |
| content top − sidebar top | +77 px | **0 px** (aligned) |
| gap below top bar | 105 px | 28 px |
| sidebar width | 168 px (12rem) | 189 px (13.5rem) |
| wrapping labels | 6 | 2 |
| page horizontal overflow | none | none |

## 5. Honest pair-status model (`derivePairStatus`)

| Inputs | State | Severity / pill | Primary · Secondary |
|---|---|---|---|
| fw & live, live == fw | `live-match` | ok · green | Live pair match |
| fw & live, live ≠ fw | `live-mismatch` | danger · red | Live pair mismatch |
| fw & boot (no live), boot == fw | `boot-match` | ok · green | Boot pair match · Live verification unavailable |
| fw & boot (no live), boot ≠ fw | `boot-mismatch` | warn · amber | Boot pair mismatch · restart-or-reflash |
| insufficient data | `unknown` | info · neutral | Pair status unknown |

Revisions are compared exactly after whitespace trimming (`-dirty` and all
suffixes are significant). The Update fields are now *Firmware Revision / Web
Revision at Boot / Live Web Revision (Not reported) / Pair Verification*. Pills
use fixed `--nx-sem-*` tokens, never the accent — a green boot-match stays green
under a red accent (`20b-red-accent-boot-match`), and a live mismatch stays a
prominent semantic red.

## 6. Pipeline results (Phase B)

| Step | Result |
|---|---|
| Clean commit verified | `1c411d52`, `version.txt = v2.14.2-31-g1c411d5` (no `-dirty`) |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **434 / 434 / 434** |
| Production web build | clean (0 errors; pre-existing budget + `??` warnings only) |
| Full ESP-IDF build (v5.5.3 container) | clean — not dirty |
| App descriptor validation | `esp-miner.bin` app_desc == `v2.14.2-31-g1c411d52` |
| Matching firmware/web pair | **PAIR OK `v2.14.2-31-g1c411d52`** |
| Merged factory image | 15 802 368 B |
| QEMU regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts, the normal NeuralAxe names |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean (no host paths/tokens; firmware unchanged) |
| Screenshot privacy scan | CLEAN (21 captures; gate enforced by the exporter) |
| Bundle-size impact | initial total 2.39 MB (no meaningful delta) |

Release artifacts (out of git) in
`…/NeuralAxe Build Artifacts/shell-polish-v0.1.0-dev-board601/`:
`release/` (www 3 145 728 B, ota 1 650 368 B, factory 15 802 368 B, config,
manifest, SHA256SUMS), `screenshots/` (21), `screenshot-privacy-scan.md`,
`pipeline-evidence/`.

## 7. Unresolved / owner-gated

- None blocking. The pre-existing initial-bundle budget warning is unchanged.
- Owner-gated: on-hardware confirmation of the alignment/width and the pair-status
  presentation on the real Gamma.

## 8. Hardware and network access confirmation

No hardware or live network was accessed: no miner IPs contacted, no scans, no
COM/USB, no esptool/bitaxetool against a device, no TCH dump. All evidence used
source, synthetic intercepted data, tests, the local dev server, and the
ESP-IDF/QEMU build container.
