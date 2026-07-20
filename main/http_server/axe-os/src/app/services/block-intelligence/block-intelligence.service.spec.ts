import { TestBed } from '@angular/core/testing';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { of } from 'rxjs';
import { take } from 'rxjs/operators';
import { LocalStorageService } from 'src/app/local-storage.service';
import { LiveDataService } from '../live-data.service';
import { BlockIntelligenceService } from './block-intelligence.service';
import { BlockIntelligenceSnapshot } from './block-intelligence.model';
import { CACHE_KEY } from './block-refresh';
import { PRIVACY_SENSITIVE, PRIVACY_FORBIDDEN_STRINGS } from './block-fixtures';

function liveStub(info: any) {
  return { info$: of(info), connected$: of(false) };
}

const STANDARD_INFO = { stratumURL: 'public-pool.io', fallbackStratumURL: 'solo.ckpool.org' };

function makeService(info: any): BlockIntelligenceService {
  localStorage.removeItem(CACHE_KEY);
  TestBed.configureTestingModule({
    imports: [HttpClientTestingModule],
    providers: [
      BlockIntelligenceService,
      LocalStorageService,
      { provide: LiveDataService, useValue: liveStub(info) },
    ],
  });
  return TestBed.inject(BlockIntelligenceService);
}

describe('BlockIntelligenceService (dev fixtures, no network)', () => {
  afterEach(() => TestBed.resetTestingModule());

  it('emits a live snapshot of normalized blocks', () => {
    const service = makeService(STANDARD_INFO);
    let snap: BlockIntelligenceSnapshot | undefined;
    service.snapshot$.subscribe(s => (snap = s));
    expect(snap!.blocks.length).toBeGreaterThan(0);
    expect(snap!.freshness.status).toBe('live');
    expect(snap!.freshness.provider).toBe('mempool.space');
  });

  it('applies configured-pool matching locally (active pool)', () => {
    const service = makeService(STANDARD_INFO);
    let snap: BlockIntelligenceSnapshot | undefined;
    service.snapshot$.subscribe(s => (snap = s));
    const hasActive = snap!.blocks.some(b => b.configuredMatch === 'active' || b.configuredMatch === 'both');
    expect(hasActive).toBeTrue();
  });

  it('captures a session start height on the first data', () => {
    const service = makeService(STANDARD_INFO);
    service.snapshot$.pipe(take(1)).subscribe();
    expect(service.sessionStartHeight).not.toBeNull();
  });

  it('persists a bounded cache after a successful refresh', () => {
    const service = makeService(STANDARD_INFO);
    service.snapshot$.subscribe();
    expect(localStorage.getItem(CACHE_KEY)).toBeTruthy();
  });

  it('fetchBlockDetail resolves a normalized detail in dev mode', () => {
    const service = makeService(STANDARD_INFO);
    service.snapshot$.subscribe();
    let detail: any;
    service.fetchBlockDetail('active-match').subscribe(d => (detail = d));
    expect(detail).toBeTruthy();
    expect(detail.merkleRoot).toBeTruthy();
    expect(detail.configuredMatch).toBe('active');
  });

  it('manual refresh keeps the snapshot usable', () => {
    const service = makeService(STANDARD_INFO);
    let snap: BlockIntelligenceSnapshot | undefined;
    service.snapshot$.subscribe(s => (snap = s));
    service.refresh();
    expect(snap!.blocks.length).toBeGreaterThan(0);
  });

  it('never emits any sensitive device value in the snapshot', () => {
    const privacyInfo = {
      stratumURL: PRIVACY_SENSITIVE.activeUrl,
      fallbackStratumURL: PRIVACY_SENSITIVE.fallbackUrl,
      stratumUser: PRIVACY_SENSITIVE.stratumUser,
      fallbackStratumUser: PRIVACY_SENSITIVE.fallbackStratumUser,
      ssid: PRIVACY_SENSITIVE.ssid,
      wifiPass: PRIVACY_SENSITIVE.wifiPass,
      hostname: PRIVACY_SENSITIVE.hostname,
      ipv4: PRIVACY_SENSITIVE.ip,
    };
    const service = makeService(privacyInfo);
    let snap: BlockIntelligenceSnapshot | undefined;
    service.snapshot$.subscribe(s => (snap = s));
    const serialized = JSON.stringify(snap);
    for (const secret of PRIVACY_FORBIDDEN_STRINGS) {
      expect(serialized).not.toContain(secret);
    }
    // ...yet still matched the active pool from the host alone.
    expect(snap!.blocks.some(b => b.configuredMatch === 'active')).toBeTrue();
  });
});
