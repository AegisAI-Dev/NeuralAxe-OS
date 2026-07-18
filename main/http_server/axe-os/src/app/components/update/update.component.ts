import { Component, ViewChild } from '@angular/core';
import { Observable, combineLatest, map, catchError, of } from 'rxjs';
import { HttpErrorResponse, HttpEventType } from '@angular/common/http';
import { ToastrService } from 'ngx-toastr';
import { FileUploadHandlerEvent, FileUpload } from 'primeng/fileupload';
import { GithubUpdateService, GithubRelease } from 'src/app/services/github-update.service';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { VersionState, deriveVersionState } from 'src/app/services/version-state';
import { ModalComponent } from '../modal/modal.component';
import { SystemInfo } from 'src/app/generated/models';
import { NEURALAXE } from 'src/app/neuralaxe';
import { UpdateFileCheck, checkUpdateFile, detectUpdateFileType } from './update-file-check';

/** A selected-and-validated file waiting for the explicit Install click. */
export interface StagedUpdateFile {
  file: File;
  check: UpdateFileCheck;
}

/** A rejected selection, kept only to show the filename and the reason. */
export interface RejectedUpdateFile {
  filename: string;
  check: UpdateFileCheck;
}

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

  /**
   * Version-pair state for the "This Device" card. The installed web revision
   * is read live from /version.txt (WebVersionService); the firmware's
   * boot-time snapshot is shown separately when stale, and equality between
   * firmware and web is never faked (services/version-state.ts).
   */
  public versionState$: Observable<VersionState>;

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
    private webVersionService: WebVersionService,
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

    this.versionState$ = combineLatest([this.info$, this.webVersionService.installedWebVersion$]).pipe(
      map(([info, liveWeb]) => deriveVersionState(info.version, info.axeOSVersion, liveWeb))
    );
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

  /**
   * The installable release asset of a given kind, matched by the SAME
   * classifier the uploader uses — so a NeuralAxe release whose assets carry
   * the export names (…-www.bin / …-ota.bin) offers download links just like a
   * legacy release named www.bin / esp-miner.bin. Dangerous assets (factory,
   * config, …) are never returned as installable downloads.
   */
  public releaseAssetOfType(release: GithubRelease, kind: 'www' | 'firmware'): { name: string; browser_download_url: string } | null {
    for (const asset of release.assets ?? []) {
      if (asset && typeof asset.name === 'string' && detectUpdateFileType(asset.name) === kind) {
        return { name: asset.name, browser_download_url: asset.browser_download_url };
      }
    }
    return null;
  }

  // ---- staged install flow (2H.1): select -> show detection -> explicit Install ----

  public stagedWeb: StagedUpdateFile | null = null;
  public stagedFirmware: StagedUpdateFile | null = null;
  public rejectedWeb: RejectedUpdateFile | null = null;
  public rejectedFirmware: RejectedUpdateFile | null = null;

  /**
   * File handed to the web-interface uploader. Nothing is uploaded here:
   * the name is classified (update-file-check.ts), rejected files show their
   * reason, accepted files wait for the explicit Install button.
   */
  otaWWWUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.websiteUpload.clear();
    const check = checkUpdateFile(file.name, 'www');
    if (!check.accepted) {
      this.stagedWeb = null;
      this.rejectedWeb = { filename: file.name, check };
      this.toastrService.error(check.reason, `Not a web-interface image (${check.typeLabel})`);
      return;
    }
    this.rejectedWeb = null;
    this.stagedWeb = { file, check };
  }

  /** File handed to the firmware uploader — same staged flow as the web side. */
  otaUpdate(event: FileUploadHandlerEvent) {
    const file = event.files[0];
    this.firmwareUpload.clear();
    const check = checkUpdateFile(file.name, 'firmware');
    if (!check.accepted) {
      this.stagedFirmware = null;
      this.rejectedFirmware = { filename: file.name, check };
      this.toastrService.error(check.reason, `Not a firmware image (${check.typeLabel})`);
      return;
    }
    this.rejectedFirmware = null;
    this.stagedFirmware = { file, check };
  }

  public cancelStagedWeb(): void {
    this.stagedWeb = null;
  }

  public cancelStagedFirmware(): void {
    this.stagedFirmware = null;
  }

  /** Explicit install of the staged firmware file — the only upload trigger. */
  public installStagedFirmware(): void {
    if (!this.stagedFirmware) {
      return;
    }
    const file = this.stagedFirmware.file;
    this.stagedFirmware = null;

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

  /** Explicit install of the staged web-interface file. */
  public installStagedWeb(): void {
    if (!this.stagedWeb) {
      return;
    }
    const file = this.stagedWeb.file;
    this.stagedWeb = null;

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
