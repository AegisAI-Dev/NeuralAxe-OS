/**
 * Block Intelligence service (Phase 2L, Stages 10 & 15).
 *
 * The ONE shared block stream for the whole app. Command Deck card and the
 * workspace both consume `snapshot$` — there is never a second polling loop.
 *
 * Contract:
 *   - visibility-aware cadence (~45 s foreground, slower hidden);
 *   - exponential backoff on failure, bounded; manual refresh always allowed;
 *   - per-request timeout; stale requests aborted; never overlapping requests;
 *   - provider fallback mempool.space → Blockstream → cached snapshot → offline;
 *   - response size bounded; retained history bounded;
 *   - honest freshness (live / stale / cached / retrying / unavailable / loading);
 *   - configured-pool matching is HOST-ONLY and local (wallet/worker never used);
 *   - in development / tests, deterministic fixtures are served with NO network,
 *     so screenshots are reproducible and unit tests never reach the internet.
 *
 * PRIVACY: the only values ever read from device telemetry are the configured
 * stratum HOSTS (for local matching). No wallet, worker, password, SSID, IP,
 * hostname, tuning or hashrate is ever sent to a block-data provider.
 */

import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import {
  BehaviorSubject,
  Observable,
  Subscription,
  defer,
  fromEvent,
  of,
  timer,
} from 'rxjs';
import {
  catchError,
  distinctUntilChanged,
  map,
  shareReplay,
  startWith,
  take,
  timeout,
} from 'rxjs/operators';
import { environment } from '../../../environments/environment';
import { SystemInfo as ISystemInfo } from '../../generated/models';
import { LocalStorageService } from '../../local-storage.service';
import { LiveDataService } from '../live-data.service';
import {
  BlockDetail,
  BlockIntelligenceSnapshot,
  BlockProviderDescriptor,
  BlockSummary,
  ProviderError,
} from './block-intelligence.model';
import {
  BACKOFF_BASE_MS,
  REQUEST_TIMEOUT_MS,
  CACHE_KEY,
  cadenceForVisibility,
  checkResponseSize,
  deriveFreshness,
  deserializeCache,
  emptySnapshot,
  nextBackoffMs,
  serializeCache,
} from './block-refresh';
import { applyConfiguredMatches, ConfiguredPools, normalizePoolHost } from './pool-normalize';
import { providerBlocksUrl, providerBlockDetailUrl } from './provider-request';
import { dedupeAndBound, detectTipChange, tipOf } from './block-normalize';
import {
  MEMPOOL_PROVIDER,
  normalizeMempoolBlocks,
  normalizeMempoolDetail,
} from './mempool-provider';
import {
  ESPLORA_PROVIDER,
  normalizeEsploraBlocks,
  normalizeEsploraDetail,
} from './esplora-provider';
import {
  MEMPOOL_BLOCKS_MIXED,
  BLOCK_AMBIGUOUS,
} from './block-fixtures';

interface EngineState {
  blocks: BlockSummary[];
  lastSuccessMs: number | null;
  consecutiveFailures: number;
  lastError: ProviderError | null;
  fromCache: boolean;
  providerId: string | null;
  inFlight: boolean;
  nextRetryMs: number | null;
  tipReplaced: boolean;
}

@Injectable({ providedIn: 'root' })
export class BlockIntelligenceService {
  /** ms epoch when this page session (engine) started. */
  public readonly sessionStartMs = Date.now();
  /** Tip height captured when the first data arrived; null until then. */
  public sessionStartHeight: number | null = null;

  private readonly snapshotSubject = new BehaviorSubject<BlockIntelligenceSnapshot>(
    emptySnapshot({ status: 'loading', now: Date.now() }),
  );

  private state: EngineState = {
    blocks: [],
    lastSuccessMs: null,
    consecutiveFailures: 0,
    lastError: null,
    fromCache: false,
    providerId: null,
    inFlight: false,
    nextRetryMs: null,
    tipReplaced: false,
  };

  private config: ConfiguredPools = { activeHost: null, fallbackHost: null };
  private visibility = 'visible';
  private engineStarted = false;
  /**
   * True in production. A test seam so the live external HTTP path can be
   * exercised (and its outbound requests inspected) without a production build.
   * It never changes WHICH endpoints are contacted — only whether the network is
   * used at all versus the deterministic dev fixtures.
   */
  private liveProviders = environment.production;
  private tickSub: Subscription | null = null;
  private readonly subs = new Subscription();

  /**
   * Shared snapshot stream. `defer` starts the engine exactly once, on the first
   * subscription; `shareReplay({refCount:false})` keeps that single engine alive
   * and replays the latest snapshot to late subscribers (the workspace) — no
   * duplicate polling.
   */
  public readonly snapshot$: Observable<BlockIntelligenceSnapshot> = defer(() => {
    this.startEngine();
    return this.snapshotSubject.asObservable();
  }).pipe(shareReplay({ bufferSize: 1, refCount: false }));

  constructor(
    private http: HttpClient,
    private liveData: LiveDataService,
    private storage: LocalStorageService,
  ) {}

  /** Force an immediate refresh (manual refresh button). */
  public refresh(): void {
    this.runRefresh('manual');
  }

  // ------------------------------------------------------------------ engine

  private startEngine(): void {
    if (this.engineStarted) {
      return;
    }
    this.engineStarted = true;

    // Restore a bounded cached snapshot so the UI is usable before the first fetch.
    this.restoreCache();

    // Track configured pool HOSTS from the shared telemetry stream. HOST ONLY —
    // the stratum username (wallet.worker) is never read here.
    this.subs.add(
      this.liveData.info$
        .pipe(
          map((info) => this.configFromInfo(info)),
          distinctUntilChanged((a, b) => a.activeHost === b.activeHost && a.fallbackHost === b.fallbackHost),
        )
        .subscribe((cfg) => {
          this.config = cfg;
          // Re-apply matching to already-loaded blocks without a network call.
          if (this.state.blocks.length > 0) {
            this.state.blocks = applyConfiguredMatches(this.state.blocks, this.config);
            this.emit();
          }
        }),
    );

    // Visibility-aware cadence: reschedule the pending tick when visibility flips.
    const visibility$ = fromEvent(document, 'visibilitychange').pipe(
      map(() => document.visibilityState),
      startWith(document.visibilityState),
      distinctUntilChanged(),
    );
    this.subs.add(
      visibility$.subscribe((state) => {
        this.visibility = state;
        // Only reschedule the ordinary cadence tick, never interrupt a backoff.
        if (!this.state.inFlight && this.state.consecutiveFailures === 0) {
          this.scheduleTick(cadenceForVisibility(state));
        }
      }),
    );

    // Kick off immediately.
    this.runRefresh('auto');
  }

  private scheduleTick(delayMs: number): void {
    this.tickSub?.unsubscribe();
    this.tickSub = timer(delayMs).subscribe(() => this.runRefresh('auto'));
  }

  /**
   * Run one refresh. `exhaustMap`-style guard: while a request is in flight, an
   * auto tick is ignored (never overlapping). A manual refresh during flight is
   * also ignored to keep the single-request invariant, but always reschedules.
   */
  private runRefresh(_kind: 'auto' | 'manual'): void {
    if (this.state.inFlight) {
      return;
    }
    this.state.inFlight = true;
    this.state.tipReplaced = false;
    this.emit();

    const now = Date.now();
    this.fetchBlocks(now)
      .pipe(take(1))
      .subscribe({
        next: (result) => {
          if ('error' in result) {
            this.onFailure(result.error);
          } else {
            this.onSuccess(result.blocks, result.providerId);
          }
        },
        error: (err) => this.onFailure(this.toProviderError(err, this.state.providerId ?? MEMPOOL_PROVIDER.id)),
      });
  }

  private onSuccess(blocks: BlockSummary[], providerId: string): void {
    const now = Date.now();
    const matched = applyConfiguredMatches(dedupeAndBound(blocks), this.config);
    const tipChange = detectTipChange(this.state.blocks.length ? this.state.blocks : null, matched);

    this.state.blocks = matched;
    this.state.lastSuccessMs = now;
    this.state.consecutiveFailures = 0;
    this.state.lastError = null;
    this.state.fromCache = false;
    this.state.providerId = providerId;
    this.state.inFlight = false;
    this.state.nextRetryMs = null;
    this.state.tipReplaced = tipChange.replaced;

    const tip = tipOf(matched);
    if (this.sessionStartHeight === null && tip) {
      this.sessionStartHeight = tip.height;
    }

    this.persistCache();
    this.emit();
    this.scheduleTick(cadenceForVisibility(this.visibility));
  }

  private onFailure(error: ProviderError): void {
    this.state.consecutiveFailures += 1;
    this.state.lastError = error;
    this.state.inFlight = false;
    const backoff = nextBackoffMs(this.state.consecutiveFailures, BACKOFF_BASE_MS);
    this.state.nextRetryMs = Date.now() + backoff;
    this.emit();
    this.scheduleTick(backoff);
  }

  private emit(): void {
    const now = Date.now();
    const tip = tipOf(this.state.blocks);
    const dataSourceMs = tip ? tip.sourceTimestampMs : null;
    const freshness = deriveFreshness({
      hasData: this.state.blocks.length > 0,
      inFlight: this.state.inFlight,
      fromCache: this.state.fromCache,
      lastSuccessMs: this.state.lastSuccessMs,
      dataSourceMs,
      consecutiveFailures: this.state.consecutiveFailures,
      nextRetryMs: this.state.nextRetryMs,
      providerId: this.state.providerId,
      now,
    });
    this.snapshotSubject.next({
      blocks: this.state.blocks,
      freshness,
      lastError: this.state.lastError,
      tipReplaced: this.state.tipReplaced,
    });
  }

  // --------------------------------------------------------------- fetching

  private configFromInfo(info: ISystemInfo): ConfiguredPools {
    return {
      activeHost: normalizePoolHost(info?.stratumURL) || null,
      fallbackHost: normalizePoolHost(info?.fallbackStratumURL) || null,
    };
  }

  /**
   * Fetch recent blocks with provider fallback. In non-production builds this
   * resolves deterministic fixtures with NO network access.
   */
  private fetchBlocks(now: number): Observable<{ blocks: BlockSummary[]; providerId: string } | { error: ProviderError }> {
    if (!this.liveProviders) {
      return of({ blocks: this.devBlocks(now), providerId: MEMPOOL_PROVIDER.id });
    }
    return this.httpBlocks(MEMPOOL_PROVIDER, now, (raw) => normalizeMempoolBlocks(raw, MEMPOOL_PROVIDER.id, now)).pipe(
      catchError(() =>
        this.httpBlocks(ESPLORA_PROVIDER, now, (raw) => normalizeEsploraBlocks(raw, ESPLORA_PROVIDER.id, now)).pipe(
          catchError((err) => of({ error: this.toProviderError(err, ESPLORA_PROVIDER.id) })),
        ),
      ),
    );
  }

  private httpBlocks(
    descriptor: BlockProviderDescriptor,
    now: number,
    normalize: (raw: unknown) => BlockSummary[],
  ): Observable<{ blocks: BlockSummary[]; providerId: string } | { error: ProviderError }> {
    const url = providerBlocksUrl(descriptor);
    return this.http.get(url, { responseType: 'json' }).pipe(
      timeout(REQUEST_TIMEOUT_MS),
      map((raw): { blocks: BlockSummary[]; providerId: string } | { error: ProviderError } => {
        const oversized = checkResponseSize(raw, descriptor.id, now);
        if (oversized) {
          throw oversized;
        }
        const blocks = normalize(raw);
        if (blocks.length === 0) {
          throw { provider: descriptor.id, kind: 'empty', message: 'Provider returned no usable blocks.', atMs: now } as ProviderError;
        }
        return { blocks, providerId: descriptor.id };
      }),
    );
  }

  private toProviderError(err: any, provider: string): ProviderError {
    const now = Date.now();
    if (err && typeof err === 'object' && 'kind' in err && 'provider' in err) {
      return err as ProviderError;
    }
    const name = err?.name ?? '';
    if (name === 'TimeoutError') {
      return { provider, kind: 'timeout', message: 'Request timed out.', atMs: now };
    }
    if (typeof err?.status === 'number' && err.status > 0) {
      return { provider, kind: 'http', message: `Provider responded ${err.status}.`, atMs: now };
    }
    return { provider, kind: 'network', message: 'Could not reach the block-data provider.', atMs: now };
  }

  // ------------------------------------------------------------ block detail

  /** Fetch a single block's detail (drawer). Dev builds resolve from fixtures. */
  public fetchBlockDetail(hash: string): Observable<BlockDetail | null> {
    const now = Date.now();
    if (!this.liveProviders) {
      const raw = [...MEMPOOL_BLOCKS_MIXED, BLOCK_AMBIGUOUS].find((b: any) => b.id === hash);
      const detail = raw ? normalizeMempoolDetail(raw, MEMPOOL_PROVIDER.id, now) : null;
      if (!detail) {
        return of(null);
      }
      const rebased = { ...detail, timestampMs: detail.timestampMs + this.devTimeOffset(now) };
      return of(applyConfiguredMatches([rebased], this.config)[0] as BlockDetail);
    }
    const mempoolUrl = providerBlockDetailUrl(MEMPOOL_PROVIDER, hash);
    return this.http.get(mempoolUrl, { responseType: 'json' }).pipe(
      timeout(REQUEST_TIMEOUT_MS),
      map((raw) => normalizeMempoolDetail(raw, MEMPOOL_PROVIDER.id, now)),
      map((detail) => (detail ? (applyConfiguredMatches([detail], this.config)[0] as BlockDetail) : null)),
      catchError(() => {
        const esploraUrl = providerBlockDetailUrl(ESPLORA_PROVIDER, hash);
        return this.http.get(esploraUrl, { responseType: 'json' }).pipe(
          timeout(REQUEST_TIMEOUT_MS),
          map((raw) => normalizeEsploraDetail(raw, ESPLORA_PROVIDER.id, now)),
          map((detail) => (detail ? (applyConfiguredMatches([detail], this.config)[0] as BlockDetail) : null)),
          catchError(() => of(null)),
        );
      }),
    );
  }

  // ----------------------------------------------------- development fixtures

  /**
   * Deterministic fixture blocks, re-based so the tip is ~2 minutes old at the
   * current wall clock. This keeps ages realistic in `ng serve` and in the
   * deterministic screenshot harness without any network access. Relative
   * intervals between blocks are preserved.
   */
  private devBlocks(now: number): BlockSummary[] {
    const blocks = dedupeAndBound(normalizeMempoolBlocks([...MEMPOOL_BLOCKS_MIXED, BLOCK_AMBIGUOUS], MEMPOOL_PROVIDER.id, now));
    const offset = this.devTimeOffset(now, blocks);
    return blocks.map((b) => ({ ...b, timestampMs: b.timestampMs + offset, sourceTimestampMs: now }));
  }

  /** Offset that maps the fixture tip timestamp to ~2 minutes before `now`. */
  private devTimeOffset(now: number, blocks?: BlockSummary[]): number {
    const set = blocks ?? dedupeAndBound(normalizeMempoolBlocks([...MEMPOOL_BLOCKS_MIXED, BLOCK_AMBIGUOUS], MEMPOOL_PROVIDER.id, now));
    const maxTs = set.reduce((m, b) => (b.timestampMs > m ? b.timestampMs : m), 0);
    return maxTs > 0 ? (now - 120_000) - maxTs : 0;
  }

  // ------------------------------------------------------------------ cache

  private restoreCache(): void {
    const cached = deserializeCache(this.storage.getItem(CACHE_KEY));
    if (cached && cached.blocks.length > 0) {
      this.state.blocks = applyConfiguredMatches(dedupeAndBound(cached.blocks), this.config);
      this.state.fromCache = true;
      this.state.providerId = cached.providerId;
      this.emit();
    }
  }

  private persistCache(): void {
    try {
      this.storage.setItem(CACHE_KEY, serializeCache(this.state.blocks, this.state.providerId, Date.now()));
    } catch {
      // Storage full / unavailable — non-fatal; the in-memory snapshot still works.
    }
  }
}
