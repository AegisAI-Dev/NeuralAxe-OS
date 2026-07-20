/**
 * Outbound privacy-contract tests (Phase 2L, Stage 11 — corrective).
 *
 * Inspects EVERY external request the service issues (live provider path) and
 * proves it is a bare GET to a documented public endpoint with no body, no
 * Authorization header, no query string, and no device/config value anywhere in
 * the URL — and that configured-pool matching only happens locally AFTER the
 * public data is received. Also proves the pure URL builders cannot embed
 * device/config values even from a hostile block hash.
 */

import { TestBed } from '@angular/core/testing';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';
import { of } from 'rxjs';
import { LocalStorageService } from 'src/app/local-storage.service';
import { LiveDataService } from '../live-data.service';
import { BlockIntelligenceService } from './block-intelligence.service';
import { MEMPOOL_PROVIDER } from './mempool-provider';
import { ESPLORA_PROVIDER } from './esplora-provider';
import { providerBlocksUrl, providerBlockDetailUrl } from './provider-request';
import { CACHE_KEY } from './block-refresh';
import {
  MEMPOOL_BLOCKS_MIXED,
  ESPLORA_BLOCKS,
  BLOCK_PROVIDER_REPORTED,
  PRIVACY_SENSITIVE,
  PRIVACY_FORBIDDEN_STRINGS,
} from './block-fixtures';

// Configured pool hosts must NEVER appear in any outbound URL/query either.
const CONFIG_VALUES = ['public-pool.io', 'solo.ckpool.org'];
const ALL_FORBIDDEN = [...PRIVACY_FORBIDDEN_STRINGS, ...CONFIG_VALUES];

const SENSITIVE_INFO = {
  stratumURL: PRIVACY_SENSITIVE.activeUrl, fallbackStratumURL: PRIVACY_SENSITIVE.fallbackUrl,
  stratumUser: PRIVACY_SENSITIVE.stratumUser, fallbackStratumUser: PRIVACY_SENSITIVE.fallbackStratumUser,
  ssid: PRIVACY_SENSITIVE.ssid, wifiPass: PRIVACY_SENSITIVE.wifiPass, hostname: PRIVACY_SENSITIVE.hostname,
  ipv4: PRIVACY_SENSITIVE.ip, hashRate: 1290, temp: 61, coreVoltage: 1150, frequency: 625,
};

function makeLiveService(): { service: BlockIntelligenceService; http: HttpTestingController } {
  localStorage.removeItem(CACHE_KEY);
  TestBed.configureTestingModule({
    imports: [HttpClientTestingModule],
    providers: [
      BlockIntelligenceService,
      LocalStorageService,
      { provide: LiveDataService, useValue: { info$: of(SENSITIVE_INFO), connected$: of(false) } },
    ],
  });
  const service = TestBed.inject(BlockIntelligenceService);
  (service as any).liveProviders = true; // exercise the real external HTTP path
  const http = TestBed.inject(HttpTestingController);
  return { service, http };
}

function assertCleanGet(req: { method: string; body: any; headers: { has: (h: string) => boolean }; urlWithParams: string }): void {
  expect(req.method).toBe('GET');
  expect(req.body).toBeNull();
  expect(req.headers.has('Authorization')).toBeFalse();
  expect(req.urlWithParams.includes('?')).toBeFalse();
  for (const secret of ALL_FORBIDDEN) {
    expect(req.urlWithParams).not.toContain(secret);
  }
}

describe('provider-request URL contract (pure)', () => {
  it('documents the mempool.space recent-blocks and block-detail URLs', () => {
    expect(providerBlocksUrl(MEMPOOL_PROVIDER)).toBe('https://mempool.space/api/v1/blocks');
    expect(providerBlockDetailUrl(MEMPOOL_PROVIDER, '00ffAB')).toBe('https://mempool.space/api/v1/block/00ffAB');
  });
  it('documents the Esplora recent-blocks and block-detail URLs', () => {
    expect(providerBlocksUrl(ESPLORA_PROVIDER)).toBe('https://blockstream.info/api/blocks');
    expect(providerBlockDetailUrl(ESPLORA_PROVIDER, '00ffAB')).toBe('https://blockstream.info/api/block/00ffAB');
  });
  it('carries no query string and cannot embed injected query params via the hash', () => {
    const url = providerBlockDetailUrl(MEMPOOL_PROVIDER, 'a/b?c=d&wallet=x');
    expect(url).not.toContain('?');
    expect(url).not.toContain('&');
    expect(url).not.toContain('=');
    expect(url).toContain('a%2Fb%3Fc%3Dd');
  });
});

describe('BlockIntelligenceService outbound requests (live path)', () => {
  afterEach(() => {
    TestBed.inject(HttpTestingController).verify();
    TestBed.resetTestingModule();
  });

  it('recent-blocks request is a clean GET to the documented endpoint; matching is local AFTER receipt', () => {
    const { service, http } = makeLiveService();
    const emissions: any[] = [];
    service.snapshot$.subscribe(s => emissions.push(s));

    const req = http.expectOne(providerBlocksUrl(MEMPOOL_PROVIDER));
    assertCleanGet(req.request);
    // No configured-pool match exists BEFORE the public data is received.
    expect(emissions[emissions.length - 1].blocks.length).toBe(0);

    req.flush(MEMPOOL_BLOCKS_MIXED);
    // ...matching happened locally only after the response.
    const last = emissions[emissions.length - 1];
    expect(last.blocks.some((b: any) => b.configuredMatch === 'active')).toBeTrue();
  });

  it('falls back to Esplora with a clean URL (no sensitive context appended)', () => {
    const { service, http } = makeLiveService();
    service.snapshot$.subscribe();

    http.expectOne(providerBlocksUrl(MEMPOOL_PROVIDER)).error(new ProgressEvent('error'));

    const fallback = http.expectOne(providerBlocksUrl(ESPLORA_PROVIDER));
    assertCleanGet(fallback.request);
    expect(fallback.request.urlWithParams).toBe(providerBlocksUrl(ESPLORA_PROVIDER));
    fallback.flush(ESPLORA_BLOCKS);
  });

  it('block-detail request is a clean GET to the documented endpoint', () => {
    const { service, http } = makeLiveService();
    service.fetchBlockDetail('0000feed').subscribe();
    const req = http.expectOne(providerBlockDetailUrl(MEMPOOL_PROVIDER, '0000feed'));
    assertCleanGet(req.request);
    req.flush(BLOCK_PROVIDER_REPORTED);
  });
});
