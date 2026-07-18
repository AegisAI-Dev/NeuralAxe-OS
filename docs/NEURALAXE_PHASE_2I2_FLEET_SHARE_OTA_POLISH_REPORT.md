# NeuralAxe OS — Phase 2I.2 Fleet Share Intelligence, Selection Polish & OTA Filename Compatibility

**Product:** NeuralAxe OS 0.1.0-dev · **Target:** Gamma / board 601 / BM1370
**Branch:** `neuralaxe-v0.1-fleet-share-ota-polish` · **Commit:** `5b9b3abf`
**Release pair:** `v2.14.2-27-g5b9b3abf` (firmware app_desc == `www.bin` == `version.txt`)

**Verdict:** **PASS** — frontend/product only; discovery, per-device API calls, control
behavior, health/classification semantics, the OTA upload architecture and all firmware
sources are unchanged.

## 1. Verdict summary

| Success criterion | Result |
|---|---|
| Honest aggregated accepted/rejected share counters | PASS — `fleetShares()` sums current counters only from online devices reporting both valid counters |
| Reporting coverage visible | PASS — "from N of M reporting" on the tile; per-device breakdown on the Mining tab |
| Counters not misrepresented as lifetime totals | PASS — tile tooltip + Mining note: "Live counters — reset after a restart, firmware update or device reset" |
| Selected Healthy no longer looks Critical under a red accent | PASS — selection uses fixed `--nx-sem-info` (cyan) + inset ring + check indicator, never the accent |
| Workspace stronger without crowding | PASS — header health badge, aligned metric tiles, sparse-tab empty-state copy; stable height; no page-level horizontal overflow (DOM `scrollWidth` verified at 390 px) |
| NeuralAxe long release filenames work through the complete upload request | PASS — request-level tests assert POST endpoint + raw bytes; screenshots stage `…-601-www.bin` / `…-601-ota.bin` with the explicit Install control |
| Request-level tests verify transmitted filename and bytes | PASS — HttpTestingController asserts the actual `/api/system/OTAWWW` and `/api/system/OTA` requests and body |
| Dangerous images remain rejected | PASS — factory/merged/bootloader/partition/ota_data/`.cvs` rejected before suffix acceptance (tested + screenshot) |
| No automatic install | PASS — select stages only; no request fires until the explicit Install click (tested) |
| All frontend tests pass | PASS — **409/409** ×3 consecutive clean-`npm ci` runs (+30 over 379) |
| Firmware behavior unchanged | PASS — zero firmware sources; QEMU **83 Tests 0 Failures 0 Ignored** |
| No hardware or live-network access | PASS — source, synthetic intercepted fleet, tests, local dev server only |

## 2. OTA root cause (proven, not assumed)

There is **no backend filename requirement** and no current source-level rejection of the
export names. Proven end-to-end:

- **Classifier** (`update-file-check.ts`) already accepts `*-www.bin` and `*-ota.bin`
  (Phase 2H.1), with dangerous markers taking precedence.
- **Upload service** (`system.service.ts`) reads the file to an `ArrayBuffer` and POSTs it as
  a **raw `application/octet-stream` body** — no filename, FormData or multipart is transmitted.
- **Backend** (`http_server.c` `POST_OTA_update` / `POST_WWW_update`) writes `req->content_len`
  raw bytes straight to the partition and **hardcodes** `firmware_update_filename` internally
  for status display only; it never reads a filename/extension/Content-Disposition.

Because the same bytes upload fine as `www.bin` and the filename never crosses the wire, the
owner's earlier need to hand-rename could only have come from a web bundle predating the 2H.1
classifier (a stale deployment), not a code defect. Per Stage 6's own conditional the backend
does **not** require canonical names, so **no filename normalization and no multipart** was
introduced (multipart would break the raw-body backend contract). The two genuine gaps closed
this phase: (a) there was **no request-level upload test** (only method-spies), and (b) a
latent literal-name gate in the release-**download** display (`asset.name == 'www.bin'`).

## 3. Changed files (10, all frontend, zero firmware)

- `components/swarm/fleet-intel.ts` (+`.spec.ts`) — `fleetShares()`, `fleetShareSeverity`,
  `fleetShareSampleNote`, `formatCompactCount`/`formatExactCount`.
- `components/swarm/swarm.component.ts` (+`.spec.ts`) — `shares` getter, share/format/health-pill
  helpers; Stage 4 DOM-class + Stage 2/3/5 presentation tests.
- `components/swarm/swarm.component.html` — Fleet Shares tile, Mining-tab share breakdown,
  workspace header health badge, selection check indicator, sparse-tab empty-state copy.
- `components/swarm/fleet.styles.scss` — accent-independent selection treatment + title row + check.
- `components/update/update.component.ts` (+`.html`, +`.spec.ts`) — `releaseAssetOfType()`
  classifier-based download links; end-to-end install-request tests.
- `services/system.service.spec.ts` — request-level OTA upload tests.

## 4. Fleet share aggregation contract (Stages 2/3)

`fleetShares(devices)` → `{accepted, rejected, total, rejectRatePct|null, reportingDevices,
totalDevices, hasData}`. Only online devices reporting **both** counters as valid non-negative
numbers contribute; a missing/invalid counter excludes the device (never zero-filled);
offline/stale devices are never live contributors. `rejectRatePct` is null when `total===0`
(no NaN/∞/÷0). Coverage renders "from N of M reporting". Tile severity reuses the
≥100-total-or-≥3-rejects confidence gate and is **never Critical** — a low-confidence startup
sample never makes the tile look critical. Large counters use compact formatting with the exact
value in the tooltip/aria label. The Mining tab adds Accepted/Rejected/Total/Reject-rate plus a
"Not reported by this device" state for devices without counters.

## 5. Selected-device visual semantics (Stage 4)

Before: `.nx-fleet-nav-on { border-color: var(--primary-color) }` — a red accent painted a
Healthy selection with a full red border (read as Critical). After: fixed, accent-independent
`var(--nx-sem-info)` (cyan) border + inset ring + `pi-check-circle` indicator (not colour-only).
Critical keeps its fixed danger `border-left`, so a selected-Critical device shows **both**
(cyan selection + red health), and Attention/Offline/Unknown stay distinct. Keyboard focus
remains the accent outline, structurally separate from selection. DOM-class tests assert:
selected Healthy is not assigned Critical styling; selected unsupported-but-Healthy stays
Healthy; a selected Critical device still presents Critical; selection is class-driven and every
item stays focusable. Proven visually in `09-red-accent-semantics` (red accent, cyan selection)
and `05-selected-critical-health`.

## 6. End-to-end OTA compatibility (Stages 6/7)

Request-level tests (HttpTestingController) prove: the long NeuralAxe name passes selection and
stages; the explicit Install control appears; no request fires on selection; clicking Install
POSTs exactly one request to the correct endpoint (`/api/system/OTAWWW` for web, `/api/system/OTA`
for firmware) carrying the exact file bytes as `application/octet-stream`; the other endpoint gets
none. The release-download display now resolves assets via the classifier, so a release published
with the export names offers links (dangerous assets never shown). Screenshots `12`/`13` stage the
long www/ota names; `14` shows the factory image rejected with no Install.

## 7. Pipeline results (Phase B)

| Step | Result |
|---|---|
| Clean commit verified | `5b9b3abf`, `version.txt = v2.14.2-27-g5b9b3ab` (no `-dirty`) |
| `npm ci` | clean from lockfile |
| Frontend tests ×3 | **409 / 409 / 409** |
| Production web build | clean (0 errors; pre-existing bundle-size + `??` warnings only) |
| Full ESP-IDF build (v5.5.3 container) | clean — APP DESC OK, not dirty |
| App descriptor validation | `esp-miner.bin` app_desc == `v2.14.2-27-g5b9b3abf` |
| Matching firmware/web pair | **PAIR OK `v2.14.2-27-g5b9b3abf`** (app_desc == revision inside `www.bin`) |
| Merged factory image | 15 802 368 B |
| QEMU regression | **83 Tests 0 Failures 0 Ignored** |
| Release export | EXPORT OK — 4 artifacts, the normal NeuralAxe names |
| Manifest validation | VALIDATION OK (Gamma/601/BM1370) |
| Secrets / local-path scan | clean (firmware PEM strings are mbedTLS parser labels, not keys; no host paths) |
| Screenshot privacy scan | CLEAN (14 captures; gate enforced by the exporter) |

Release artifacts (out of git) in
`…/NeuralAxe Build Artifacts/fleet-share-ota-polish-v0.1.0-dev-board601/`:
`release/` (`NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin` 3 145 728 B,
`…-ota.bin` 1 650 368 B, `…-factory.bin` 15 802 368 B, `config-601.cvs`, manifest, SHA256SUMS),
`screenshots/` (14), `screenshot-privacy-scan.md`, `pipeline-evidence/`.

The exact exported `…-www.bin` and `…-ota.bin` are the names the updated frontend workflow
accepts at selection, stages, and installs through the real upload request — no manual copies or
renaming.

## 8. Unresolved / owner-gated

- None blocking. The pre-existing initial-bundle budget warning (2.38 MB vs a 2 MB warning /
  3 MB error budget) is unchanged by this work.
- Owner-gated: real-network validation against the physical mixed fleet, and confirming a
  release built with these exact filenames installs on hardware from the Update page.

## 9. Hardware and network access confirmation

No hardware or live network was accessed at any point: no miner IPs contacted, no scans, no
COM/USB, no esptool/bitaxetool against a device, no TCH dump. All evidence used source, mocked
`File` objects, an intercepted synthetic fleet on 10.0.0.x, tests, the local dev server, and the
ESP-IDF/QEMU build container.
