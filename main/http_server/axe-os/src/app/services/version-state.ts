/**
 * Version-pair state derivation for the firmware / web-interface pairing.
 *
 * Three independent observations exist on a live device:
 *  - `firmware`: revision of the RUNNING firmware image (esp_app_desc). A
 *    firmware OTA always restarts, so this is always current.
 *  - `liveWeb`:  revision embedded in the www partition RIGHT NOW, read live
 *    from /version.txt (WebVersionService). Null when the file cannot be
 *    fetched or looks implausible.
 *  - `bootWeb`:  the firmware's boot-time snapshot of that same file
 *    (`axeOSVersion`). A www-only OTA does not restart the device, so this
 *    goes stale until the next restart.
 *
 * The rules here are deliberately conservative:
 *  - the installed web revision comes ONLY from the live read — the boot
 *    snapshot is never silently substituted for it;
 *  - equality is never faked: when the live read is unavailable the state is
 *    'unverified', not 'match';
 *  - a stale boot snapshot (live ≠ boot) is an informational restart-pending
 *    flag, independent of whether the installed pair actually matches.
 */

/** Where the displayed web revision came from. */
export type WebVersionSource = 'live' | 'boot-snapshot' | 'none';

export type VersionPairStatus =
  /** Live-verified: installed web matches the running firmware. */
  | 'match'
  /** Live-verified: installed web differs from the running firmware — genuine artifact mismatch. */
  | 'mismatch'
  /** Live /version.txt unavailable; only the boot snapshot is known — pairing cannot be verified. */
  | 'unverified'
  /** Not enough data (no firmware revision or no web observation at all). */
  | 'unknown';

export interface VersionState {
  /** Running firmware revision, or null when unreported. */
  firmware: string | null;
  /** Installed web revision — live /version.txt only; null when unavailable. */
  installedWeb: string | null;
  /** Firmware's boot-time snapshot of the web revision, or null. */
  bootWeb: string | null;
  /** Source of the best available web revision (for labeling). */
  webSource: WebVersionSource;
  /**
   * True when the live web revision differs from the boot snapshot: a
   * www-only update was installed after the last restart. Informational —
   * a restart refreshes the snapshot; nothing is wrong with the artifacts.
   */
  restartPending: boolean;
  status: VersionPairStatus;
}

function normalize(value: string | null | undefined): string | null {
  return typeof value === 'string' && value.trim() !== '' ? value.trim() : null;
}

export function deriveVersionState(
  firmware: string | null | undefined,
  bootWeb: string | null | undefined,
  liveWeb: string | null | undefined,
): VersionState {
  const fw = normalize(firmware);
  const boot = normalize(bootWeb);
  const live = normalize(liveWeb);

  const webSource: WebVersionSource = live ? 'live' : (boot ? 'boot-snapshot' : 'none');
  const restartPending = !!(live && boot && live !== boot);

  let status: VersionPairStatus;
  if (!fw || (!live && !boot)) {
    status = 'unknown';
  } else if (!live) {
    status = 'unverified';
  } else if (live === fw) {
    status = 'match';
  } else {
    status = 'mismatch';
  }

  return {
    firmware: fw,
    installedWeb: live,
    bootWeb: boot,
    webSource,
    restartPending,
    status,
  };
}

// ---------------------------------------------------------------------------
// Honest pair-status presentation (Phase 2J.1)
// ---------------------------------------------------------------------------

/**
 * The five distinguishable firmware/web pairing outcomes for the Update page.
 * The distinction that matters: a LIVE-verified result (live /version.txt was
 * read) vs a BOOT-only result (only the firmware's boot snapshot is known). A
 * missing live revision is NOT a mismatch.
 */
export type PairStatusState =
  | 'live-match'      // live web revision available and equals firmware
  | 'live-mismatch'   // live web revision available and differs from firmware
  | 'boot-match'      // live unavailable; boot web revision equals firmware
  | 'boot-mismatch'   // live unavailable; boot web revision differs from firmware
  | 'unknown';        // not enough revision data to judge either

export type PairSeverity = 'ok' | 'info' | 'warn' | 'danger';

export interface PairStatus {
  state: PairStatusState;
  /** Concise headline (e.g. "Live pair match" / "Boot pair match"). */
  primary: string;
  /** One-line qualifier (e.g. "Live verification unavailable"). */
  secondary: string;
  severity: PairSeverity;
  /** True only when the live web revision was available and compared. */
  liveVerified: boolean;
  /** True when the boot-time web revision was available and compared. */
  bootVerified: boolean;
}

/**
 * Honest, deterministic pair-status derivation. Never claims a live match from
 * boot-time data and never reports a missing live revision as a mismatch — it
 * reports exactly what could be verified. Revision strings are compared exactly
 * after whitespace trimming ('-dirty' and every other suffix are significant).
 */
export function derivePairStatus(
  firmware: string | null | undefined,
  bootWeb: string | null | undefined,
  liveWeb: string | null | undefined,
): PairStatus {
  const fw = normalize(firmware);
  const boot = normalize(bootWeb);
  const live = normalize(liveWeb);

  // Live verification available: the strongest, most current signal.
  if (fw && live) {
    return live === fw
      ? { state: 'live-match', primary: 'Live pair match',
          secondary: 'Firmware and installed web revision match',
          severity: 'ok', liveVerified: true, bootVerified: !!boot }
      : { state: 'live-mismatch', primary: 'Live pair mismatch',
          secondary: 'Installed web differs from the running firmware — update both from the same release',
          severity: 'danger', liveVerified: true, bootVerified: !!boot };
  }

  // Live unavailable: fall back to the firmware's boot-time snapshot, and say so.
  if (fw && boot) {
    return boot === fw
      ? { state: 'boot-match', primary: 'Boot pair match',
          secondary: 'Live verification unavailable',
          severity: 'ok', liveVerified: false, bootVerified: true }
      : { state: 'boot-mismatch', primary: 'Boot pair mismatch',
          secondary: 'Live verification unavailable — restart to refresh, or reflash the matching pair',
          severity: 'warn', liveVerified: false, bootVerified: true };
  }

  return { state: 'unknown', primary: 'Pair status unknown',
    secondary: 'Insufficient revision data', severity: 'info',
    liveVerified: false, bootVerified: false };
}
