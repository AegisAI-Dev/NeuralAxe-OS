import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Observable, of } from 'rxjs';
import { catchError, map, shareReplay } from 'rxjs/operators';

/**
 * Reports the web-interface revision that is actually INSTALLED on the www
 * partition right now, by fetching /version.txt — the very file the release
 * pipeline embeds in www.bin and the firmware reads at boot.
 *
 * Why this exists: the firmware's `axeOSVersion` field is a boot-time
 * snapshot (read once in SYSTEM_init_versions). A www-only OTA deliberately
 * does not restart the device, so after "install web interface" the API keeps
 * reporting the pre-update revision until the next restart, while the new UI
 * is already being served from flash. Fetching /version.txt live shows the
 * true installed artifact without touching firmware behavior.
 *
 * Emits `null` when the file cannot be fetched or looks implausible (e.g. the
 * dev server, where /version.txt does not exist) — callers must fall back to
 * the boot-reported value and must never fake equality.
 */
@Injectable({ providedIn: 'root' })
export class WebVersionService {

  /** Plausibility guard: a git-describe-style or plain tag revision. */
  private static readonly REVISION_PATTERN = /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:-\d+-g[0-9a-f]{7,12})?$/;

  public readonly installedWebVersion$: Observable<string | null>;

  constructor(private http: HttpClient) {
    // Cache-busting query: the device web server may serve cached content and
    // the whole point is to observe the CURRENT partition content.
    this.installedWebVersion$ = this.http.get(`/version.txt?t=${Date.now()}`, { responseType: 'text' }).pipe(
      map((raw) => {
        const value = (raw ?? '').trim();
        return WebVersionService.REVISION_PATTERN.test(value) ? value : null;
      }),
      catchError(() => of(null)),
      shareReplay({ refCount: false, bufferSize: 1 }),
    );
  }
}
