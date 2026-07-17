import { TestBed } from '@angular/core/testing';
import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController } from '@angular/common/http/testing';
import { firstValueFrom } from 'rxjs';

import { WebVersionService } from './web-version.service';

describe('WebVersionService', () => {
  let httpMock: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(), provideHttpClientTesting()],
    });
    httpMock = TestBed.inject(HttpTestingController);
  });

  afterEach(() => httpMock.verify());

  function expectVersionRequest() {
    return httpMock.expectOne((req) => req.url.startsWith('/version.txt'));
  }

  it('reports the installed web revision from /version.txt (the artifact source)', async () => {
    const service = TestBed.inject(WebVersionService);
    const promise = firstValueFrom(service.installedWebVersion$);
    expectVersionRequest().flush('v2.14.2-13-g388287da\n');
    expect(await promise).toBe('v2.14.2-13-g388287da');
  });

  it('accepts a plain tagged release revision', async () => {
    const service = TestBed.inject(WebVersionService);
    const promise = firstValueFrom(service.installedWebVersion$);
    expectVersionRequest().flush('v2.14.2');
    expect(await promise).toBe('v2.14.2');
  });

  it('returns null when /version.txt is unavailable (dev server) instead of guessing', async () => {
    const service = TestBed.inject(WebVersionService);
    const promise = firstValueFrom(service.installedWebVersion$);
    expectVersionRequest().flush('not found', { status: 404, statusText: 'Not Found' });
    expect(await promise).toBeNull();
  });

  it('returns null for implausible content (e.g. an HTML error page)', async () => {
    const service = TestBed.inject(WebVersionService);
    const promise = firstValueFrom(service.installedWebVersion$);
    expectVersionRequest().flush('<!doctype html><html>...</html>');
    expect(await promise).toBeNull();
  });
});
