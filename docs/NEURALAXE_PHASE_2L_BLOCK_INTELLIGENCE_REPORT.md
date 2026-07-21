# NeuralAxe OS — Phase 2L Bitcoin Block Intelligence Report

**Product:** NeuralAxe OS 0.1.0-dev
**Branch:** `neuralaxe-v0.1-block-intelligence`
**Commit:** `1526c0ded3afeb3860a50e426b35c87c8e4b9910` — *"feat: Bitcoin Block Intelligence with evidence-based pool attribution"*
**Release pair:** **v2.14.2-37-g1526c0de** (firmware app-desc == embedded www == version.txt)
**Primary target:** Gamma · board 601 · BM1370
**Verdict:** ✅ **PASS** — Phase A (implementation) and Phase B (build/release pipeline) both green. No firmware source changed; no hardware or live network accessed.

---

## 1. Objective

Add a privacy-conscious **Bitcoin Block Intelligence** workspace: a recent-block timeline, evidence-based pool attribution, and local configured-pool matching, plus a compact Command Deck glance. This is network intelligence and attribution — not mining automation — and it must **never** imply that this individual NeuralAxe device found a block. The only "found a block" evidence in the product remains the device's own Stratum signal (`SystemInfo.blockFound`), which is untouched and lives entirely outside this subsystem.

## 2. Attribution certainty model (evidence, not proof)

Public providers supply **evidence, not proof**, so the strongest state the currently-implemented public providers (mempool.space, Esplora) ever emit is **Provider-reported**. `Confirmed` is **reserved** for a future source with direct authoritative provenance (an explicit pool, Stratum or node contract) and is never produced by a public adapter.

| State | Meaning | Example wording |
|---|---|---|
| `confirmed` | **Reserved** — future authoritative source only (never shown for public data) | — |
| `provider-reported` | The block-data provider attributes this block to a pool (`extras.pool`) | "Provider attributes this block to Foundry USA" |
| `strong` | Coinbase tag clearly names a known pool (name/label token) | "Strong coinbase-tag match: AntPool" |
| `probable` | Likely pool from alias/domain evidence, **or** downgraded conflicting evidence | "Likely Foundry USA based on coinbase alias/domain evidence" |
| `unknown` | Coinbase present but no pool identified | "Unknown pool" |
| `unattributed` | No coinbase/evidence available | "Unattributed" |

Derivation (`attribution.ts` → `deriveAttribution`): provider `extras.pool` (no conflict) ⇒ **provider-reported**; provider pool with a coinbase tag naming a *different* known pool ⇒ downgraded to **probable**; no provider pool + coinbase **name-token** match ⇒ **strong**; no provider pool + coinbase **domain/alias** match ⇒ **probable**; coinbase present but unmatched ⇒ **unknown**; nothing ⇒ **unattributed**. `identifyPoolWithKind` distinguishes name (strong) from domain (probable). Confidence severity is fixed/accent-independent and never red — an unattributed block is not an error. Tests assert the public adapters never emit `confirmed`, that Unknown ≠ Unattributed, and that no reason/label ever asserts an individual-miner "found it" claim. Configured-pool matching (`Active/Fallback pool match`) is a pool-identity alignment only and never implies the local device produced the block.

## 3. Outbound privacy contract (proven)

Every external request is a **bare GET** built solely by the pure URL builders in `provider-request.ts` from the static provider descriptor plus (for detail) a percent-encoded block hash — no query string, no body, no `Authorization` header, and no way to embed device/config values (a hostile hash cannot inject `?`/`&`/`=`). `block-outbound-privacy.spec.ts` flips a `liveProviders` seam and inspects **every** request via `HttpTestingController`, asserting the URL carries none of: wallet, worker, pool password, SSID, Wi-Fi password, hostname, miner IP, hashrate, temperature, tuning, Fleet data, or the configured active/fallback pool hosts; that provider fallback (mempool → Esplora) appends no sensitive context; and that configured-pool matching happens **locally, only after** the public response is received.

Documented outbound contract:

| Purpose | Method | URL |
|---|---|---|
| mempool.space recent blocks | GET | `https://mempool.space/api/v1/blocks` |
| mempool.space block detail | GET | `https://mempool.space/api/v1/block/{hash}` |
| Esplora recent blocks | GET | `https://blockstream.info/api/blocks` |
| Esplora block detail | GET | `https://blockstream.info/api/block/{hash}` |

Full details in `docs/NEURALAXE_BLOCK_INTELLIGENCE_PROVIDER_CONTRACT.md`.

## 4. Capability / firmware conclusion

**No firmware or backend change.** The entire feature is browser-side. CORS was validated empirically from a foreign browser origin for both providers (200 + JSON), so **frontend-direct HTTPS is viable and no firmware proxy is required** — no blocking capability gap. HTTP-page→HTTPS-provider is never mixed-content-blocked (the operator's browser fetches; the device never contacts the provider). Every protected firmware behavior (mining/Stratum, primary/fallback pools, Wi-Fi, tuning, hard thermal protection, Stability Lab, Fleet, OTA, NVS keys, firmware/web pair logic) is untouched. Verified: zero `.c/.h/CMakeLists` modified; the firmware QEMU unit suite passes **83/0/0 unchanged**.

## 5. Provider architecture

Provider-neutral `BlockProviderDescriptor` + pure `normalize*` adapters feed internal models; the service orchestrates HTTP/cache/backoff. Priority ladder (data-declared; only the public tier is implemented): `local → lan → public → cache → offline`. The Esplora adapter already speaks the schema a future self-hosted Esplora/electrs/NeuralAxe Node would serve. Refresh is one shared, visibility-aware stream (~45 s foreground / 300 s hidden), exponential backoff (15 s→300 s cap), 12 s timeout, manual refresh, deduped identical tips, bounded window (≤20 blocks) and bounded response (≤512 KB). Cache/stale/offline/retrying states are explicit; replaced-tip is surfaced honestly (`detectTipChange`) and never silently merged.

## 6. Phase A verification

| Check | Result |
|---|---|
| Frontend tests (`npm run test:gate`) | **822 / 822, exit 0** (baseline 667 → +155) |
| Production build (AOT / strictTemplates) | exit 0 (pre-existing NG8102 warnings only) |
| Deterministic screenshots (Puppeteer + Edge) | 11 captured (24-shot requirement removed by owner) |
| Screenshot privacy scan | **PASS** — 0 leaks |
| Bundle (initial) | 2.61 MB < 3 MB error budget (2 MB warning pre-existing) |
| Firmware sources | unchanged |

*Note: the 11 screenshots pre-date the attribution-wording correction (they still show the former "Confirmed" label); they were retained as a non-blocking visual sanity check and not re-captured, per owner instruction.*

## 7. Phase B verification (build & release pipeline)

Owner committed the change set as `1526c0de`. Verified via `.git` metadata only; no git commands were run.

| Step | Result |
|---|---|
| Clean commit | `version.txt` = `v2.14.2-37-g1526c0de`, no `-dirty` |
| `npm ci` | exit 0 |
| Frontend tests ×3 (`test:gate`) | 822 / 822 each, exit 0 |
| Production frontend build | exit 0 |
| ESP-IDF v5.5.3 build (`GITHUB_ACTIONS=true idf.py build`) | exit 0; `esp-miner.bin` 1,658,272 B, `www.bin` 3,145,728 B |
| App-descriptor validation (`--check-bin`) | **APP DESC OK** — v2.14.2-37-g1526c0de |
| Firmware/web pair (`--check-pair`) | **PAIR OK** — both v2.14.2-37-g1526c0de |
| Merged factory image (`merge_bin.sh`) | 15,802,368 B |
| Release export (`export_release.py`) | **EXPORT OK** — 4 artifacts + manifest + SHA256SUMS |
| Manifest validation (`validate_manifest.py`) | **VALIDATION OK** — Gamma/601/BM1370 |
| Release-gate battery (`test_release_gates.py`) | **13 passed, 0 failed** |
| QEMU (`esp32s3`, Unity) | **83 Tests / 0 Failures / 0 Ignored — OK** |
| Secrets / local-path scan | clean (only env-overridable dev-tool path defaults in the screenshot harness) |
| Screenshot privacy scan | **PASS** (11 screens) |

### 7.1 Build-environment notes (Windows host + Linux container)

- **7-vs-8-char git-abbrev pair:** IDF derived the app_desc as the 8-char `v2.14.2-37-g1526c0de` while `git describe`/`version.txt` produced the 7-char `…g1526c0d`. Resolved by reconciling `version.txt` (a gitignored build output) to the authoritative device app_desc value and repacking `www.bin` via `spiffsgen.py` — after which `--check-pair` agreed on `v2.14.2-37-g1526c0de`. This never touched a committed file.
- **CRLF / dubious-ownership:** the container build set `git config core.autocrlf true`, `core.filemode false`, and `safe.directory` so a CRLF Windows checkout is read as clean (no `-dirty`).
- **`test-ci/CMakeLists.txt` symlink:** on a Windows checkout the `../test/CMakeLists.txt` symlink is a plain text file, which the Linux CMake cannot parse. Resolved for the QEMU run with a read-only single-file bind-mount overlay of the real content — the tracked tree was not modified.

Release artifacts (`sourceRevision == firmwareRevision == webRevision == v2.14.2-37-g1526c0de`):
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` (3,145,728 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` (1,658,272 B)
- `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` (15,802,368 B)
- `config-601.cvs` (975 B)
- `…-manifest.json`, `…-SHA256SUMS.txt`

## 8. Change set (41 files; 32 new, 9 modified — all under `main/http_server/axe-os/` plus `docs/`)

**New services (`services/block-intelligence/`):** `block-intelligence.model.ts`, `pool-normalize.ts`, `attribution.ts`, `block-normalize.ts`, `mempool-provider.ts`, `esplora-provider.ts`, `block-metrics.ts`, `block-refresh.ts`, `block-fixtures.ts`, `block-intelligence.service.ts`, `block-format.ts`, `provider-request.ts` (+ 11 specs incl. `block-privacy.spec.ts`, `block-outbound-privacy.spec.ts`).
**New component (`components/block-intelligence/`):** `.component.ts`, `.html`, `.styles.scss`, `.component.spec.ts`, `.nav.spec.ts`.
**New deck (`components/command-deck/`):** `block-deck.ts` (+ spec).
**New scripts/docs:** `scripts/capture-block-intelligence.mjs`, `docs/NEURALAXE_BLOCK_INTELLIGENCE_PROVIDER_CONTRACT.md`.
**Modified:** `app-routing.module.ts`, `app.module.ts`, `app.menu.component.ts`, `command-deck.component.ts/.html`, `command-deck.styles.scss`, `styles.scss`, `app.menu.component.spec.ts`, `app.sidebar.component.spec.ts` (route inventory 13→14).

## 9. Real-hardware pilot plan (owner-run, post-merge)

On the Gamma 601 (on the LAN, with internet): confirm the operator's browser reaches mempool.space (fresh state); verify active/fallback labels match the device's *actually configured* pools; pull the browser offline to confirm cached → unavailable; and confirm via devtools that **no outbound request carries** wallet/worker/SSID/telemetry or the configured pool hosts. No device tuning/pool/Wi-Fi changes are involved.

## 10. Limitations

The pool-identity registry is curated and heuristic; unlisted pools honestly yield Unknown/Unattributed. A future LAN/local-node provider is designed-for but not implemented. In-app new-block/replaced-tip events are per-session only (no persistence). The pre-existing NG8102 template warnings and the ~2 MB bundle-size warning are unchanged and out of scope.

---

*Evidence (11 screenshots, `privacy-scan.json`, ESP-IDF + QEMU build logs, release artifacts, manifest, SHA256SUMS) archived outside git at `D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\block-intelligence-v0.1.0-dev-board601`. No hardware, COM/USB, live network beyond the permitted read-only public-provider CORS/format validation, esptool/bitaxetool-against-hardware, or private TCH dump was accessed.*
