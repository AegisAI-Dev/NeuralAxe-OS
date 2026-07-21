/**
 * Bitcoin Block Intelligence workspace (Phase 2L, Stage 6).
 *
 * Consumes the ONE shared snapshot stream (never a second poll). Presents a
 * summary, a keyboard-accessible recent-block timeline, a block table, a detail
 * drawer, a source-status panel and a persistent privacy panel. Every value is
 * honest: Unknown / Unattributed are shown as such, provider outage / stale /
 * cached states are explicit, and nothing here ever claims this NeuralAxe device
 * found a block.
 */

import { Component, OnDestroy, OnInit } from '@angular/core';
import { Observable, Subject, timer } from 'rxjs';
import { takeUntil } from 'rxjs/operators';
import { NEURALAXE } from 'src/app/neuralaxe';
import { BlockIntelligenceService } from 'src/app/services/block-intelligence/block-intelligence.service';
import { PoolStrategyService } from 'src/app/services/pool-strategy.service';
import { ChainContext, deriveChainContext } from 'src/app/components/pool-strategy/pool-chain';
import {
  AttributionConfidence,
  BlockDetail,
  BlockIntelligenceSnapshot,
  BlockSummary,
  ConfiguredPoolMatch,
  ProviderError,
  ProviderStatus,
} from 'src/app/services/block-intelligence/block-intelligence.model';
import { MEMPOOL_PROVIDER } from 'src/app/services/block-intelligence/mempool-provider';
import {
  computeBlockMetrics,
  sessionObservation,
} from 'src/app/services/block-intelligence/block-metrics';
import { tipOf, blockAgeMs } from 'src/app/services/block-intelligence/block-normalize';
import {
  confidenceLabel,
  confidenceSeverity,
  configuredMatchLabel,
} from 'src/app/services/block-intelligence/attribution';
import {
  formatAgeShort,
  formatBtc,
  formatBytes,
  formatCount,
  formatInterval,
  formatPerHour,
  formatSats,
  formatUtc,
  formatWeight,
  shortHash,
} from 'src/app/services/block-intelligence/block-format';
import { freshnessLabel, freshnessSeverity } from '../command-deck/block-deck';

export interface BlockRow {
  hash: string;
  height: number;
  isTip: boolean;
  highlightNew: boolean;
  ageText: string;
  utcText: string;
  shortHash: string;
  poolLabel: string;
  providerLabel: string | null;
  confidence: AttributionConfidence;
  confidenceText: string;
  confidenceSeverity: 'ok' | 'info' | 'neutral';
  matchKind: ConfiguredPoolMatch;
  matchLabel: string;
  matchNotable: boolean;
  txText: string;
  feesText: string;
  sizeText: string;
  weightText: string;
  rewardText: string;
}

export interface WorkspaceSummary {
  hasData: boolean;
  blockCount: number;
  latestHeight: number | null;
  latestAgeText: string;
  avgIntervalText: string;
  medianIntervalText: string;
  shortestText: string;
  longestText: string;
  perHourText: string;
  activeCount: number;
  fallbackCount: number;
  unknownCount: number;
  highConfidenceText: string;
}

/** A local, non-sensitive in-app event (Stage 13). No push, no sound. */
export interface BlockEvent {
  id: number;
  message: string;
  severity: 'ok' | 'info' | 'neutral';
  atMs: number;
}

const HIGHLIGHT_MS = 20_000;
const MAX_EVENTS = 8;

@Component({
  selector: 'app-block-intelligence',
  templateUrl: './block-intelligence.component.html',
})
export class BlockIntelligenceComponent implements OnInit, OnDestroy {
  public readonly neuralaxe = NEURALAXE;
  public readonly snapshot$: Observable<BlockIntelligenceSnapshot>;
  public readonly primaryProvider = MEMPOOL_PROVIDER;

  public now = Date.now();
  public rows: BlockRow[] = [];
  public summary: WorkspaceSummary = emptySummary();
  public status: ProviderStatus = 'loading';
  public freshnessText = 'Loading…';
  public freshnessSeverity: 'ok' | 'info' | 'warn' | 'neutral' = 'neutral';
  public provider: string | null = null;
  public lastError: ProviderError | null = null;
  public lastSuccessText = '—';
  public dataAgeText = '—';
  public nextRetryText: string | null = null;
  public tipReplaced = false;

  public sessionStartHeight: number | null = null;
  public sessionBlocksObserved = 0;
  public sessionElapsedText = '—';

  public events: BlockEvent[] = [];

  // Detail drawer state
  public drawerOpen = false;
  public detailLoading = false;
  public selectedDetail: BlockDetail | null = null;
  public selectedRow: BlockRow | null = null;
  /** Bounded-hex disclosure in the coinbase-evidence section; reset per open. */
  public showCoinbaseHex = false;

  private lastSnapshot: BlockIntelligenceSnapshot | null = null;
  private prevTipHeight: number | null = null;
  private newTipHeight: number | null = null;
  private newTipAtMs = 0;
  private eventSeq = 0;
  private readonly destroy$ = new Subject<void>();

  // Expose pure formatters for the template.
  public readonly fmtAge = formatAgeShort;
  public readonly fmtUtc = formatUtc;

  /**
   * Active chain context (Phase 2M, Stage 11). Block Intelligence shows the
   * Bitcoin network; when a BCH-labelled profile is active we present a context
   * banner and suppress configured-pool match claims (they would be misleading
   * against Bitcoin blocks). BTC / no-profile keep the normal 2L behavior.
   */
  public chainContext: ChainContext = deriveChainContext(null, null);

  constructor(
    private service: BlockIntelligenceService,
    private poolStrategy: PoolStrategyService,
  ) {
    this.snapshot$ = this.service.snapshot$;
  }

  /** BCH-labelled profile active → Bitcoin data is explicitly out-of-context. */
  public get bchContext(): boolean {
    return this.chainContext.labelled && this.chainContext.chain === 'BCH';
  }

  /** Custom-labelled profile active → chain matching is unavailable. */
  public get customContext(): boolean {
    return this.chainContext.labelled && this.chainContext.chain === 'custom';
  }

  /** Match claims are shown unless a non-BTC profile is actively labelled. */
  public get showConfiguredMatches(): boolean {
    return !(this.chainContext.labelled && this.chainContext.chain !== 'BTC');
  }

  ngOnInit(): void {
    this.poolStrategy.chainContext$.pipe(takeUntil(this.destroy$)).subscribe((ctx) => {
      this.chainContext = ctx;
    });
    this.snapshot$.pipe(takeUntil(this.destroy$)).subscribe((snap) => {
      this.now = Date.now();
      this.detectEvents(snap);
      this.lastSnapshot = snap;
      this.rebuild();
    });
    // Light ticker so relative ages stay current without re-polling.
    timer(15_000, 15_000).pipe(takeUntil(this.destroy$)).subscribe(() => {
      this.now = Date.now();
      this.rebuild();
    });
  }

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  public refresh(): void {
    this.service.refresh();
  }

  public openDetail(row: BlockRow): void {
    this.selectedRow = row;
    this.selectedDetail = null;
    this.detailLoading = true;
    this.drawerOpen = true;
    this.showCoinbaseHex = false;
    this.service.fetchBlockDetail(row.hash).pipe(takeUntil(this.destroy$)).subscribe({
      next: (detail) => {
        this.selectedDetail = detail;
        this.detailLoading = false;
      },
      error: () => {
        this.selectedDetail = null;
        this.detailLoading = false;
      },
    });
  }

  public closeDetail(): void {
    this.drawerOpen = false;
    this.selectedDetail = null;
    this.selectedRow = null;
  }

  public copyHash(hash: string): void {
    if (navigator?.clipboard?.writeText) {
      navigator.clipboard.writeText(hash).catch(() => void 0);
    }
  }

  public onRowKeydown(event: KeyboardEvent, row: BlockRow): void {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      this.openDetail(row);
    }
  }

  public pillClass(severity: 'ok' | 'info' | 'warn' | 'neutral' | 'danger'): string {
    switch (severity) {
      case 'ok': return 'nx-pill-ok';
      case 'warn': return 'nx-pill-warn';
      case 'danger': return 'nx-pill-err';
      case 'info': return 'nx-pill-info';
      default: return 'nx-pill-neutral';
    }
  }

  public matchPillClass(kind: ConfiguredPoolMatch): string {
    if (kind === 'active' || kind === 'both') return 'nx-pill-ok';
    if (kind === 'fallback') return 'nx-pill-info';
    return 'nx-pill-neutral';
  }

  public detailFees(detail: BlockDetail): string { return formatBtc(detail.totalFees); }
  public detailReward(detail: BlockDetail): string { return formatBtc(detail.reward); }
  public detailSubsidy(detail: BlockDetail): string { return formatBtc(detail.subsidy); }
  public detailFeesSats(detail: BlockDetail): string { return formatSats(detail.totalFees); }
  public detailSize(detail: BlockDetail): string { return formatBytes(detail.size); }
  public detailWeight(detail: BlockDetail): string { return formatWeight(detail.weight); }
  public detailTx(detail: BlockDetail): string { return formatCount(detail.txCount); }
  public detailUtc(detail: BlockDetail): string { return formatUtc(detail.timestampMs); }
  public detailAge(detail: BlockDetail): string { return formatAgeShort(blockAgeMs(detail.timestampMs, this.now)); }
  public detailConfidenceText(detail: BlockDetail): string { return confidenceLabel(detail.attribution.confidence); }
  public detailMatchLabel(detail: BlockDetail): string { return configuredMatchLabel(detail.configuredMatch); }

  // ------------------------------------------------------------ derivation

  private rebuild(): void {
    const snap = this.lastSnapshot;
    if (!snap) {
      return;
    }
    this.status = snap.freshness.status;
    this.freshnessText = freshnessLabel(snap.freshness.status);
    this.freshnessSeverity = freshnessSeverity(snap.freshness.status);
    this.provider = snap.freshness.provider;
    this.lastError = snap.lastError;
    this.tipReplaced = snap.tipReplaced;

    if (snap.freshness.lastSuccessMs) {
      const ago = formatAgeShort(Math.max(0, this.now - snap.freshness.lastSuccessMs));
      this.lastSuccessText = ago === 'just now' ? ago : `${ago} ago`;
    } else {
      this.lastSuccessText = 'never';
    }
    this.dataAgeText = snap.freshness.ageMs !== null ? formatAgeShort(snap.freshness.ageMs) : '—';
    this.nextRetryText = snap.freshness.nextRetryMs && snap.freshness.nextRetryMs > this.now
      ? 'in ' + formatInterval(snap.freshness.nextRetryMs - this.now)
      : null;

    this.rows = snap.blocks.map((b) => this.toRow(b));
    this.summary = this.toSummary(snap.blocks);

    const tip = tipOf(snap.blocks);
    this.sessionStartHeight = this.service.sessionStartHeight;
    const sess = sessionObservation(this.service.sessionStartHeight, tip?.height ?? null, this.service.sessionStartMs, this.now);
    this.sessionBlocksObserved = sess.networkBlocksObserved;
    this.sessionElapsedText = formatInterval(sess.elapsedMs);
  }

  private toRow(b: BlockSummary): BlockRow {
    const highlightNew = this.newTipHeight === b.height && (this.now - this.newTipAtMs) < HIGHLIGHT_MS;
    const notable = b.configuredMatch === 'active' || b.configuredMatch === 'fallback' || b.configuredMatch === 'both';
    return {
      hash: b.hash,
      height: b.height,
      isTip: false, // set after mapping (tip is the max height)
      highlightNew,
      ageText: formatAgeShort(blockAgeMs(b.timestampMs, this.now)),
      utcText: formatUtc(b.timestampMs),
      shortHash: shortHash(b.hash),
      poolLabel: b.attribution.poolName ?? (b.attribution.confidence === 'unattributed' ? 'Unattributed' : 'Unknown pool'),
      providerLabel: b.attribution.providerLabel,
      confidence: b.attribution.confidence,
      confidenceText: confidenceLabel(b.attribution.confidence),
      confidenceSeverity: confidenceSeverity(b.attribution.confidence),
      matchKind: b.configuredMatch,
      matchLabel: configuredMatchLabel(b.configuredMatch),
      matchNotable: notable,
      txText: formatCount(b.txCount),
      feesText: formatBtc(b.totalFees),
      sizeText: formatBytes(b.size),
      weightText: formatWeight(b.weight),
      rewardText: formatBtc(b.reward),
    };
  }

  private toSummary(blocks: BlockSummary[]): WorkspaceSummary {
    if (blocks.length === 0) {
      return emptySummary();
    }
    const m = computeBlockMetrics(blocks);
    const tip = tipOf(blocks);
    // Mark the tip row.
    if (tip) {
      const tipRow = this.rows.find((r) => r.height === tip.height);
      if (tipRow) {
        tipRow.isTip = true;
      }
    }
    return {
      hasData: true,
      blockCount: m.blockCount,
      latestHeight: tip?.height ?? null,
      latestAgeText: formatAgeShort(tip ? blockAgeMs(tip.timestampMs, this.now) : null),
      avgIntervalText: formatInterval(m.averageIntervalMs),
      medianIntervalText: formatInterval(m.medianIntervalMs),
      shortestText: formatInterval(m.shortestIntervalMs),
      longestText: formatInterval(m.longestIntervalMs),
      perHourText: formatPerHour(m.blocksPerHour),
      activeCount: m.activePoolBlocks,
      fallbackCount: m.fallbackPoolBlocks,
      unknownCount: m.unknownAttribution,
      highConfidenceText: m.highConfidenceCoverage !== null ? `${Math.round(m.highConfidenceCoverage * 100)}%` : '—',
    };
  }

  // ------------------------------------------------------------ events (13)

  private detectEvents(snap: BlockIntelligenceSnapshot): void {
    const tip = tipOf(snap.blocks);
    if (!tip) {
      return;
    }
    if (this.prevTipHeight !== null && tip.height > this.prevTipHeight) {
      this.newTipHeight = tip.height;
      this.newTipAtMs = Date.now();
      this.pushEvent(`New network block ${tip.height} observed.`, 'info');
      if (tip.configuredMatch === 'active' || tip.configuredMatch === 'both') {
        this.pushEvent('Your active pool was attributed with a newly observed block.', 'ok');
      } else if (tip.configuredMatch === 'fallback') {
        this.pushEvent('Your fallback pool was attributed with a newly observed block.', 'ok');
      }
    }
    if (snap.tipReplaced) {
      this.pushEvent('Recent block data changed after a provider refresh.', 'neutral');
    }
    this.prevTipHeight = tip.height;
  }

  private pushEvent(message: string, severity: 'ok' | 'info' | 'neutral'): void {
    this.events = [{ id: ++this.eventSeq, message, severity, atMs: Date.now() }, ...this.events].slice(0, MAX_EVENTS);
  }

  public trackByHash(_index: number, row: BlockRow): string {
    return row.hash;
  }

  public trackByEvent(_index: number, ev: BlockEvent): number {
    return ev.id;
  }
}

function emptySummary(): WorkspaceSummary {
  return {
    hasData: false,
    blockCount: 0,
    latestHeight: null,
    latestAgeText: '—',
    avgIntervalText: '—',
    medianIntervalText: '—',
    shortestText: '—',
    longestText: '—',
    perHourText: '—',
    activeCount: 0,
    fallbackCount: 0,
    unknownCount: 0,
    highConfidenceText: '—',
  };
}
