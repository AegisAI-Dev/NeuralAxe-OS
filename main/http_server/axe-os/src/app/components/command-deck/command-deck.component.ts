import { Component, OnDestroy, OnInit } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { Observable, Subject, shareReplay, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';
import { SystemInfo as ISystemInfo, SystemAsic as ISystemASIC, GenericResponse } from 'src/app/generated/models';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { VersionState, deriveVersionState } from 'src/app/services/version-state';
import { NEURALAXE } from 'src/app/neuralaxe';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { DeckFmt, fmtLatency } from './deck-format';
import {
  SoloOdds,
  ThermalControlInsight,
  ThermalHeadroom,
  ThermalStatusView,
  modeAwareThermalStatus,
  bestDiffPctOfNetwork,
  compactNumber,
  currentVsAveragePct,
  currentVsExpectedPct,
  formatExpectedTime,
  formatPctCompact,
  formatProbabilityPct,
  recentVariabilityPct,
  rejectRatePct,
  sharesPerHour,
  soloOdds,
  thermalControlInsight,
  thermalHeadroom,
} from './deck-intel';
import { curveSegmentLabel, fanCurveSummary, thermalModeLabel } from '../edit/tuning';
import { FleetDevice, fleetSummary } from '../swarm/fleet-intel';
import { LocalStorageService } from 'src/app/local-storage.service';

/** Snapshot of the stored fleet list for the compact deck entry (2I). */
export interface FleetGlance {
  total: number;
  online: number;
  totalHashRate: number;
  alerts: number;
  /** Age of the newest stored device contact; null when no timestamp exists. */
  ageText: string | null;
}

/** One derived, read-only operational insight (frontend-only view model). */
export interface DeckInsight {
  icon: string;
  severity: 'ok' | 'info' | 'warn' | 'error';
  label: string;
  detail: string;
}

const HERO_SERIES_LENGTH = 48;
const SPARK_SERIES_LENGTH = 24;

export type DeckRange = 'live' | '5m' | '15m' | '1h' | 'all';
export type DeckOverlay = 'none' | 'asicTemp' | 'power' | 'errorPercentage';

const RANGE_MS: { [key in Exclude<DeckRange, 'live' | 'all'>]: number } = {
  '5m': 5 * 60 * 1000,
  '15m': 15 * 60 * 1000,
  '1h': 60 * 60 * 1000,
};

const OVERLAY_META: { [key in Exclude<DeckOverlay, 'none'>]: { label: string; color: string } } = {
  asicTemp: { label: 'ASIC Temp (°C)', color: '#e6b23c' },
  power: { label: 'Power (W)', color: '#35c3e6' },
  errorPercentage: { label: 'ASIC Errors (%)', color: '#ef5350' },
};

@Component({
  selector: 'app-command-deck',
  templateUrl: './command-deck.component.html',
})
export class CommandDeckComponent implements OnInit, OnDestroy {
  public readonly neuralaxe = NEURALAXE;
  public readonly fmt = DeckFmt;
  public readonly formatExpectedTime = formatExpectedTime;
  public readonly formatProbabilityPct = formatProbabilityPct;
  public readonly formatPctCompact = formatPctCompact;
  public readonly compactNumber = compactNumber;
  public info$: Observable<ISystemInfo>;
  public connected$: Observable<boolean>;
  public asic$: Observable<ISystemASIC>;

  public heroSeries: number[] = [];
  public tempSeries: number[] = [];
  public powerSeries: number[] = [];
  public errorSeries: number[] = [];
  public domainSeries: number[][] = [];
  public insights: DeckInsight[] = [];

  /** Live /version.txt revision; null when unavailable (never substituted). */
  public installedWebVersion: string | null = null;
  public versionState: VersionState | null = null;

  /** Advanced telemetry drawer (secondary expert info), collapsed by default. */
  public drawerOpen = false;

  // ---- hero chart state (Stage 5) ----
  public selectedRange: DeckRange = 'live';
  public selectedOverlay: DeckOverlay = 'none';
  public readonly ranges: { id: DeckRange; label: string }[] = [
    { id: 'live', label: 'Live' },
    { id: '5m', label: '5 min' },
    { id: '15m', label: '15 min' },
    { id: '1h', label: '1 h' },
    { id: 'all', label: 'All' },
  ];
  public readonly overlays: { id: DeckOverlay; label: string }[] = [
    { id: 'none', label: 'Hashrate only' },
    { id: 'asicTemp', label: '+ ASIC Temp' },
    { id: 'power', label: '+ Power' },
    { id: 'errorPercentage', label: '+ ASIC Errors' },
  ];
  /** User-facing note when history cannot be shown (warm-up, logging off …). */
  public historyNote: string | null = null;
  private historyPoints: { t: number; hashrate: number; overlay: number | null }[] = [];
  private lastInfo: ISystemInfo | null = null;

  public chartData: any;
  public chartOptions: any;

  private destroy$ = new Subject<void>();

  /** Non-null only when the Fleet page has genuinely discovered devices. */
  public fleetGlance: FleetGlance | null = null;

  constructor(
    private liveDataService: LiveDataService,
    private systemService: SystemApiService,
    private webVersionService: WebVersionService,
    private toastr: ToastrService,
    private localStorageService: LocalStorageService,
  ) {
    this.info$ = this.liveDataService.info$;
    this.connected$ = this.liveDataService.connected$;
    this.asic$ = this.systemService.getAsicSettings().pipe(
      shareReplay({ refCount: true, bufferSize: 1 })
    );
    this.fleetGlance = this.deriveFleetGlance();
  }

  /**
   * Compact fleet snapshot from the list the Fleet page stores locally. This
   * is a point-in-time view (refreshed whenever Fleet runs), so it carries an
   * explicit data age instead of pretending to be live. Hidden entirely when
   * no fleet beyond this device exists.
   */
  private deriveFleetGlance(): FleetGlance | null {
    const stored = this.localStorageService.getObject('SWARM_DATA') as FleetDevice[] | null;
    if (!Array.isArray(stored) || stored.length === 0) {
      return null;
    }
    const others = stored.filter(device => device.IP !== window.location.hostname);
    if (others.length === 0) {
      return null;
    }
    const summary = fleetSummary(stored);
    const newest = stored.reduce((max, device) =>
      typeof device.nxLastSeenMs === 'number' && device.nxLastSeenMs > max ? device.nxLastSeenMs : max, 0);
    let ageText: string | null = null;
    if (newest > 0) {
      const ageMin = Math.round((Date.now() - newest) / 60000);
      ageText = ageMin < 2 ? 'just now' : ageMin < 90 ? `${ageMin} min ago` : `${Math.round(ageMin / 60)} h ago`;
    }
    return {
      total: summary.total,
      online: summary.online,
      totalHashRate: summary.totalHashRate,
      alerts: summary.attention + summary.critical,
      ageText,
    };
  }

  ngOnInit(): void {
    this.initChart();
    this.webVersionService.installedWebVersion$
      .pipe(takeUntil(this.destroy$))
      .subscribe(v => {
        this.installedWebVersion = v;
        if (this.lastInfo) {
          this.versionState = deriveVersionState(this.lastInfo.version, this.lastInfo.axeOSVersion, v);
        }
      });
    this.info$.pipe(takeUntil(this.destroy$)).subscribe(info => {
      this.lastInfo = info;
      this.pushSample(this.heroSeries, info.hashRate ?? 0, HERO_SERIES_LENGTH);
      this.pushSample(this.tempSeries, info.temp ?? 0, HERO_SERIES_LENGTH);
      this.pushSample(this.powerSeries, info.power ?? 0, HERO_SERIES_LENGTH);
      this.pushSample(this.errorSeries, info.errorPercentage ?? 0, HERO_SERIES_LENGTH);
      const domains = info.hashrateMonitor?.asics?.[0]?.domains ?? [];
      domains.forEach((value, i) => {
        if (!this.domainSeries[i]) {
          this.domainSeries[i] = [];
        }
        this.pushSample(this.domainSeries[i], value ?? 0, SPARK_SERIES_LENGTH);
      });
      this.versionState = deriveVersionState(info.version, info.axeOSVersion, this.installedWebVersion);
      this.insights = this.deriveInsights(info);
      if (this.selectedRange === 'live') {
        this.updateChart();
      }
    });
  }

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  private pushSample(series: number[], value: number, max: number): void {
    series.push(value);
    if (series.length > max) {
      series.shift();
    }
  }

  // ---------- hero chart (chart.js via p-chart) ----------

  private initChart(): void {
    this.chartOptions = {
      animation: false,
      responsive: true,
      maintainAspectRatio: false,
      plugins: { legend: { display: false }, tooltip: { enabled: false } },
      scales: {
        x: { display: false },
        y: {
          position: 'right',
          grid: { color: 'rgba(56, 214, 154, 0.08)' },
          border: { display: false },
          ticks: { color: '#5e7f93', font: { size: 10 }, maxTicksLimit: 4 },
        },
      },
      elements: { line: { borderWidth: 1.5 }, point: { radius: 0 } },
    };
    this.updateChart();
  }

  /** Range selection; anything but Live loads a history snapshot. */
  public setRange(range: DeckRange): void {
    if (this.selectedRange === range) {
      return;
    }
    this.selectedRange = range;
    if (range === 'live') {
      this.historyNote = null;
      this.updateChart();
    } else {
      this.loadHistory();
    }
  }

  public setOverlay(overlay: DeckOverlay): void {
    if (this.selectedOverlay === overlay) {
      return;
    }
    this.selectedOverlay = overlay;
    if (this.selectedRange === 'live') {
      this.updateChart();
    } else {
      this.loadHistory();
    }
  }

  private overlayLiveSeries(): number[] | null {
    switch (this.selectedOverlay) {
      case 'asicTemp': return this.tempSeries;
      case 'power': return this.powerSeries;
      case 'errorPercentage': return this.errorSeries;
      default: return null;
    }
  }

  private applyOverlayAxis(hasOverlay: boolean): void {
    const scales: any = {
      x: { display: false },
      y: {
        position: 'right',
        grid: { color: 'rgba(56, 214, 154, 0.08)' },
        border: { display: false },
        ticks: { color: '#5e7f93', font: { size: 10 }, maxTicksLimit: 4 },
      },
    };
    if (hasOverlay) {
      const meta = OVERLAY_META[this.selectedOverlay as Exclude<DeckOverlay, 'none'>];
      scales.y1 = {
        position: 'left',
        grid: { drawOnChartArea: false },
        border: { display: false },
        ticks: { color: meta.color, font: { size: 10 }, maxTicksLimit: 4 },
      };
    }
    this.chartOptions = { ...this.chartOptions, scales };
  }

  private updateChart(): void {
    const overlaySeries = this.overlayLiveSeries();
    this.applyOverlayAxis(!!overlaySeries);
    const datasets: any[] = [{
      data: [...this.heroSeries],
      borderColor: '#2fe6a0',
      backgroundColor: 'rgba(47, 230, 160, 0.08)',
      fill: true,
      tension: 0.35,
      yAxisID: 'y',
    }];
    if (overlaySeries) {
      const meta = OVERLAY_META[this.selectedOverlay as Exclude<DeckOverlay, 'none'>];
      datasets.push({
        data: [...overlaySeries],
        borderColor: meta.color,
        fill: false,
        tension: 0.35,
        yAxisID: 'y1',
      });
    }
    this.chartData = {
      labels: this.heroSeries.map((_, i) => i),
      datasets,
    };
  }

  /**
   * Load a history snapshot from the device's rolling statistics buffer.
   * Only ever triggered by an explicit range/overlay selection — never on
   * page load. No data is invented: if the buffer does not cover the chosen
   * window (warm-up or logging disabled), an explicit note is shown instead.
   */
  private loadHistory(): void {
    const overlayKey = this.selectedOverlay === 'none' ? 'hashrate' : this.selectedOverlay;
    this.systemService.getStatistics(overlayKey, 'hashrate')
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: stats => {
          const idxHashrate = stats.labels.indexOf('hashrate');
          const idxOverlay = this.selectedOverlay === 'none' ? -1 : stats.labels.indexOf(this.selectedOverlay);
          const idxTimestamp = stats.labels.indexOf('timestamp');
          if (idxHashrate === -1 || idxTimestamp === -1 || !stats.statistics?.length) {
            this.historyPoints = [];
            this.renderHistory('No history is available yet — the device has not logged any statistics.');
            return;
          }

          const points = stats.statistics
            .map(row => ({
              t: row[idxTimestamp],
              hashrate: row[idxHashrate] || 0,
              overlay: idxOverlay !== -1 ? (row[idxOverlay] ?? null) : null,
            }))
            .sort((a, b) => a.t - b.t);

          const latest = points[points.length - 1].t;
          const windowed = this.selectedRange === 'all'
            ? points
            : points.filter(p => latest - p.t <= RANGE_MS[this.selectedRange as Exclude<DeckRange, 'live' | 'all'>]);

          this.historyPoints = windowed;
          if (windowed.length < 2) {
            this.renderHistory('Not enough logged history for this range yet — the buffer is still warming up.');
            return;
          }

          const coveredMs = latest - windowed[0].t;
          const requestedMs = this.selectedRange === 'all' ? coveredMs : RANGE_MS[this.selectedRange as Exclude<DeckRange, 'live' | 'all'>];
          this.renderHistory(coveredMs < requestedMs * 0.8
            ? `Only ${Math.max(1, Math.round(coveredMs / 60000))} min of history is available so far.`
            : null);
        },
        error: () => {
          this.historyPoints = [];
          this.renderHistory('Could not load history from the device — showing nothing rather than made-up data.');
        }
      });
  }

  private renderHistory(note: string | null): void {
    this.historyNote = note;
    const hasOverlay = this.selectedOverlay !== 'none' && this.historyPoints.some(p => p.overlay !== null);
    this.applyOverlayAxis(hasOverlay);
    const datasets: any[] = [{
      data: this.historyPoints.map(p => p.hashrate),
      borderColor: '#2fe6a0',
      backgroundColor: 'rgba(47, 230, 160, 0.08)',
      fill: true,
      tension: 0.35,
      yAxisID: 'y',
    }];
    if (hasOverlay) {
      const meta = OVERLAY_META[this.selectedOverlay as Exclude<DeckOverlay, 'none'>];
      datasets.push({
        data: this.historyPoints.map(p => p.overlay ?? 0),
        borderColor: meta.color,
        fill: false,
        tension: 0.35,
        yAxisID: 'y1',
      });
    }
    this.chartData = {
      labels: this.historyPoints.map(p => p.t),
      datasets,
    };
  }

  // ---------- derived read-only view model ----------

  /** J/TH derived from live power and hashrate; null when hashrate is 0. */
  public efficiency(info: ISystemInfo): number | null {
    const th = (info.hashRate ?? 0) / 1000;
    return th > 0 ? (info.power ?? 0) / th : null;
  }

  /** Share stability in percent; null before any share was submitted. */
  public stability(info: ISystemInfo): number | null {
    const total = (info.sharesAccepted ?? 0) + (info.sharesRejected ?? 0);
    return total > 0 ? ((info.sharesAccepted ?? 0) / total) * 100 : null;
  }

  /** Relative spread between strongest and weakest hash domain, in percent. */
  public domainSpread(info: ISystemInfo): number | null {
    const domains = info.hashrateMonitor?.asics?.[0]?.domains ?? [];
    if (domains.length < 2) {
      return null;
    }
    const avg = domains.reduce((a, b) => a + b, 0) / domains.length;
    if (avg <= 0) {
      return null;
    }
    return ((Math.max(...domains) - Math.min(...domains)) / avg) * 100;
  }

  // ---- Stage 4 intelligence (thin wrappers over tested deck-intel) ----

  public rejectRate(info: ISystemInfo): number | null {
    return rejectRatePct(info.sharesAccepted, info.sharesRejected);
  }

  public sharesHr(info: ISystemInfo): number | null {
    return sharesPerHour(info.sharesAccepted, info.uptimeSeconds);
  }

  public vsExpected(info: ISystemInfo): number | null {
    return currentVsExpectedPct(info.hashRate, info.expectedHashrate);
  }

  public vsHourAverage(info: ISystemInfo): number | null {
    return currentVsAveragePct(info.hashRate, info.hashRate_1h);
  }

  public variability(): number | null {
    return recentVariabilityPct(this.heroSeries);
  }

  public odds(info: ISystemInfo): SoloOdds | null {
    return soloOdds(info.networkDifficulty, info.hashRate);
  }

  public bestSessionPct(info: ISystemInfo): number | null {
    return bestDiffPctOfNetwork(info.bestSessionDiff, info.networkDifficulty);
  }

  public bestAllTimePct(info: ISystemInfo): number | null {
    return bestDiffPctOfNetwork(info.bestDiff, info.networkDifficulty);
  }

  public headroom(info: ISystemInfo): ThermalHeadroom {
    return thermalHeadroom(info.temp, info.temptarget, (info.autofanspeed ?? 0) == 1, info.fanspeed);
  }

  /** Mode-aware thermal pill (2H.1): tested pure derivation in deck-intel. */
  public thermalStatus(info: ISystemInfo): ThermalStatusView {
    return modeAwareThermalStatus(info);
  }

  public thermalPillClass(severity: ThermalStatusView['severity']): string {
    switch (severity) {
      case 'ok': return 'nx-pill-ok';
      case 'warn': return 'nx-pill-warn';
      case 'error': return 'nx-pill-err';
      default: return '';
    }
  }

  /**
   * ASIC gauge amber threshold, mode-aware: the configured target only means
   * something in TARGET mode; curve/manual use the fixed 70 °C overheat line.
   */
  public asicGaugeWarn(info: ISystemInfo): boolean {
    const temp = info.temp ?? 0;
    if (info.thermalControlMode === 'target') {
      return temp >= (info.temptarget ?? 60) + 5;
    }
    return temp >= 70;
  }

  // ---- Phase 2H thermal-control visibility (thin wrappers, tested logic) ----

  public thermalIntel(info: ISystemInfo): ThermalControlInsight {
    return thermalControlInsight(info);
  }

  public thermalModeText(info: ISystemInfo): string {
    return thermalModeLabel(info.thermalControlMode);
  }

  public curveSegmentText(info: ISystemInfo): string {
    return curveSegmentLabel(info.activeCurveSegment);
  }

  public fanCurveText(info: ISystemInfo): string {
    return fanCurveSummary(info.fanCurve);
  }

  public controlTempText(info: ISystemInfo): string {
    const t = info.effectiveControlTemperature;
    if (typeof t !== 'number' || !isFinite(t) || t <= 0 || info.controlSensorValid !== 1) {
      return DeckFmt.INVALID;
    }
    return DeckFmt.temp(t);
  }

  /** Measured minus configured core voltage in mV; null without both readings. */
  public voltageDeltaMv(info: ISystemInfo): number | null {
    const configured = info.coreVoltage;
    const measured = info.coreVoltageActual;
    if (typeof configured !== 'number' || !isFinite(configured) || configured <= 0
      || typeof measured !== 'number' || !isFinite(measured) || measured <= 0) {
      return null;
    }
    return measured - configured;
  }

  public fallbackConfigured(info: ISystemInfo): boolean {
    return !!(info.fallbackStratumURL && String(info.fallbackStratumURL).trim() !== '');
  }

  public deriveInsights(info: ISystemInfo): DeckInsight[] {
    const insights: DeckInsight[] = [];

    const overheat = (info.overheat_mode ?? 0) !== 0;
    if (overheat) {
      insights.push({ icon: 'pi-exclamation-triangle', severity: 'error', label: 'Overheat protection', detail: 'Device entered overheat mode' });
    } else {
      insights.push({ icon: 'pi-wave-pulse', severity: 'ok', label: 'Stable', detail: 'All systems normal' });
    }

    // Live fan-decision transparency (Phase 2H). Skipped during overheat:
    // the emergency insight above already owns that state.
    if (!overheat) {
      const thermal = thermalControlInsight(info);
      insights.push({ icon: thermal.icon, severity: thermal.severity, label: thermal.label, detail: thermal.detail });
    }

    const eff = this.efficiency(info);
    if (eff !== null) {
      if (eff <= 26) {
        insights.push({ icon: 'pi-bolt', severity: 'ok', label: 'Excellent efficiency', detail: `${eff.toFixed(1)} J/TH is top tier` });
      } else if (eff <= 34) {
        insights.push({ icon: 'pi-bolt', severity: 'info', label: 'Good efficiency', detail: `${eff.toFixed(1)} J/TH` });
      } else {
        insights.push({ icon: 'pi-bolt', severity: 'warn', label: 'Efficiency below par', detail: `${eff.toFixed(1)} J/TH is above the expected range` });
      }
    }

    if ((info.isUsingFallbackStratum ?? 0) !== 0) {
      insights.push({ icon: 'pi-shield', severity: 'warn', label: 'Fallback pool active', detail: 'Mining on the fallback pool' });
    } else if ((info.responseTime ?? 0) <= 150) {
      insights.push({ icon: 'pi-shield', severity: 'ok', label: 'Pool healthy', detail: 'Low latency, high stability' });
    } else {
      insights.push({ icon: 'pi-shield', severity: 'info', label: 'Pool latency elevated', detail: `${fmtLatency(info.responseTime)} response time` });
    }

    if ((info.isUsingFallbackStratum ?? 0) === 0 && !this.fallbackConfigured(info)) {
      insights.push({ icon: 'pi-share-alt', severity: 'info', label: 'No fallback pool', detail: 'Only the primary pool is configured' });
    }

    const spread = this.domainSpread(info);
    if (spread !== null) {
      if (spread > 15) {
        insights.push({ icon: 'pi-sliders-h', severity: 'warn', label: 'Mild domain imbalance', detail: 'Hash domains drift apart — informational' });
      } else {
        insights.push({ icon: 'pi-sliders-h', severity: 'ok', label: 'Domains balanced', detail: `Spread ${spread.toFixed(1)}%` });
      }
    }

    // Version-pair integrity (shared rules in services/version-state.ts).
    const vs = this.versionState;
    if (vs) {
      if (vs.status === 'mismatch') {
        insights.push({ icon: 'pi-clone', severity: 'warn', label: 'Firmware / web mismatch', detail: `Firmware ${vs.firmware} vs web ${vs.installedWeb} — install both from one release` });
      } else if (vs.status === 'match' && vs.restartPending) {
        insights.push({ icon: 'pi-refresh', severity: 'info', label: 'Web update restart pending', detail: `Installed pair matches; restart refreshes the boot snapshot (${vs.bootWeb})` });
      }
    }

    return insights;
  }

  // ---------- formatting helpers ----------

  /** Big hero number, unit-scaled (input is GH/s); em dash on invalid data. */
  public heroValue(info: ISystemInfo): string {
    const gh = info.hashRate;
    if (gh === null || gh === undefined || typeof gh !== 'number' || !isFinite(gh)) {
      return DeckFmt.INVALID;
    }
    if (gh >= 1000) {
      return (gh / 1000).toFixed(2);
    }
    return gh >= 100 ? gh.toFixed(0) : gh.toFixed(1);
  }

  public heroUnit(info: ISystemInfo): string {
    const gh = info.hashRate;
    if (gh === null || gh === undefined || typeof gh !== 'number' || !isFinite(gh)) {
      return '';
    }
    return gh >= 1000 ? 'TH/s' : 'GH/s';
  }

  /** Millivolt fields scaled to volts, or null when the reading is invalid. */
  public volts(mv: number | undefined): number | null {
    return (typeof mv === 'number' && isFinite(mv)) ? mv / 1000 : null;
  }

  public uptime(info: ISystemInfo): string {
    const value = DateAgoPipe.transform(info.uptimeSeconds ?? 0, { short: true, intervals: 3, strict: true });
    return typeof value === 'string' && value ? value : '—';
  }

  /** SVG polyline points for a sparkline in a w×h viewBox. */
  public sparkPoints(series: number[], w: number = 72, h: number = 20): string {
    if (!series || series.length === 0) {
      return '';
    }
    if (series.length === 1) {
      return `0,${h / 2} ${w},${h / 2}`;
    }
    const min = Math.min(...series);
    const max = Math.max(...series);
    const range = max - min || 1;
    const pad = 2;
    return series
      .map((v, i) => {
        const x = (i / (series.length - 1)) * w;
        const y = h - pad - ((v - min) / range) * (h - pad * 2);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
      })
      .join(' ');
  }

  /** Stroke-dashoffset for the 3/4 arc gauges (circumference 264 at r=42). */
  public gaugeOffset(value: number, max: number): number {
    const arc = 198; // 3/4 of 264
    const clamped = Math.max(0, Math.min(1, max > 0 ? value / max : 0));
    return arc * (1 - clamped);
  }

  public wifiBars(rssi: number | undefined): number {
    if (rssi === undefined || rssi === null) return 0;
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
  }

  public heapPercent(info: ISystemInfo): number {
    // Qualitative bar: 300 KB internal free heap renders as a full bar.
    return Math.max(2, Math.min(100, ((info.freeHeapInternal ?? info.freeHeap ?? 0) / 300000) * 100));
  }

  public dismissBlockFound(): void {
    this.systemService.dismissBlockFound()
      .subscribe({
        next: (result) => this.toastr.success((result as GenericResponse).message),
        error: (err: HttpErrorResponse) => this.toastr.error(`Could not dismiss. ${err.message}`),
      });
  }

  public trackByIndex(index: number): number {
    return index;
  }
}
