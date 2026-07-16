import { Component, OnDestroy, OnInit } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { Observable, Subject, map, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';
import { SystemInfo as ISystemInfo, GenericResponse } from 'src/app/generated/models';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { NEURALAXE } from 'src/app/neuralaxe';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';

/** One derived, read-only operational insight (frontend-only view model). */
export interface DeckInsight {
  icon: string;
  severity: 'ok' | 'info' | 'warn' | 'error';
  label: string;
  detail: string;
}

const HERO_SERIES_LENGTH = 48;
const SPARK_SERIES_LENGTH = 24;

@Component({
  selector: 'app-command-deck',
  templateUrl: './command-deck.component.html',
})
export class CommandDeckComponent implements OnInit, OnDestroy {
  public readonly neuralaxe = NEURALAXE;
  public info$: Observable<ISystemInfo>;
  public connected$: Observable<boolean>;

  public heroSeries: number[] = [];
  public domainSeries: number[][] = [];
  public insights: DeckInsight[] = [];

  public chartData: any;
  public chartOptions: any;

  private destroy$ = new Subject<void>();

  constructor(
    private liveDataService: LiveDataService,
    private systemService: SystemApiService,
    private toastr: ToastrService,
  ) {
    this.info$ = this.liveDataService.info$;
    this.connected$ = this.liveDataService.connected$;
  }

  ngOnInit(): void {
    this.initChart();
    this.info$.pipe(takeUntil(this.destroy$)).subscribe(info => {
      this.pushSample(this.heroSeries, info.hashRate ?? 0, HERO_SERIES_LENGTH);
      const domains = info.hashrateMonitor?.asics?.[0]?.domains ?? [];
      domains.forEach((value, i) => {
        if (!this.domainSeries[i]) {
          this.domainSeries[i] = [];
        }
        this.pushSample(this.domainSeries[i], value ?? 0, SPARK_SERIES_LENGTH);
      });
      this.insights = this.deriveInsights(info);
      this.updateChart();
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
    this.chartData = { labels: [], datasets: [] };
  }

  private updateChart(): void {
    this.chartData = {
      labels: this.heroSeries.map((_, i) => i),
      datasets: [{
        data: [...this.heroSeries],
        borderColor: '#2fe6a0',
        backgroundColor: 'rgba(47, 230, 160, 0.08)',
        fill: true,
        tension: 0.35,
      }],
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

  public deriveInsights(info: ISystemInfo): DeckInsight[] {
    const insights: DeckInsight[] = [];

    const overheat = (info.overheat_mode ?? 0) !== 0;
    if (overheat) {
      insights.push({ icon: 'pi-exclamation-triangle', severity: 'error', label: 'Overheat protection', detail: 'Device entered overheat mode' });
    } else {
      insights.push({ icon: 'pi-wave-pulse', severity: 'ok', label: 'Stable', detail: 'All systems normal' });
    }

    const eff = this.efficiency(info);
    if (eff !== null) {
      if (eff <= 26) {
        insights.push({ icon: 'pi-bolt', severity: 'ok', label: 'Excellent efficiency', detail: `${eff.toFixed(1)} J/TH is top tier` });
      } else if (eff <= 34) {
        insights.push({ icon: 'pi-bolt', severity: 'info', label: 'Good efficiency', detail: `${eff.toFixed(1)} J/TH` });
      } else {
        insights.push({ icon: 'pi-bolt', severity: 'warn', label: 'Efficiency below par', detail: `${eff.toFixed(1)} J/TH — check cooling/tuning` });
      }
    }

    if ((info.isUsingFallbackStratum ?? 0) !== 0) {
      insights.push({ icon: 'pi-shield', severity: 'warn', label: 'Fallback pool active', detail: 'Primary pool unreachable' });
    } else if ((info.responseTime ?? 0) <= 150) {
      insights.push({ icon: 'pi-shield', severity: 'ok', label: 'Pool healthy', detail: 'Low latency, high stability' });
    } else {
      insights.push({ icon: 'pi-shield', severity: 'info', label: 'Pool latency elevated', detail: `${info.responseTime} ms response time` });
    }

    const spread = this.domainSpread(info);
    if (spread !== null) {
      if (spread > 15) {
        insights.push({ icon: 'pi-sliders-h', severity: 'warn', label: 'Mild domain imbalance', detail: 'Hash domains drift apart — informational' });
      } else {
        insights.push({ icon: 'pi-sliders-h', severity: 'ok', label: 'Domains balanced', detail: `Spread ${spread.toFixed(1)}%` });
      }
    }

    return insights;
  }

  // ---------- formatting helpers ----------

  /** Big hero number, unit-scaled (input is GH/s). */
  public heroValue(info: ISystemInfo): string {
    const gh = info.hashRate ?? 0;
    if (gh >= 1000) {
      return (gh / 1000).toFixed(2);
    }
    return gh >= 100 ? gh.toFixed(0) : gh.toFixed(1);
  }

  public heroUnit(info: ISystemInfo): string {
    return (info.hashRate ?? 0) >= 1000 ? 'TH/s' : 'GH/s';
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
