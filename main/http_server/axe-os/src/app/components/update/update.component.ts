import { Component, ViewChild } from '@angular/core';
import { Observable, map, catchError, of } from 'rxjs';
import { HttpErrorResponse, HttpEventType } from '@angular/common/http';
import { ToastrService } from 'ngx-toastr';
import { FileUploadHandlerEvent, FileUpload } from 'primeng/fileupload';
import { GithubUpdateService, GithubRelease } from 'src/app/services/github-update.service';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { ModalComponent } from '../modal/modal.component';
import { SystemInfo } from 'src/app/generated/models';
import { NEURALAXE } from 'src/app/neuralaxe';

const IGNORE_RELEASE_CHECK_WARNING = 'IGNORE_RELEASE_CHECK_WARNING';

/**
 * Result of a manual NeuralAxe release check.
 * - 'release': a NeuralAxe release is available (never an upstream ESP-Miner release);
 * - 'none':    the NeuralAxe repository has no suitable release yet;
 * - 'error':   the check failed — non-destructive, retry later. `kind`
 *              distinguishes offline (no network), rate-limit (GitHub API
 *              limit) and unknown failures for clear user messaging.
 */
export type ReleaseCheckResult =
  | { state: 'release'; release: GithubRelease }
  | { state: 'none' }
  | { state: 'error'; kind: 'offline' | 'rate-limit' | 'unknown' };

/**
 * Declared board compatibility of a release, derived from explicit
 * `board-NNN` markers in the release tag, title, notes or asset names.
 * - 'confirmed': the release explicitly declares board 601;
 * - 'mismatch':  the release declares only other boards (e.g. board-702) —
 *                it must not be offered for this device;
 * - 'unknown':   the release declares no board — shown with a caution.
 */
export type ReleaseCompatibility = 'confirmed' | 'mismatch' | 'unknown';

@Component({
  selector: 'app-update',
  templateUrl: './update.component.html',
  styleUrls: ['./update.component.scss']
})
export class UpdateComponent {

  public firmwareUpdateProgress: number = 0;
  public websiteUpdateProgress: number = 0;

  public checkLatestRelease: boolean = false;
  public latestRelease$: Observable<ReleaseCheckResult>;

  public info$: Observable<SystemInfo>;

  public readonly neuralaxe = NEURALAXE;

  @ViewChild('firmwareUpload') firmwareUpload!: FileUpload;
  @ViewChild('websiteUpload') websiteUpload!: FileUpload;

  @ViewChild('privacyModal') privacyModal?: ModalComponent;
  @ViewChild('progressModal') progressModal?: ModalComponent;

  public updateTarget: string = '';
  public updateStatus: 'progress' | 'success' | 'error' = 'progress';
  public updateMessage: string = '';

  constructor(
    private systemService: SystemApiService,
    private liveDataService: LiveDataService,
    private toastrService: ToastrService,
    private loadingService: LoadingService,
    private githubUpdateService: GithubUpdateService,
    private localStorageService: LocalStorageService,
  ) {
    // Cold observable: no request is made until the user explicitly triggers the
    // release check (checkLatestRelease gates the subscribing template branch).
    this.latestRelease$ = this.githubUpdateService.getReleases().pipe(
      map((releases): ReleaseCheckResult => {
        const release = releases[0];
        return release ? { state: 'release', release } : { state: 'none' };
      }),
      catchError((err) => of<ReleaseCheckResult>({ state: 'error', kind: this.classifyCheckError(err) }))
    );

    this.info$ = this.liveDataService.info$;
  }

  /**
   * Map a failed release check to a user-facing error kind. GitHub reports
   * API rate limiting as HTTP 403/429; a network-level failure surfaces as
   * status 0. Everything else is reported as an unknown failure.
   */
  private classifyCheckError(err: unknown): 'offline' | 'rate-limit' | 'unknown' {
    if (err instanceof HttpErrorResponse) {
      if (err.status === 403 || err.status === 429) {
        return 'rate-limit';
      }
      if (err.status === 0) {
        return 'offline';
      }
    }
    return 'unknown';
  }

  /**
   * Board compatibility of a release, from explicit `board-NNN` markers in
   * its tag, title, notes and asset names. NeuralAxe OS currently supports
   * board 601 (Gamma / BM1370) only; a release that declares other boards
   * without 601 (e.g. a misleading board-702 claim) is rejected and its
   * downloads are not offered.
   */
  public releaseCompatibility(release: GithubRelease): ReleaseCompatibility {
    const haystack = [
      release.tag_name,
      release.name,
      release.body,
      ...(release.assets ?? []).map(asset => asset.name),
    ].filter((part): part is string => typeof part === 'string').join(' ').toLowerCase();

    const declaredBoards = new Set<string>();
    for (const match of haystack.matchAll(/board[-_ ]?(\d{3})/g)) {
      declaredBoards.add(match[1]);
    }

    if (declaredBoards.size === 0) {
      return 'unknown';
    }
    return declaredBoards.has(NEURALAXE.targetBoard) ? 'confirmed' : 'mismatch';
  }

  otaUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.firmwareUpload.clear(); // clear the file upload component

    if (file.name != 'esp-miner.bin') {
      this.toastrService.error('Incorrect file, looking for esp-miner.bin.');
      return;
    }

    this.updateTarget = 'Firmware';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performOTAUpdate(file)
      .subscribe({
        next: (event: any) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.firmwareUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.toastrService.success('Device restarted');
              this.updateStatus = 'success';
              this.updateMessage = 'Firmware updated. Device has been successfully restarted.';
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
          else if (event instanceof HttpErrorResponse)
          {
            this.updateStatus = 'error';
            this.updateMessage = event.error?.message || event.error || event.message || 'Unknown error occurred';
          }
        },
        error: (err) => {
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.firmwareUpdateProgress = 0;
        }
      });
  }

  otaWWWUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.websiteUpload.clear(); // clear the file upload component

    if (file.name != 'www.bin') {
      this.toastrService.error('Incorrect file, looking for www.bin.');
      return;
    }

    this.updateTarget = 'Web Interface';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performWWWOTAUpdate(file)
      .subscribe({
        next: (event: any) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.websiteUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.updateStatus = 'success';
              this.updateMessage = 'Web interface updated. The page will reload in a few seconds.';
              setTimeout(() => {
                window.location.reload();
              }, 2000);
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
          else if (event instanceof HttpErrorResponse)
          {
            this.updateStatus = 'error';
            this.updateMessage = event.error?.message || event.error || event.message || 'Unknown error occurred';
          }
        },
        error: (err) => {
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.websiteUpdateProgress = 0;
        }
      });
  }

  // https://gist.github.com/elfefe/ef08e583e276e7617cd316ba2382fc40
  public simpleMarkdownParser(markdown: string | undefined): string {
    if (!markdown) {
      return '';
    }
    const toHTML = markdown
      .replace(/^#{1,6}\s+(.+)$/gim, '<h4 class="mt-2">$1</h4>') // Headlines
      .replace(/\*\*(.+?)\*\*|__(.+?)__/gim, '<b>$1</b>') // Bold text
      .replace(/\*(.+?)\*|_(.+?)_/gim, '<i>$1</i>') // Italic text
      .replace(/\[(.*?)\]\((.*?)\s?(?:"(.*?)")?\)/gm, '<a href="$2" class="underline text-white" target="_blank">$1</a>') // Markdown links
      .replace(/(https?:\/\/github\.com\/.+\/(.+[^\s])+)/gim, (match, p1, p2) => `<a href="${p1}" target="_blank">${match.includes('/pull/') ? '#' : ''}${p2}</a>`) // Regular links
      .replace(/@([^\s]+)/gim, ' <a href="https://github.com/$1" target="_blank">@$1</a> ') // Username links
      .replace(/^\s*[-+*]\s?(.+)$/gim, '<li>$1</li>') // Unordered list
      .replace(/`([^`]+)`/gim, '<code class="surface-100">$1</code>') // Code
      .replace(/\r\n\r\n/gim, '<br>'); // Breaks

    return toHTML.trim();
  }

  public handleReleaseCheck(): void {
    if (this.localStorageService.getBool(IGNORE_RELEASE_CHECK_WARNING)) {
      this.checkLatestRelease = true;
    } else {
      if (this.privacyModal) {
        this.privacyModal.isVisible = true;
      }
    }
  }

  public continueReleaseCheck(skipWarning: boolean): void {
    this.checkLatestRelease = true;
    if (this.privacyModal) {
      this.privacyModal.isVisible = false;
    }

    if (!skipWarning) {
      return;
    }

    this.localStorageService.setBool(IGNORE_RELEASE_CHECK_WARNING, true);
  }
}
