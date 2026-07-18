# NeuralAxe OS — Phase 2J Neural Navigation Identity Report

**Product:** NeuralAxe OS 0.1.0-dev · **Target:** Gamma / board 601 / BM1370
**Branch:** `neuralaxe-v0.1-navigation-identity` · **Commit:** `bbad2369`
**Release pair:** `v2.14.2-29-gbbad2369` (firmware app_desc == `www.bin` == `version.txt`)

**Verdict:** **PASS** — frontend shell/navigation only. Routing, page content, Fleet/thermal/mining/OTA behaviour, privacy controls, semantic health colors and all firmware sources are unchanged.

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Navigation is distinctive and recognizably NeuralAxe | PASS — Neural Command Rail: circuit spine, cyan section nodes, tactical control-module items, hardware Active Unit Dock |
| No longer resembles a generic admin sidebar | PASS — bespoke CSS/SVG geometry, brand-aligned green+cyan identity |
| Active route unmistakable, never a health alert | PASS — structural active state (fixed-cyan edge marker + route-lock chevron + elevated module); nav never uses health classes/tokens (DOM-asserted) |
| Command-rail GFX crisp, restrained, maintainable | PASS — pure CSS pseudo-elements (aria-hidden), no raster/WebGL/canvas/animation loop |
| Brand area stronger without duplicating telemetry | PASS — logo frame / `NeuralAxe OS` / `COMMAND DECK` / version badge; hardware pills stay in the top bar |
| Active Unit Dock is a deliberate hardware module | PASS — clipped silhouette, chip styling, masked hostname, NeuralAxe-Managed chip; existing data only |
| Sidebar + top bar form one shell | PASS — cyan hairline continuation + brand frame |
| Mobile navigation intentional | PASS — full-height drawer, touch modules, backdrop, Escape-to-close, current route visible |
| Keyboard/focus correct | PASS — dedicated cyan focus outline distinct from hover/active; Escape returns focus to the toggle |
| Reduced-motion respected | PASS — `prefers-reduced-motion` disables nav transitions |
| Accent separated from semantic health | PASS — dedicated `--nx-nav-*` tokens; red accent proven not to impersonate Critical (screenshot 14b) |
| No meaningful performance regression | PASS — ≈ +10 kB CSS, no JS growth, no external assets |
| All frontend tests pass | PASS — **420/420** ×3 consecutive clean-`npm ci` runs (+11 over 409) |
| Firmware behaviour unchanged | PASS — zero firmware sources; QEMU **83 Tests 0 Failures 0 Ignored** |
| No hardware or live-network access | PASS — source, synthetic intercepted fleet, tests, local dev server only |

## 2. Audit findings

PrimeNG Sakai shell: `app.layout.component` wraps a fixed `app-topbar` (brand + hardware pills + controls) and a floating 12 rem `.layout-sidebar` (`<app-menu>` + device card). Active detection is Angular `routerLinkActive="active-route"` (shared desktop/mobile). Two problems were fixed: the old active style used `var(--primary-color)` with colour+weight only (no structure; a red accent turned the active label red), and the device-card **hostname was unmasked**. Global `nx-` styles live in `command-deck.styles.scss` (loads after layout → can override); masking is CSS-only via `[sensitive-data]`.

## 3. Changed files (7, all frontend, zero firmware)

- `components/command-deck/command-deck.styles.scss` — navigation tokens, Active Unit Dock, brand-mark frame, top-bar hairline.
- `layout/styles/layout/_menu.scss` — the Neural Command Rail (spine, section nodes, tactical items, structural active state, focus, tablet/mobile, reduced-motion).
- `layout/app.sidebar.component.html` — device card → Active Unit Dock (masked hostname).
- `layout/app.topbar.component.html` — brand command module.
- `layout/app.layout.component.ts` — additive Escape-to-close for the mobile drawer.
- `layout/app.sidebar.component.spec.ts` (new) + `layout/app.layout.component.spec.ts` (new) — navigation/active-route/privacy/drawer tests.

## 4. Command-rail GFX

CSS pseudo-elements only (not in the a11y tree): a faint circuit spine per section group with a cyan diamond node per header, a bracketed accent cap along the rail top, and a hairline continuation across the top-bar foot. Crisp at any DPI, no raster, no JS, no animation loop.

## 5. Active-state semantics (Stages 4/12)

Elevated module background + **fixed-cyan** illuminated left edge + **fixed-cyan** route-lock chevron + border-wedge corner + accent-tinted label/icon + bold weight. The *structural selection marker is accent-independent cyan* (`--nx-nav-marker`), so a red accent can never make the active item read as a Critical alert — it only tints the label. Navigation never reuses `--nx-sem-*`/health classes (asserted: the active `<a>` carries `active-route` and none of the health classes). Focus is a dedicated cyan `outline` (not box-shadow/clip-path, which clip the ring), distinct from hover and active.

## 6. Brand module + 7. Active Unit Dock + 8. Top-bar integration

Brand: logo in a clipped tactical frame, `NeuralAxe OS` + version pill, `COMMAND DECK` subtitle — no telemetry duplication. Dock: hardware-module silhouette (clipped corner), cyan header node, target `Gamma 601` / `BM1370 · dev`, **privacy-masked** hostname, "NeuralAxe Managed" chip; secondary rows collapse ≤ 720 px height. Integration: a cyan hairline across the top-bar foot echoes the rail cap so the two read as one shell.

## 9. Responsive / 10. Motion / 11. Icons

Desktop full floating rail; tablet (992–1200 px) compact modules; mobile full-height drawer with touch-friendly modules, simplified GFX and a backdrop; Escape closes it. Transitions are 160 ms and disabled under reduced-motion. Existing PrimeIcons retained (consistent `pi-fw`); custom SVG/CSS only for decorative rail GFX and markers.

## 12. Pipeline results (Phase B)

| Step | Result |
|---|---|
| Clean commit verified | `bbad2369`, `version.txt = v2.14.2-29-gbbad236` (no `-dirty`) |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **420 / 420 / 420** |
| Production web build | clean (0 errors; pre-existing budget + `??` warnings only) |
| Full ESP-IDF build (v5.5.3 container) | clean — not dirty |
| App descriptor validation | `esp-miner.bin` app_desc == `v2.14.2-29-gbbad2369` |
| Matching firmware/web pair | **PAIR OK `v2.14.2-29-gbbad2369`** (app_desc == revision inside `www.bin`) |
| Merged factory image | 15 802 368 B |
| QEMU regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts, the normal NeuralAxe names |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean (no host paths/tokens; firmware unchanged from the prior 83/83 baseline) |
| Screenshot privacy scan | CLEAN (16 captures; gate enforced by the exporter) |
| Bundle-size impact | initial total 2.39 MB (≈ +10 kB CSS vs prior); no JS growth |

Release artifacts (out of git) in
`…/NeuralAxe Build Artifacts/navigation-identity-v0.1.0-dev-board601/`:
`release/` (www 3 145 728 B, ota 1 650 368 B, factory 15 802 368 B, config, manifest, SHA256SUMS),
`screenshots/` (16), `screenshot-privacy-scan.md`, `pipeline-evidence/`.

## 13. Unresolved / owner-gated

- None blocking. The pre-existing initial-bundle budget warning (2.39 MB vs a 2 MB warning / 3 MB error budget) is unchanged in character.
- Owner-gated: on-hardware visual confirmation of the rail across the physical fleet and accent themes.

## 14. Hardware and network access confirmation

No hardware or live network was accessed: no miner IPs contacted, no scans, no COM/USB, no esptool/bitaxetool against a device, no TCH dump. All evidence used source, a synthetic intercepted fleet, tests, the local dev server, and the ESP-IDF/QEMU build container.
