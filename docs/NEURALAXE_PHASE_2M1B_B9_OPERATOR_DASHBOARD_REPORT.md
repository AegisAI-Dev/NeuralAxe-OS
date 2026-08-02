# NeuralAxe OS — Phase 2M.1B, Gate B9

## Timed Pool Session Operator Dashboard and Safe Command UX Report

**Product:** NeuralAxe OS 0.1.0-dev · **Board:** Bitaxe Gamma / 601 / BM1370 (ESP32-S3)
**Branch:** `neuralaxe-v0.1-timed-pool-sessions-ui`
**Commit:** `a7793af` — `feat: add timed-session operator dashboard and safe command UX` (**v2.14.2-63-ga7793af**, parent `98c6635` = the committed Gate B8 report; descends from `2021622`). 28 files, 6,458 insertions / 4 deletions.
**Scope:** the first operator-facing surface for the committed Gate B8 control API — feature-availability detection, a bounded create form, sanitized live status, a backend-authoritative progress timeline, queued-command feedback, Restore Now, safe terminal acknowledgement, trusted-time and heartbeat visibility, conflict and recovery presentation, and the command-correlation contract that governs what the page is allowed to claim. Frontend-only apart from one approved OpenAPI enum correction. Keep-current-password only: the dashboard has no password control and sends no password-like field.

Architectural sources of truth: the committed Phase 2M.1A report and the B1–B8 gate reports. Gate B9 was accepted after one owner review round; both blockers that round raised are part of the committed implementation and are documented below.

---

## 1. Verdict

**Gate B9 PASS (Phase A + one owner review round + Phase B committed-state verification green).**

| Check | Result |
|---|---|
| Committed files | exactly the 28 approved files: **21 added + 7 modified**; every path inside the approved boundary |
| Ancestry | `a7793af` → `98c6635` (B8 report) → `2021622` (B8 implementation); verified read-only |
| Firmware source | **untouched** — the commit contains no `.c`, `.h`, `CMakeLists`, `Kconfig`, `sdkconfig` or partition path |
| Non-frontend file | exactly one: `main/http_server/openapi.yaml` (+4 lines, approved enum correction) |
| Frontend `npm run test:gate` | **1270 / 1270 executed (complete), 0 failed, exit 0** (1052 baseline → **+218**) |
| Production web build | exit 0; 19 `NG8102` warnings, **all pre-existing**, none from B9 |
| `www` SPIFFS payload | **910,088 B of 3,145,728 B — 28.9%** |
| OpenAPI generation | deterministic (identical tree hash across two runs); 17 models / 3 services; nothing generated is committable |
| Fresh `test-ci` build (ESP-IDF v5.5.3) | exit 0 |
| QEMU | **693 Tests, 0 Failures, 0 Ignored**, 0 explicit FAIL lines — unchanged, as expected |
| Posture A (all three flags off) | build exit 0; 1,658,416 B; **0** timed-session API symbols |
| Posture B (`NX_TIMED_SESSIONS` only) | build exit 0; 1,691,040 B; 1 symbol (`send_conflict`), **0 routes** |
| Posture C (+ `_EXECUTION`) | build exit 0; 1,712,784 B; 1 symbol, **0 routes** |
| Posture D (+ `_API`) | build exit 0; 1,736,320 B; 3 symbols incl. `register_routes`, `boot_init` |
| Conflict-code parity | firmware **15** = schema **15** = UI **15**, no gap in either direction |
| Secret / privacy / local-path scan | committed diff clean |
| Tree after verification | clean; `test-ci/CMakeLists.txt` byte-exact stub (22 B); no `report.xml`; no release artifacts |
| Access | no hardware, COM/USB, flashing, physical-device NVS, real NTP, live DNS, real pools/accounts/wallets/credentials, OTA or device restart; all builds, QEMU and audits ran offline |

- **Frontend-only runtime behaviour.** All four firmware postures are **byte-identical** to the pre-B9 measurements, proving the schema edit does not reach the firmware binary.
- **No automatic session creation of any kind.** Every command is operator-initiated behind an explicit confirmation.
- The **unauthenticated LAN posture** remains the dominant residual and is precisely why the correlation contract is framed as device posture rather than ownership.

---

## 2. What the dashboard is allowed to claim

The single hardest problem in this gate is not layout — it is epistemic. The page submits a command and receives HTTP 202, which means *queued* and nothing more. To say anything further it must read the device's own status. But a status snapshot carries the result of whatever the device processed **last**, which may be a command from a previous page instance or from another client on the same unauthenticated network. Read naively, that produces a completion claim for a command the device has not looked at.

Two defects of exactly this shape were found and fixed during Phase A, and a third contract gap was closed in the owner review round.

**Defect 1 — a command could resolve from a result the device produced earlier.** The first implementation resolved on `lastCommandResult` alone. On a device already mining with a leftover `ACCEPTED`, pressing Restore Now displayed a success banner before the device had looked at the request.

**Defect 2 — `createOffered` ignored the capability flags.** A posture-B or posture-C build answers the status route and reports `executionEnabled: false`. The page would still have offered the create form, promising something that firmware is structurally unable to do.

**Contract gap — a request number is not proof.** `client_request_id` is client-chosen, unauthenticated, 32 bits, and RAM-only. A reloaded page or a second client can legitimately present the same number. Correlating on the id echo alone is better than nothing, but it is **not** ownership, and no wording may be built on it that says otherwise.

---

## 3. The command-correlation contract

`timed-session-correlation.ts` is pure: no clock, no IO, no storage. Before every command the page captures a **baseline** of what the device was reporting — `statusSequence`, `durableState`, `runtimeState`, `executionState`, `sessionPresent`, `terminalResultPending`, `restoreRequired`, `lastCommand`, `lastCommandResult`, `lastClientRequestId`. All three commands refuse to submit when no status has been read, because a command with no baseline could never be correlated.

A later status may resolve that command only when **all four** checks pass, evaluated in order, with the first failure recorded in `blockedBy`:

| # | Check | Blocked value |
|---|---|---|
| 1 | echoed `lastClientRequestId` equals the number sent (and is non-zero) | `id` |
| 2 | echoed `lastCommand` equals `COMMAND_TOKEN[kind]` | `kind` |
| 3 | `statusSequence` is **strictly greater** than the baseline | `sequence` |
| 4 | the command-specific authoritative transition has happened | `evidence` |

Check 4 carries the weight. `lastCommandResult` is never sufficient on its own, because it is exactly the field a stale or foreign command can populate.

**CREATE** — a durable session must now exist (`sessionPresent`, `durableState !== 'IDLE'`) **and** at least one of durable / runtime / execution state must have moved off the captured free posture.

**RESTORE** — `durableState` must be in `{RESTORE_DUE, APPLYING_RESTORE, RESTARTING_FOR_RESTORE, VERIFYING_RESTORE, COMPLETE, RESTORE_FAILED}` **and must differ from the baseline**. A device already sitting in the same restore state proves nothing.

**ACKNOWLEDGE** — the baseline must have had `terminalResultPending === true` and it must now be false. The terminal card is never cleared optimistically.

Anything short of all four leaves the command **queued**, which is always the truthful answer: the device has not reported on it yet.

A **refusal** is reported on the first three checks alone. Claiming that nothing happened cannot mislead an operator into believing something did, so it does not need the fourth.

### 3.1 Attribution is never ownership

The `succeeded` phase does not exist. The strongest positive outcome is **`posture-reached`**, titled *"The device now reports the requested posture"*, whose body states that request numbers are best-effort correlation on a network with no authentication, so this is "the device reporting its state — not a receipt proving this page's request was the one carried out."

`CommandResolution.attribution` has exactly two values, `device-posture` and `none`. **There is deliberately no value meaning "owned"**, so no caller can accidentally upgrade the claim.

A separate **`device-refused`** phase was added so that a device rejection (*"The device did not accept the request"*) is never confused with a lost request (*"The request did not reach the device"*). The two need different operator actions and must not share copy.

### 3.2 Request-number allocation

`timed-session-request-id.ts` allocates strictly monotonically over `1 … MAX_CLIENT_REQUEST_ID` (`0xFFFFFFFF`, the committed B8 parser bound; 0 stays reserved for "no id"). It is **non-wrapping**: `next()` returns `null` once the range is spent and **never returns to 1**, because a reused number re-creates the very confusion the number exists to prevent.

The component treats `null` as fail-closed — it sets the `request-ids-exhausted` phase, clears `submitting`, closes the dialog and **sends no HTTP request at all**. Recovery is an explicit page reload, never a silent reuse. The number is derived solely from a counter: no clock, host, account or device value feeds it, and two fresh allocators are proven indistinguishable.

Page reload still resets RAM, which is exactly why the sequence-baseline and command-specific-evidence checks remain mandatory rather than optional.

---

## 4. Capability detection

Answering the status route is not the same as being able to run a session. `capable` requires `apiEnabled && executionEnabled && runtimeInitialized`, each read strictly so that a **missing** flag counts as not capable rather than "probably fine". `createOffered` is gated on it.

When the form is withheld the page states which posture caused it, rather than silently dropping the form:

| Posture | Operator sentence |
|---|---|
| `apiEnabled: false` | the build answers the status route but has the control API turned off |
| `executionEnabled: false` | the build reports the runtime but not the execution layer, so it can describe a session and never carry one out |
| `runtimeInitialized: false` | the runtime has not finished starting; this normally clears shortly after boot |
| session already present | only one timed session runs at a time |
| terminal result retained | acknowledge it first |
| operator recovery required | the device needs recovery before it accepts anything new |

Capability gates only what the **operator is offered**; it never changes what the device is reported to be doing. An incapable device that reports an active session is still described honestly, and Restore Now is still offered.

---

## 5. OpenAPI conflict-enum correction

The Phase A audit found a real committed API-contract defect: `pool_operation_http_code_str()` in `components/pool_operation_coordinator/pool_operation_http_policy.c` has a defensive `default:` branch returning `"OPERATION_CODE_UNKNOWN"`, but that value was absent from the `OperationConflict.code` enum in `main/http_server/openapi.yaml`. The firmware could emit a conflict code its own published schema rejects.

Fixed in this gate, schema-only:

- `OPERATION_CODE_UNKNOWN` added to the exact enum, all fourteen existing values preserved, CRLF preserved, with a comment naming the source branch.
- **No firmware conflict semantics changed.** All four postures rebuilt byte-identical.
- Client regenerated: 17 models / 3 services (unchanged counts, 41 files). The generated union now ends `… | 'OPERATION_INVALID_REQUEST' | 'OPERATION_CODE_UNKNOWN'`.
- A test assigns the literal to `OperationConflict['code']` **without a cast**, so removing it from the schema again breaks compilation.
- Generation is deterministic — two consecutive runs produce an identical tree hash (`42a14897…`) — and `src/app/generated` remains gitignored, so nothing generated is committable.
- The bounded mapper gained explicit safe wording for the code, and the generic fallback is retained for genuinely unknown or malformed strings.

Three-way parity now holds exactly: **firmware 15 = schema 15 = UI 15**, verified from the committed tree with no gap in either direction.

---

## 6. Privacy and the shipped surface

- **No password control exists.** The rendered DOM is enumerated in test: zero `input[type="password"]`, and every control id is in an allow-list of target, duration and acknowledgement ids. No source control and no session-identifier control exist either — the committed request schema has none of them.
- **Target host and account live only in a plain in-memory object**, cleared on accepted submission and on destroy. Planted markers are proven absent from `localStorage`, `sessionStorage`, the URL, the console, toasts, the command banner, the rendered DOM after reset, and `Storage.prototype.setItem` calls. The only storage-API references in the whole feature are inside the privacy spec that performs that audit, and two doc comments stating the rule.
- **The view model has no field that could hold an identity**, so a hostile status payload carrying extra keys cannot leak — there is nowhere to put it.
- **Custom-certificate TLS is never offered**: the control renders exactly two options, neither matching `/custom/i`. The page explains the exclusion in prose, which is not the same as offering it.
- **Two HTTP call sites exist in the entire feature** — one `GET` and one `postAccepted` helper — both against generated `PATH` constants. No absolute URL, no `fetch`, no WebSocket, no `sendBeacon`. Route parity is exact across firmware `.uri`, generated `PATH` and spec constants.
- **The development mock switch is dead on a device**, proven in the shipped bundle: `live(){return true}`, `isMocked(){return!this.live()}`.
- Form bounds match the firmware exactly — duration 900–86,400 s, host ≤ 79, account ≤ 127, port 1–65,535 — and surrounding whitespace is **reported, never trimmed**, because it is part of the pool identity.

---

## 7. Polling, accessibility and layout

Cadence is bounded and explicit: 2 s while a session or command is active, 10 s idle, 30 s while the tab is hidden, and a stepped backoff of `4 s · 2^(n−1)` capped at 60 s. A `statusInFlight` single-flight guard means only one status read is ever in the air, and everything unsubscribes on destroy.

Automatic polling **stops** only on the two answers that cannot change by themselves — 404 (this build has no such route) and 401 (origin denied) — and an explicit "Check again" is the only way out.

**No command is ever retried automatically.** A lost response is surfaced, never resent; a double click produces exactly one request; 400 and 409 never resubmit. Only idempotent status reads back off and retry.

All eleven form controls are label-bound, decorative icons are `aria-hidden`, state changes are announced once through a polite live region, the recovery banner is `role="alert"`, severity is never the only indicator, and the layout collapses cleanly at the mobile breakpoint with `prefers-reduced-motion` respected. Feature styles live in a global partial imported from `src/styles.scss`, so the 2 KB / 4 KB `anyComponentStyle` budget is trivially satisfied — the component declares no `styleUrls`.

Nothing in this gate touches Weather-Aware Tuning; the routing spec asserts no weather route or menu entry exists.

---

## 8. Test suite (212 declared cases → 1270 executed)

Frontend total rose from the committed 1052 baseline to **1270** (+218; parameterised describes expand the 212 declared `it` blocks).

**Correlation, 22** — baseline capture and its identity-free shape · each of the four checks failing in isolation · a stale retained result with an old sequence · a matching id with the wrong kind · id, kind and sequence matching with no state change · CREATE / RESTORE / ACKNOWLEDGE evidence in both directions · a restore posture that never moved · refusal on the first three checks · attribution never upgraded to ownership.

**Request ids, 8** — monotonic allocation · strictly increasing over the full range · issuance tracking · the final id usable exactly once · fail-closed refusal afterwards, never back to 1 · a degenerate range issuing nothing · the committed backend maximum · two allocators indistinguishable.

**Component, 46** — availability transitions and the stop/re-check path · cadence, single-flight and teardown · create review, double-click suppression, 202-is-not-started · the full correlation chain end to end · device refusal distinguished from a lost request · request-number exhaustion sending no HTTP · Restore Now and acknowledgement confirmation gates · capability postures · status presentation, screen-reader announcement and disabled-submit reasons.

**Mapper, 47** — availability, durable / runtime / execution tokens, deadline, trusted time, heartbeat, conflicts, commands, the whole status view, capability derivation, all fifteen firmware conflict codes, and the `OPERATION_CODE_UNKNOWN` schema contract.

**Service, 23** · **Form, 21** · **Timeline, 16** · **Privacy, 12** · **Polling, 9** · **Routing, 8**.

No frontend test performs outbound networking.

---

## 9. Committed file manifest

**Added, 21 files** — 11 shipped (3,295 lines) and 10 spec (3,144 lines), all under `main/http_server/axe-os/src/app/`:

| Shipped | Spec |
|---|---|
| `components/timed-session/timed-session.models.ts` | `timed-session-correlation.spec.ts` |
| `components/timed-session/timed-session-mapper.ts` | `timed-session-form.spec.ts` |
| `components/timed-session/timed-session-form.ts` | `timed-session-mapper.spec.ts` |
| `components/timed-session/timed-session-timeline.ts` | `timed-session-polling.spec.ts` |
| `components/timed-session/timed-session-polling.ts` | `timed-session-privacy.spec.ts` |
| `components/timed-session/timed-session-correlation.ts` | `timed-session-request-id.spec.ts` |
| `components/timed-session/timed-session-request-id.ts` | `timed-session-routing.spec.ts` |
| `components/timed-session/timed-session.component.ts` | `timed-session-timeline.spec.ts` |
| `components/timed-session/timed-session.component.html` | `timed-session.component.spec.ts` |
| `components/timed-session/timed-session.styles.scss` | `services/timed-session.service.spec.ts` |
| `services/timed-session.service.ts` | |

**Modified, 7 files (+19 / −4):** `main/http_server/openapi.yaml` (+4) · `axe-os/src/app/app-routing.module.ts` (+7 / −1) · `app.module.ts` (+2) · `layout/app.menu.component.ts` (+1) · `layout/app.menu.component.spec.ts` (+2 / −1) · `layout/app.sidebar.component.spec.ts` (+2 / −2) · `src/styles.scss` (+1).

`app-routing.module.ts` also changed `const routes` to `export const routes` so the routing spec can assert the real table instead of reaching into `ɵinj.providers`; no routing behaviour changed. The two edited layout specs are existing navigation-contract tests that had to grow because the navigation legitimately gained a sixteenth destination.

---

## 10. Residuals and limits

- **The device HTTP surface is unauthenticated on the LAN.** Anyone who can reach it can create, restore and acknowledge a timed session. B9 adds no authentication and does not claim to. This is the reason the correlation contract stops at device posture.
- **A request number is not identity.** Even with all four checks passing, the honest claim is about what the device reports. This is a property of the transport, not a gap in the page.
- **A command can remain queued indefinitely** when the evidence never appears — for example if a session completes and clears between polls. This is deliberate: the alternative is a time-based guess, and nothing in this dashboard is inferred from elapsed browser time. The status cards below the banner always show the truth.
- **~4.3 KB of minified mock-posture data ships as unreachable code.** It is provably dead in production and contains no real values, but it is dead weight; moving it behind a lazy development-only chunk is a candidate for a later polish gate.
- **The initial-bundle budget warning is pre-existing.** Measured by unwiring B9 and rebuilding: 2.72 MB without, 2.80 MB with, against 2.10 MB warn / 3.00 MB error. B9's share is +74,519 B of `main.js` and +5,129 B of `styles.css`. The error threshold is never reached, and the `www` partition that actually governs flashability sits at 28.9%.
- **Nineteen `NG8102` template warnings** remain in `CommandDeckComponent` and `EditComponent`. They are pre-existing and untouched by this gate.
- **No real-hardware pilot is authorized by this report.** The dashboard has never contacted a device; every posture it renders came from synthetic status fixtures or `HttpTestingController`.
- The Gate B1–B8 limits stand in full, including the open trusted-time source decision and the B7 probabilistic binding guarantee.

---

## 11. Phase B verification appendix

All commands ran offline against the committed tree (`a7793af`, clean status), Docker `espressif/idf:v5.5.3` for builds and the `nx-qemu-action` image for QEMU only:

1. `git show --stat / --name-status HEAD` — 28 files, 21 A + 7 M, 6,458 / 4; ancestry to `98c6635` and `2021622` verified. Read-only git throughout; the owner performed the commit.
2. Committed-diff scans — no firmware source, no build config, no generated or artifact path; secret, local-path, storage, console and outbound-network scans clean.
3. Conflict-code parity computed from the committed firmware, schema and mapper — 15 / 15 / 15, no gap.
4. `npm run test:gate` — **1270 / 1270, 0 failed, exit 0**.
5. `npm run build` — exit 0; 19 pre-existing `NG8102` warnings; `www` payload 910,088 B (28.9%); shipped bundle verified to carry `live(){return true}`, `OPERATION_CODE_UNKNOWN` and the posture wording.
6. `npm run generate:api` twice — identical tree hash; 17 models / 3 services; `git status` on `src/app/generated` empty.
7. Postures A / B / C / D from pristine sdkconfig copies — exit 0 each; sizes and symbol counts as tabled in §1; tracked `sdkconfig.defaults` untouched.
8. Fresh `test-ci` build — exit 0; full QEMU — **693 Tests, 0 Failures, 0 Ignored**, 0 explicit FAIL lines.
9. `test-ci/CMakeLists.txt` restored to the byte-exact 22-byte stub; all `build_posture_*`, `test-ci/build`, temporary scripts and `report.xml` removed; tree clean after verification.

**Next gate:** B10 has not been started and is not authorized by this report.
