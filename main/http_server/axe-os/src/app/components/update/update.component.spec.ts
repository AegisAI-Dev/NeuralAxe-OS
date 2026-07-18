import { ComponentFixture, TestBed } from '@angular/core/testing';
import { firstValueFrom, of, throwError } from 'rxjs';

import { UpdateComponent } from './update.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { CheckboxModule } from 'primeng/checkbox';
import { TooltipModule } from 'primeng/tooltip';
import { HttpErrorResponse, provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController } from '@angular/common/http/testing';
import { provideToastr } from 'ngx-toastr';
import { GithubUpdateService, GithubRelease } from 'src/app/services/github-update.service';
import { SystemApiService } from 'src/app/services/system.service';

describe('UpdateComponent', () => {
  let component: UpdateComponent;
  let fixture: ComponentFixture<UpdateComponent>;
  let httpMock: HttpTestingController;
  let githubUpdateService: GithubUpdateService;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [UpdateComponent, ModalComponent],
      imports: [FileUploadModule, CheckboxModule, TooltipModule],
      providers: [provideHttpClient(), provideHttpClientTesting(), provideToastr()]
    });
    httpMock = TestBed.inject(HttpTestingController);
    githubUpdateService = TestBed.inject(GithubUpdateService);
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should never check for releases in the background (no request without user action)', () => {
    // Component is constructed and rendered; the release check must not have fired.
    httpMock.expectNone(GithubUpdateService.RELEASES_URL);
    expect(component.checkLatestRelease).toBeFalse();
  });

  it('should keep the manual www.bin and esp-miner.bin upload controls available', () => {
    const el: HTMLElement = fixture.nativeElement;
    const uploads = el.querySelectorAll('p-fileupload');
    expect(uploads.length).toBe(2);
    expect(el.textContent).toContain('Install Web Interface');
    expect(el.textContent).toContain('Install Firmware');
  });

  it('should identify the device and distinguish the image types', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('This Device');
    expect(text).toContain('Target Board');
    expect(text).toContain('www.bin');
    expect(text).toContain('esp-miner.bin');
    expect(text).toContain('factory');
    expect(text).toContain('replaces NeuralAxe OS');
  });

  it('should report clearly when the NeuralAxe repository has no release', async () => {
    spyOn(githubUpdateService, 'getReleases').and.returnValue(of([]));
    // Recreate so the constructor picks up the spied service.
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;

    const result = await firstValueFrom(component.latestRelease$);
    expect(result.state).toBe('none');

    component.checkLatestRelease = true;
    fixture.detectChanges();
    expect((fixture.nativeElement as HTMLElement).textContent)
      .toContain('No NeuralAxe OS release is available yet');
  });

  it('should report offline/rate-limit failures non-destructively', async () => {
    spyOn(githubUpdateService, 'getReleases').and.returnValue(throwError(() => new Error('offline')));
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;

    const result = await firstValueFrom(component.latestRelease$);
    expect(result.state).toBe('error');

    component.checkLatestRelease = true;
    fixture.detectChanges();
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('Could not reach the NeuralAxe OS release service');
    expect(text).toContain('Nothing was changed on this device');
  });

  it('should surface a NeuralAxe release when one exists', async () => {
    spyOn(githubUpdateService, 'getReleases').and.returnValue(of([
      { id: 1, tag_name: 'v0.1.0', name: 'NeuralAxe OS 0.1.0', prerelease: false } as any
    ]));
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;

    const result = await firstValueFrom(component.latestRelease$);
    expect(result.state).toBe('release');
    expect((result as any).release.tag_name).toBe('v0.1.0');
  });

  it('should distinguish rate-limit failures from offline failures', async () => {
    spyOn(githubUpdateService, 'getReleases').and.returnValue(
      throwError(() => new HttpErrorResponse({ status: 403, statusText: 'rate limit exceeded' }))
    );
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;

    const result = await firstValueFrom(component.latestRelease$);
    expect(result.state).toBe('error');
    expect((result as any).kind).toBe('rate-limit');

    component.checkLatestRelease = true;
    fixture.detectChanges();
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('rate-limited');
    expect(text).toContain('Nothing was changed on this device');
  });

  it('should report a network-level failure as offline', async () => {
    spyOn(githubUpdateService, 'getReleases').and.returnValue(
      throwError(() => new HttpErrorResponse({ status: 0, statusText: 'Unknown Error' }))
    );
    fixture = TestBed.createComponent(UpdateComponent);
    component = fixture.componentInstance;

    const result = await firstValueFrom(component.latestRelease$);
    expect(result.state).toBe('error');
    expect((result as any).kind).toBe('offline');

    component.checkLatestRelease = true;
    fixture.detectChanges();
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('offline');
    expect(text).toContain('Nothing was changed on this device');
  });

  describe('board compatibility', () => {
    const release = (overrides: Partial<GithubRelease>): GithubRelease => ({
      id: 1, tag_name: 'v0.1.0', name: 'NeuralAxe OS 0.1.0', prerelease: false, ...overrides
    });

    it('confirms releases that declare board 601', () => {
      expect(component.releaseCompatibility(release({
        assets: [{ name: 'esp-miner-factory-board-601.bin', browser_download_url: '' }]
      }))).toBe('confirmed');
      expect(component.releaseCompatibility(release({ body: 'Built for board-601 (Gamma).' })))
        .toBe('confirmed');
    });

    it('rejects releases that only claim other boards (e.g. board-702)', () => {
      expect(component.releaseCompatibility(release({ body: 'Supports board-702 only.' })))
        .toBe('mismatch');
      expect(component.releaseCompatibility(release({
        assets: [{ name: 'esp-miner-board-702.bin', browser_download_url: '' }]
      }))).toBe('mismatch');
    });

    it('marks releases without a declared board as unknown', () => {
      expect(component.releaseCompatibility(release({ body: 'General fixes.' }))).toBe('unknown');
    });

    it('withholds download links for a board-mismatched release', () => {
      spyOn(githubUpdateService, 'getReleases').and.returnValue(of([
        release({
          body: 'board-702 image',
          assets: [
            { name: 'esp-miner.bin', browser_download_url: 'https://example.invalid/esp-miner.bin' },
            { name: 'www.bin', browser_download_url: 'https://example.invalid/www.bin' }
          ]
        }) as any
      ]));
      fixture = TestBed.createComponent(UpdateComponent);
      component = fixture.componentInstance;

      component.checkLatestRelease = true;
      // of() emits synchronously, so two change-detection passes fully render
      // the release branch without waiting for zone stability (the shared
      // live-data service keeps periodic timers running, so whenStable never
      // resolves here).
      fixture.detectChanges();
      fixture.detectChanges();

      const el: HTMLElement = fixture.nativeElement;
      const text = el.textContent ?? '';
      expect(text).toContain('Not for this board');
      expect(text).toContain('not offered for this device');
      const downloadLinks = Array.from(el.querySelectorAll('a'))
        .filter(a => (a.getAttribute('href') ?? '').includes('example.invalid'));
      expect(downloadLinks.length).toBe(0);
    });

    it('shows the compatibility pill for a board-601 release', () => {
      spyOn(githubUpdateService, 'getReleases').and.returnValue(of([
        release({
          body: 'Built for board-601.',
          assets: [
            { name: 'esp-miner.bin', browser_download_url: 'https://example.invalid/esp-miner.bin' },
            { name: 'www.bin', browser_download_url: 'https://example.invalid/www.bin' }
          ]
        }) as any
      ]));
      fixture = TestBed.createComponent(UpdateComponent);
      component = fixture.componentInstance;

      component.checkLatestRelease = true;
      fixture.detectChanges();
      fixture.detectChanges();

      const el: HTMLElement = fixture.nativeElement;
      expect(el.textContent).toContain('Board 601 compatible');
      const downloadLinks = Array.from(el.querySelectorAll('a'))
        .filter(a => (a.getAttribute('href') ?? '').includes('example.invalid'));
      expect(downloadLinks.length).toBe(2);
    });
  });

  describe('staged upload flow (2H.1 filename compatibility)', () => {
    function selectWeb(name: string) {
      component.otaWWWUpdate({ files: [new File([''], name)] } as any);
      fixture.detectChanges();
    }
    function selectFirmware(name: string) {
      component.otaUpdate({ files: [new File([''], name)] } as any);
      fixture.detectChanges();
    }

    it('stages the NeuralAxe www name without uploading, then installs only on the explicit click', () => {
      const systemService = TestBed.inject(SystemApiService);
      const uploadSpy = spyOn(systemService, 'performWWWOTAUpdate').and.returnValue(of());

      selectWeb('NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin');

      expect(component.stagedWeb?.file.name).toBe('NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin');
      expect(uploadSpy).not.toHaveBeenCalled(); // selection alone never uploads
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Web interface update');

      component.installStagedWeb();
      expect(uploadSpy).toHaveBeenCalledTimes(1);
      expect(component.stagedWeb).toBeNull();
    });

    it('stages the NeuralAxe ota name for the firmware uploader and installs explicitly', () => {
      const systemService = TestBed.inject(SystemApiService);
      const uploadSpy = spyOn(systemService, 'performOTAUpdate').and.returnValue(of());

      selectFirmware('NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin');
      expect(component.stagedFirmware).not.toBeNull();
      expect(uploadSpy).not.toHaveBeenCalled();

      component.installStagedFirmware();
      expect(uploadSpy).toHaveBeenCalledTimes(1);
    });

    it('still accepts the legacy names on both uploaders', () => {
      selectWeb('www.bin');
      expect(component.stagedWeb?.check.accepted).toBeTrue();
      selectFirmware('esp-miner.bin');
      expect(component.stagedFirmware?.check.accepted).toBeTrue();
    });

    it('rejects a factory image from both uploaders with the USB-recovery reason and no upload', () => {
      const systemService = TestBed.inject(SystemApiService);
      const webSpy = spyOn(systemService, 'performWWWOTAUpdate');
      const fwSpy = spyOn(systemService, 'performOTAUpdate');

      selectWeb('NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin');
      expect(component.stagedWeb).toBeNull();
      expect(component.rejectedWeb?.check.detectedType).toBe('factory');

      selectFirmware('NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin');
      expect(component.stagedFirmware).toBeNull();
      expect(component.rejectedFirmware?.check.detectedType).toBe('factory');

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('not installable here');
      expect(text.toLowerCase()).toContain('usb-recovery');
      expect(webSpy).not.toHaveBeenCalled();
      expect(fwSpy).not.toHaveBeenCalled();
    });

    it('rejects wrong-type and arbitrary files with visible reasons', () => {
      selectWeb('esp-miner.bin'); // firmware image on the web uploader
      expect(component.rejectedWeb?.check.reason).toContain('Install Firmware');

      selectFirmware('arbitrary.bin');
      expect(component.rejectedFirmware?.check.detectedType).toBe('unknown');

      for (const name of ['esp-miner-merged.bin', 'bootloader.bin', 'partition-table.bin', 'ota_data_initial.bin']) {
        selectFirmware(name);
        expect(component.stagedFirmware).withContext(name).toBeNull();
        expect(component.rejectedFirmware?.check.accepted).withContext(name).toBeFalse();
      }
    });

    it('cancel clears a staged file without uploading', () => {
      const systemService = TestBed.inject(SystemApiService);
      const uploadSpy = spyOn(systemService, 'performWWWOTAUpdate');
      selectWeb('www.bin');
      expect(component.stagedWeb).not.toBeNull();

      component.cancelStagedWeb();
      expect(component.stagedWeb).toBeNull();
      expect(uploadSpy).not.toHaveBeenCalled();
    });
  });
});
