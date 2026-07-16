import { ComponentFixture, TestBed } from '@angular/core/testing';
import { firstValueFrom, of, throwError } from 'rxjs';

import { UpdateComponent } from './update.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { CheckboxModule } from 'primeng/checkbox';
import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController } from '@angular/common/http/testing';
import { provideToastr } from 'ngx-toastr';
import { GithubUpdateService } from 'src/app/services/github-update.service';

describe('UpdateComponent', () => {
  let component: UpdateComponent;
  let fixture: ComponentFixture<UpdateComponent>;
  let httpMock: HttpTestingController;
  let githubUpdateService: GithubUpdateService;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [UpdateComponent, ModalComponent],
      imports: [FileUploadModule, CheckboxModule],
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
    expect(el.textContent).toContain('Update AxeOS');
    expect(el.textContent).toContain('Update Firmware');
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
});
