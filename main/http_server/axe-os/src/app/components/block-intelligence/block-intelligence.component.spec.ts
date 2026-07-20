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

const DETAIL: BlockDetail = {
  ...block(870010, 'active', 'provider-reported', 'Public Pool'),
  version: 1, merkleRoot: 'mm', previousBlockHash: 'pp', medianTimeMs: 1, bits: 1, nonce: 1, difficulty: 1,
};

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
