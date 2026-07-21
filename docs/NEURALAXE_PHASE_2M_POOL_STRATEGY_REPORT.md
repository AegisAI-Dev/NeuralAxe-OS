# NeuralAxe OS — Phase 2M: Pool Strategy Center Report

**Feature:** Pool Strategy Center — bounded BTC/BCH/Custom pool profiles and safe, reviewed, verified, auto-rolling-back pool switching.
**Branch:** `neuralaxe-v0.1-pool-strategy`
**Feature commit:** `8a1366400b5ded53db4c8e2d72e2d0383b2ed7ec` — _feat: add secure BTC/BCH Pool Strategy Center_
**Firmware/web pair:** `v2.14.2-41-g8a13664`
**Target:** Gamma · board 601 · BM1370 (exclusively)
**Verdict:** ✅ **PASS** — Phase A + owner security-hardening round + Phase B release gate all green. No firmware C/native source was changed by this feature.

---

## 1. What shipped

A new `Pool Strategy Center` (route `/pool-strategy`) that lets the owner:

- create **bounded pool profiles** (max 10) explicitly labelled **BTC / BCH / Custom**;
- review the **exact, masked** pool changes before switching;
- switch through the **existing** `PATCH /api/system` + restart APIs (no new firmware endpoint);
- **verify** reconnect + resumed mining + target-host before declaring success;
- **auto-roll-back** to the captured original when verification fails;
- **restore** the previous configuration on demand;
- see an **explicit chain context** (from the applied profile, never a hostname guess);
- recover honestly from a **browser-interrupted** switch.

It is controlled pool orchestration — **not** automatic profitability switching, and it never converts or moves funds.

Architecture mirrors the Stability Lab: pure, unit-tested modules (`pool-profile`, `pool-chain`, `pool-diff`, `pool-preflight`, `pool-switch-machine`, `pool-verify`, `pool-history`, `pool-recovery`, `pool-deck`, `pool-fixtures`) + one component + one root service (`PoolStrategyService`) + a `CanDeactivate` guard, with surgical integrations into routing, the nav rail, Command Deck and Block Intelligence.

---

## 2. Security-hardening round (owner blockers)

The first Phase A draft stored `set`-mode passwords in the persisted `PoolProfile`. The owner correctly rejected that. The final design:

- **Raw passwords are session-only and never persisted** — not in profiles, restore snapshots, history, exports, timelines, the interruption record, logs, or RxJS replay state. `PoolEndpoint` carries only `passwordMode: keep | set`.
- **Keep switch** → the password field is **omitted** from the PATCH (never `""`, never a masked placeholder); the device keeps its own password.
- **Replace switch** → the owner types the target password **and** the current password (for rollback) at switch time; both are held in component memory for the active operation only and wiped on complete/rollback/cancel/abort/failure/destroy. If the current password is not supplied, the switch is **blocked** with the exact wording:
  > _"The target profile replaces the pool password, but the current password cannot be read from the device. Enter the current password to enable verified rollback, or use Keep current password."_
  …with a **"Keep current password instead"** override that downgrades the switch to keep-mode.
- `realSecret()` rejects empty and masked (`*`, `•`, `(hidden)`) values so they can never be PATCHed.
- **Interruption recovery** (`pool-recovery.ts`): a **non-secret** record persisted at switch start drives an honest post-reload recovery state — `on-original / on-target / unknown / no-telemetry`, **never "Complete"** — with Retry-rollback / Verify-current actions and masked guidance when the original secret is gone.
- **Browser-supervised wording:** the confirm dialog states the dashboard must stay open until the switch and verification finish; a browser interruption becomes **Interrupted**, not Complete.
- **Safe success language:** verification never claims the password was "read back" — a successful pool connection only *implies* the pool accepted the worker credentials.

---

## 3. Chain context, preflight, state machine

- **Chain context** (`deriveChainContext`) = the applied profile's chain **only** when the device still holds that config; otherwise **Unknown** with a reason. Always shown with: _"Chain label describes the selected pool profile. NeuralAxe does not cryptographically determine which chain the pool is mining."_
- **Preflight** (no opaque score): supported device, online (fresh telemetry), firmware/web pair, mining-state known, no emergency, no Stability Lab session, no other switch, original captured, profile valid, differs from current, credentials available.
- **Switch FSM:** `idle → preflight → awaiting-confirmation → capturing → applying → restarting → reconnecting → verifying → mining-resumed → complete`, with `rolling-back / restoring / aborted / failed / interrupted`. Valid transitions only, timestamped, no credentials in state.
- **Verification** (telemetry-only): target settings submitted → device restarted & reconnected → mining resumed → target host observed → share activity (optional). Reconnect budget 90 s, verify 45 s (from the freshness contract). Success ≠ HTTP 200.
- **Rollback contract:** on failure, reapply the captured original (reinstating the original password only for pools whose password the switch replaced, using the session secret) → verify → report verified / partial / failed. Never leaves the target active by default; explicit Retry rollback.
- **On-chain failover safety:** a profile with no explicit fallback mirrors its primary (and shares its secret) so an automatic failover never lands on a different chain; every switch prefers the profile's primary (`useFallbackStratum: 0`).

---

## 4. Integrations

- **Block Intelligence:** when a **BCH**-labelled profile is active, a context banner states _"This miner is using a BCH-labelled pool profile. The current Block Intelligence provider displays Bitcoin network data."_ and configured-pool match claims are suppressed ("out of context"); Custom shows "chain matching unavailable"; BTC / no-profile keep the normal Phase 2L behavior.
- **Command Deck:** a compact Pool Strategy card (current profile, chain label, active/fallback pool, last switch result, restore availability, Open action); the block-match pill is gated by chain.

---

## 5. Privacy & history

Bounded sanitized local history (IDs/names, chain labels, **masked** host/port changes, timeline, verification/rollback results, reason) — never a password, full wallet/account, worker, SSID, Wi-Fi password, miner IP or raw API response; proven by a `FORBIDDEN_KEY_RE` deep-scan guard that also runs before every export. Existing `SensitiveData` masking + `AddressPipe` reused.

---

## 6. Timed sessions — deferred (Phase 2M.1)

Reliable "mine BCH for six hours, then restore BTC" requires on-device scheduling; a browser timer cannot survive a tab close / sleep / Wi-Fi drop and would strand the wrong pool. **No timer is shipped.** A minimal firmware sub-phase is proposed (owner approval required, not implemented):

- **NVS:** `pool_sched_target` / `pool_sched_restore` (settings blobs) / `pool_sched_deadline` / `pool_sched_state`.
- **API:** `POST /api/system/pool-session` (arm) / `DELETE` (cancel).
- **Boot/tick state machine** applies target, and at deadline (or on boot past deadline) reapplies restore + restarts.
- **Security caveats:** raw pool passwords must **not** be stored in ordinary unencrypted NVS blobs — a future implementation must audit NVS encryption availability and secret-at-rest handling, atomic target/original snapshots, crash-safe state transitions, boot-past-deadline restoration, and monotonic duration vs. unreliable wall-clock time.

---

## 7. Verification results

### Frontend (Phase A + hardening)
- **`npm run test:gate` × 3 → 1020 / 1020, exit 0** each (baseline 865 → **+155**).
- `npm ci` exit 0; production `npm run build` exit 0 (regen API + AOT + gzip + `version.txt` = `v2.14.2-41-g8a13664`).
- `main.js` 2.06 MB raw / **445.78 kB** transfer (the 2.10 MB budget overage is pre-existing: chart.js + primeng + gridstack + moment).
- Visual sanity (dev server + mock data): landing page, masked BCH→BTC confirm (exact blocker wording, session-only password fields, Keep-current override, dashboard-open statement, SWITCH disabled until secrets satisfied), active-switch timeline, BCH banner "out of context", Command Deck card, interruption-recovery panel.

### Firmware / release (Phase B)
| Gate | Result |
| --- | --- |
| Commit verified clean | `8a13664`, 36 files, **0 firmware/native sources**, `.claude/launch.json` excluded |
| Identity agreement | header = frontend = `0.1.0-dev`, board 601 |
| Dirty-source check | tree clean, `git describe` = `v2.14.2-41-g8a13664` (not dirty) |
| Full ESP-IDF build (v5.5.3, Docker) | **exit 0** — `esp-miner.bin` 0x194da0 B, 60% app-partition free |
| App descriptor | **OK** — `v2.14.2-41-g8a13664` |
| Firmware/web pair | **OK** — firmware and web both `v2.14.2-41-g8a13664` |
| Merged/factory image | **15,802,368 bytes** (board-601 factory) |
| QEMU (test-ci, esp32s3) | **83 Tests, 0 Failures, 0 Ignored** |
| Release export | **OK** — 4 artifacts + manifest + SHA256SUMS |
| Manifest validation | **OK** — Gamma/601/BM1370, coherent pair |
| Release-gate battery | **13 / 13 passed** |
| Secrets / local-path scan | clean (no D:\ paths, no hardcoded secrets, SECRET sentinels test-only) |
| Privacy scan | clean — 0 `SECRET_`/`DO_NOT_LEAK` in the shipped bundle; `FORBIDDEN_KEY_RE` guard present |

**Build environment notes (Windows Docker Desktop):** the frontend is host-built and packed via `GITHUB_ACTIONS=true` (the ESP-IDF image has no node/npm), avoiding the node_modules-shadow gotcha. The container git was aligned to the host checkout (`core.autocrlf true`, `core.filemode false`, `core.abbrev 7`, `safe.directory`) to fix the CRLF/filemode "dirty" misread and the 7-vs-8-char `git describe` abbreviation, giving a clean matching pair. The `test-ci/CMakeLists.txt` symlink (stored as text on a `core.symlinks=false` Windows checkout) was restored as a real symlink inside the container for the QEMU build and reverted to its exact tracked text afterward (tree verified clean).

---

## 8. Release artifacts

Exported to `release-export/` and staged at
`D:\Companys\Neuralshield\Firmware\NeuralAxe Build Artifacts\pool-strategy-v0.1.0-dev-board601`:

| Artifact | Size (bytes) | SHA256 |
| --- | --- | --- |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin` | 15,802,368 | `6a1cd9413069b01a438367a11b863e883338de0a7e41161df657cbbf2a303e39` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin` | 1,658,272 | `8036b5d05315f6d3fac2efa763e17cd41ca0b1d00142572e5d15f7729a162a01` |
| `NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` | 3,145,728 | `d3aed222e33b0131d0e781f2c444efaa6c96c5bdbef04c400ea3ca1149b2ee43` |
| `config-601.cvs` | 975 | `3c0f28f6112cf9e12ade941e3f82aa750fa3554a7e84b73340d665a571a55f22` |

Plus `NeuralAxe-OS-v0.1.0-dev-Gamma-601-manifest.json`, `…-SHA256SUMS.txt`, and `esp-miner-merged.bin` (identical to the factory image).

---

## 9. Committed file set (36 files, feature commit `8a13664`)

**New (26):** under `components/pool-strategy/` — `pool-profile`, `pool-chain`, `pool-diff`, `pool-preflight`, `pool-switch-machine`, `pool-verify`, `pool-history`, `pool-deck`, `pool-fixtures`, `pool-recovery` (each `.ts` + `.spec.ts` = 20), plus `pool-strategy.component.ts/.html/.spec.ts`; `services/pool-strategy.service.ts` + `.spec.ts`; `guards/pool-switch.guard.ts`.
**Modified (10):** `app-routing.module.ts`, `app.module.ts`, `layout/app.menu.component.ts` (+ `.spec.ts`), `layout/app.sidebar.component.spec.ts`, `components/command-deck/command-deck.component.ts` + `.html`, `components/block-intelligence/block-intelligence.component.ts` + `.html` + `.spec.ts`.
**Firmware C/native sources: unchanged.** `.claude/launch.json` was **not** committed.

---

## 10. Real-device pilot plan (Gamma / 601 / BM1370)

1. Flash the factory image (or OTA the www + app pair). 2. Open Pool Strategy; create a starter profile from the current config (label BTC). 3. Create a second BTC profile on a different reachable pool; switch and confirm reconnect + mining + target-host verify. 4. Force a failure (unreachable-host profile) → confirm automatic rollback to the original. 5. Test a replace-password switch: confirm it blocks without the current password, then completes with both entered, and rolls back with the original password on failure. 6. **Restore previous**; confirm verified restore. 7. Label a real BCH pool profile, switch, and confirm the BCH banner + suppressed matches in Block Intelligence. 8. Interrupt a switch (refresh mid-switch) and confirm the recovery panel is honest and never shows "Complete". 9. Confirm passwords are never displayed and history/export are sanitized.

---

## 11. Confirmation

No physical miner, COM/USB, real Stratum pool, pool account, or owner network was contacted during development or validation. No firmware was uploaded; the firmware build and QEMU ran in a local ESP-IDF v5.5.3 Docker container against the committed source. Chain labels describe the selected pool profiles; NeuralAxe does not cryptographically determine which chain a pool mines.
