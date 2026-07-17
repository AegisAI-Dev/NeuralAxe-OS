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
