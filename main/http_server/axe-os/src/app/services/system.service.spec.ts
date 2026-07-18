import { TestBed } from '@angular/core/testing';

import { SystemApiService } from './system.service';
import { provideHttpClient, HttpEventType } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController, TestRequest } from '@angular/common/http/testing';

describe('SystemApiService', () => {
  let service: SystemApiService;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient()]
    });
    service = TestBed.inject(SystemApiService);
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });

  it('should keep existing system-info version fields intact (regression)', (done) => {
    service.getInfo().subscribe(info => {
      expect(info.version).toBeDefined();
      expect(info.axeOSVersion).toBeDefined();
      expect(info.idfVersion).toBeDefined();
      expect(info.boardVersion).toBeDefined();
      done();
    });
  });

  it('should expose additive NeuralAxe metadata without replacing upstream fields', (done) => {
    service.getInfo().subscribe(info => {
      expect(info.productName).toBe('NeuralAxe OS');
      expect(info.productVersion).toBe('0.1.0-dev');
      expect(info.buildChannel).toBe('development');
      expect(info.vendor).toBe('NeuralShield');
      expect(info.upstreamProject).toBe('ESP-Miner / AxeOS');
      expect(info.upstreamVersion).toBe('v2.14.2');
      expect(info.targetBoard).toBe('601');
      expect(info.targetDevice).toBe('Gamma');
      expect(info.targetAsic).toBe('BM1370');
      // additive: product identity must not overwrite the firmware version field
      expect(info.productVersion).not.toEqual(info.version);
      done();
    });
  });
});

/**
 * Request-level proof (2I.2 Stage 6) that the NeuralAxe release-export
 * filenames install end-to-end through the ACTUAL upload request — inspecting
 * the real HTTP request the service builds, not just spying the method.
 *
 * The upload transmits the raw file bytes as an application/octet-stream body;
 * no filename, FormData or multipart is sent. The backend (POST_OTA_update /
 * POST_WWW_update) writes those bytes straight to the partition and never reads
 * a filename, so the long export names need no canonicalization — the endpoint
 * (/api/system/OTA vs /OTAWWW) is the entire backend contract, and it is
 * preserved here.
 */
describe('SystemApiService OTA uploads (2I.2 Stage 6 — transmitted request)', () => {
  let service: SystemApiService;
  let httpMock: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(), provideHttpClientTesting()],
    });
    service = TestBed.inject(SystemApiService);
    httpMock = TestBed.inject(HttpTestingController);
  });

  afterEach(() => {
    httpMock.verify();
  });

  // The upload reads the file via FileReader.readAsArrayBuffer, which resolves
  // asynchronously in the browser; poll the testing backend for the POST it
  // then fires.
  async function waitForRequest(url: string, timeoutMs = 3000): Promise<TestRequest> {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
      const matches = httpMock.match(url);
      if (matches.length === 1) {
        return matches[0];
      }
      await new Promise(resolve => setTimeout(resolve, 5));
    }
    throw new Error(`No request to ${url} within ${timeoutMs}ms`);
  }

  it('web install POSTs the exact file bytes as octet-stream to /api/system/OTAWWW', async () => {
    const bytes = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const file = new File([bytes], 'NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin', { type: 'application/octet-stream' });

    let completed = false;
    service.performWWWOTAUpdate(file).subscribe({ complete: () => (completed = true) });

    const req = await waitForRequest('/api/system/OTAWWW');
    expect(req.request.method).toBe('POST');
    expect(req.request.headers.get('Content-Type')).toBe('application/octet-stream');
    // raw bytes — no multipart wrapper, no transmitted filename
    expect(req.request.body instanceof ArrayBuffer).toBeTrue();
    expect(Array.from(new Uint8Array(req.request.body as ArrayBuffer))).toEqual(Array.from(bytes));
    // endpoint separation: the web image never touches the firmware endpoint
    httpMock.expectNone('/api/system/OTA');

    req.flush('WWW update complete');
    await new Promise(resolve => setTimeout(resolve, 0));
    expect(completed).toBeTrue();
  });

  it('firmware install POSTs the exact file bytes to /api/system/OTA', async () => {
    const bytes = new Uint8Array([9, 8, 7, 6, 5, 4, 3, 2, 1, 0]);
    const file = new File([bytes], 'NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin', { type: 'application/octet-stream' });

    service.performOTAUpdate(file).subscribe();

    const req = await waitForRequest('/api/system/OTA');
    expect(req.request.method).toBe('POST');
    expect(Array.from(new Uint8Array(req.request.body as ArrayBuffer))).toEqual(Array.from(bytes));
    httpMock.expectNone('/api/system/OTAWWW');

    req.flush('Firmware update complete, rebooting now!');
  });

  it('surfaces the HttpClient event stream (progress + response) to the caller', async () => {
    const file = new File([new Uint8Array([1, 2, 3])], 'esp-miner.bin');
    const events: any[] = [];
    service.performOTAUpdate(file).subscribe(e => events.push(e));

    const req = await waitForRequest('/api/system/OTA');
    req.flush('ok');
    await new Promise(resolve => setTimeout(resolve, 0));
    expect(events.some(e => e.type === HttpEventType.Response)).toBeTrue();
  });
});
