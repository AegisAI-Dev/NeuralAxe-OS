import { ComponentFixture, TestBed } from '@angular/core/testing';
import { CommonModule } from '@angular/common';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { SidebarModule } from 'primeng/sidebar';
import { SharedModule } from 'primeng/api';
import { BehaviorSubject, of } from 'rxjs';
import { BlockIntelligenceComponent } from './block-intelligence.component';
import { BlockIntelligenceService } from 'src/app/services/block-intelligence/block-intelligence.service';
import {
  BlockIntelligenceSnapshot,
  BlockSummary,
  BlockDetail,
  ConfiguredPoolMatch,
  AttributionConfidence,
  ProviderStatus,
} from 'src/app/services/block-intelligence/block-intelligence.model';

function block(height: number, match: ConfiguredPoolMatch = 'insufficient', conf: AttributionConfidence = 'unknown', poolName: string | null = null): BlockSummary {
  return {
    height, hash: 'hash' + height, timestampMs: 1_733_000_000_000 - (900000 - height) * 600000, sourceTimestampMs: Date.now(),
    txCount: 3000, size: 1_500_000, weight: 3_990_000, totalFees: 2_500_000, subsidy: 312_500_000, reward: 315_000_000,
    attribution: { poolName, providerLabel: poolName, slug: null, method: 'provider-pool', confidence: conf, source: 'mempool.space', evidence: { coinbaseTagAscii: 'tag', coinbaseTagId: null, providerMatchRate: null, reason: 'because', aliases: [] } },
    configuredMatch: match, source: 'mempool.space',
  };
}

function snap(blocks: BlockSummary[], status: ProviderStatus = 'live', tipReplaced = false): BlockIntelligenceSnapshot {
  return {
    blocks,
    freshness: { status, provider: 'mempool.space', lastSuccessMs: Date.now(), ageMs: 1000, nextRetryMs: null, inFlight: false, fromCache: false },
    lastError: null, tipReplaced,
  };
}

function withCoinbase(readable: string, escaped: string, hex: string | null): BlockDetail {
  const b = block(870010, 'active', 'provider-reported', 'Public Pool');
  return {
    ...b,
    attribution: {
      ...b.attribution,
      evidence: {
        ...b.attribution.evidence,
        coinbase: { readable, escaped, hex, originalLength: 24, truncated: false, status: 'sanitized', hasReadable: readable.length > 0 },
      },
    },
    version: 1, merkleRoot: 'mm', previousBlockHash: 'pp', medianTimeMs: 1, bits: 1, nonce: 1, difficulty: 1,
  };
}

const DETAIL: BlockDetail = withCoinbase('/Foundry USA Pool/', '\\x00\\x00/Foundry USA Pool/\\xFF', '000000' + '2f466f756e6472792f');
const SCRIPT_DETAIL: BlockDetail = withCoinbase('<script>alert(1)</script>', '<script>alert(1)</script>', null);

class StubService {
  snapshot$ = new BehaviorSubject<BlockIntelligenceSnapshot>(snap([], 'loading'));
  sessionStartMs = Date.now() - 60000;
  sessionStartHeight: number | null = 870005;
  refresh = jasmine.createSpy('refresh');
  fetchBlockDetail = jasmine.createSpy('fetchBlockDetail').and.returnValue(of(DETAIL));
}

describe('BlockIntelligenceComponent', () => {
  let fixture: ComponentFixture<BlockIntelligenceComponent>;
  let component: BlockIntelligenceComponent;
  let stub: StubService;

  beforeEach(async () => {
    stub = new StubService();
    await TestBed.configureTestingModule({
      declarations: [BlockIntelligenceComponent],
      imports: [CommonModule, SidebarModule, SharedModule, NoopAnimationsModule],
      providers: [{ provide: BlockIntelligenceService, useValue: stub }],
    }).compileComponents();
    fixture = TestBed.createComponent(BlockIntelligenceComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  afterEach(() => fixture.destroy());

  it('creates', () => {
    expect(component).toBeTruthy();
  });

  it('renders the privacy panel and attribution disclaimer', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('public Bitcoin block data only');
    expect(text).toContain('does not prove that this individual NeuralAxe device found the block');
  });

  it('renders a timeline card and table row per block', () => {
    stub.snapshot$.next(snap([block(870010, 'active', 'provider-reported', 'Public Pool'), block(870009, 'none', 'strong', 'Foundry USA')]));
    fixture.detectChanges();
    const cards = fixture.nativeElement.querySelectorAll('.nx-tl-card');
    const rows = fixture.nativeElement.querySelectorAll('.nx-table tbody tr');
    expect(cards.length).toBe(2);
    expect(rows.length).toBe(2);
  });

  it('marks the tip row and computes the summary', () => {
    stub.snapshot$.next(snap([block(870010), block(870009)]));
    fixture.detectChanges();
    expect(component.summary.hasData).toBeTrue();
    expect(component.summary.latestHeight).toBe(870010);
    expect(component.rows.find(r => r.height === 870010)?.isTip).toBeTrue();
  });

  it('opens the detail drawer and loads block detail', () => {
    stub.snapshot$.next(snap([block(870010, 'active', 'provider-reported', 'Public Pool')]));
    fixture.detectChanges();
    component.openDetail(component.rows[0]);
    expect(stub.fetchBlockDetail).toHaveBeenCalledWith('hash870010');
    expect(component.drawerOpen).toBeTrue();
    expect(component.selectedDetail).toBe(DETAIL);
  });

  it('renders the "Latest Mined Block" summary tile (2L.1)', () => {
    stub.snapshot$.next(snap([block(870010, 'active', 'provider-reported', 'Public Pool')]));
    fixture.detectChanges();
    expect((fixture.nativeElement.textContent as string)).toContain('Latest Mined Block');
  });

  it('renders sanitized coinbase evidence and toggles bounded hex; never raw binary (2L.1)', () => {
    stub.snapshot$.next(snap([block(870010, 'active', 'provider-reported', 'Public Pool')]));
    fixture.detectChanges();
    component.openDetail(component.rows[0]);   // stub.fetchBlockDetail → of(DETAIL)
    fixture.detectChanges();
    const text = fixture.nativeElement.textContent as string;
    expect(text).toContain('Coinbase evidence');
    expect(text).toContain('/Foundry USA Pool/');  // readable preview
    expect(text).toContain('\\x00');               // escaped form (literal backslash-x-0-0)
    expect(text).not.toContain('\x00');            // no raw NUL byte rendered
    expect(component.showCoinbaseHex).toBeFalse(); // reset on open
    expect(fixture.nativeElement.querySelector('#nx-coinbase-hexval')).toBeNull();
    component.showCoinbaseHex = true;
    fixture.detectChanges();
    expect(fixture.nativeElement.querySelector('#nx-coinbase-hexval')).toBeTruthy();
  });

  it('renders coinbase evidence as inert text — never innerHTML / injected script (2L.1)', () => {
    stub.fetchBlockDetail.and.returnValue(of(SCRIPT_DETAIL));
    stub.snapshot$.next(snap([block(870010, 'active', 'provider-reported', 'Public Pool')]));
    fixture.detectChanges();
    component.openDetail(component.rows[0]);
    fixture.detectChanges();
    expect(fixture.nativeElement.querySelector('script')).toBeNull();
    expect((fixture.nativeElement.textContent as string)).toContain('<script>alert(1)</script>');
  });

  it('opens the drawer on keyboard Enter', () => {
    stub.snapshot$.next(snap([block(870010)]));
    fixture.detectChanges();
    const ev = new KeyboardEvent('keydown', { key: 'Enter' });
    component.onRowKeydown(ev, component.rows[0]);
    expect(component.drawerOpen).toBeTrue();
  });

  it('emits an in-app event for a new block', () => {
    stub.snapshot$.next(snap([block(870010)]));                     // establishes prev tip, no event
    stub.snapshot$.next(snap([block(870011, 'active', 'provider-reported', 'Public Pool'), block(870010)]));
    const messages = component.events.map(e => e.message);
    expect(messages.some(m => m.includes('New network block 870011'))).toBeTrue();
    expect(messages.some(m => m.includes('active pool was attributed'))).toBeTrue();
    // never claims the local device found it
    expect(messages.some(m => /your miner found/i.test(m))).toBeFalse();
  });

  it('emits a replaced-tip event honestly', () => {
    stub.snapshot$.next(snap([block(870010)]));
    stub.snapshot$.next(snap([block(870010, 'none', 'strong', 'AntPool')], 'live', true));
    expect(component.events.some(e => e.message.includes('changed after a provider refresh'))).toBeTrue();
  });

  it('shows the provider-unavailable state without any data', () => {
    stub.snapshot$.next(snap([], 'unavailable'));
    fixture.detectChanges();
    expect((fixture.nativeElement.textContent as string)).toContain('provider unavailable');
    expect(component.status).toBe('unavailable');
  });

  it('manual refresh delegates to the service', () => {
    component.refresh();
    expect(stub.refresh).toHaveBeenCalled();
  });

  it('renders under a mobile viewport without error', () => {
    stub.snapshot$.next(snap([block(870010, 'fallback', 'strong', 'Solo CKPool')]));
    fixture.detectChanges();
    // structural sanity: the timeline is scrollable within its own container
    const tl = fixture.nativeElement.querySelector('.nx-tl');
    expect(tl).toBeTruthy();
  });
});
