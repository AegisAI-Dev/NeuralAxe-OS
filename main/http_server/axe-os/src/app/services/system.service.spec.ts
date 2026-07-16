import { TestBed } from '@angular/core/testing';

import { SystemApiService } from './system.service';
import { provideHttpClient } from '@angular/common/http';

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
