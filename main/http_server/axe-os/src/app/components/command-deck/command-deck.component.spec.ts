import { ComponentFixture, TestBed, fakeAsync, tick, discardPeriodicTasks } from '@angular/core/testing';
import { of } from 'rxjs';
import { provideHttpClient } from '@angular/common/http';
import { SystemApiService } from 'src/app/services/system.service';
import { provideRouter } from '@angular/router';
import { provideToastr } from 'ngx-toastr';
import { ChartModule } from 'primeng/chart';
import { TooltipModule } from 'primeng/tooltip';

import { CommandDeckComponent } from './command-deck.component';
import { WebVersionService } from 'src/app/services/web-version.service';
import { HashSuffixPipe } from 'src/app/pipes/hash-suffix.pipe';
import { DiffSuffixPipe } from 'src/app/pipes/diff-suffix.pipe';
import { AddressPipe } from 'src/app/pipes/address.pipe';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

describe('CommandDeckComponent', () => {
  let component: CommandDeckComponent;
  let fixture: ComponentFixture<CommandDeckComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [CommandDeckComponent, HashSuffixPipe, DiffSuffixPipe, AddressPipe],
      imports: [ChartModule, TooltipModule],
      providers: [provideRouter([]), provideHttpClient(), provideToastr()]
    }).compileComponents();

    // Deterministic fleet-glance state regardless of other spec files.
    window.localStorage.removeItem('SWARM_DATA');
    fixture = TestBed.createComponent(CommandDeckComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  const baseInfo = (overrides: Partial<ISystemInfo> = {}): ISystemInfo => ({
    hashRate: 1200,
    power: 20,
    sharesAccepted: 990,
    sharesRejected: 10,
    responseTime: 30,
    isUsingFallbackStratum: 0,
    overheat_mode: 0,
    hashrateMonitor: { asics: [{ total: 400, domains: [100, 100, 100, 100], errorCount: 0 }], hashrate: 400 },
    ...overrides,
  } as unknown as ISystemInfo);

  it('should derive efficiency in J/TH from power and hashrate', () => {
    expect(component.efficiency(baseInfo())).toBeCloseTo(20 / 1.2, 3);
    expect(component.efficiency(baseInfo({ hashRate: 0 } as any))).toBeNull();
  });

  it('should derive share stability', () => {
    expect(component.stability(baseInfo())).toBeCloseTo(99, 1);
    expect(component.stability(baseInfo({ sharesAccepted: 0, sharesRejected: 0 } as any))).toBeNull();
  });

  it('should flag domain imbalance above 15 percent spread', () => {
    const balanced = component.deriveInsights(baseInfo());
    expect(balanced.some(i => i.label === 'Domains balanced')).toBeTrue();

    const imbalanced = component.deriveInsights(baseInfo({
      hashrateMonitor: { asics: [{ total: 400, domains: [140, 90, 100, 70], errorCount: 0 }], hashrate: 400 },
    } as any));
    expect(imbalanced.some(i => i.label === 'Mild domain imbalance')).toBeTrue();
  });

  it('should report fallback pool as a warning insight', () => {
    const insights = component.deriveInsights(baseInfo({ isUsingFallbackStratum: 1 } as any));
    const pool = insights.find(i => i.icon === 'pi-shield');
    expect(pool?.severity).toBe('warn');
    expect(pool?.label).toContain('Fallback');
  });

  it('should report overheat mode as an error insight', () => {
    const insights = component.deriveInsights(baseInfo({ overheat_mode: 1 } as any));
    expect(insights[0].severity).toBe('error');
  });

  it('should build sparkline points within the viewbox', () => {
    const points = component.sparkPoints([1, 2, 3, 2], 72, 20);
    const coords = points.split(' ').map(p => p.split(',').map(Number));
    expect(coords.length).toBe(4);
    for (const [x, y] of coords) {
      expect(x).toBeGreaterThanOrEqual(0);
      expect(x).toBeLessThanOrEqual(72);
      expect(y).toBeGreaterThanOrEqual(0);
      expect(y).toBeLessThanOrEqual(20);
    }
  });

  it('should clamp gauge offsets', () => {
    expect(component.gaugeOffset(0, 100)).toBe(198);
    expect(component.gaugeOffset(100, 100)).toBe(0);
    expect(component.gaugeOffset(500, 100)).toBe(0);
  });

  describe('insight threshold boundaries (deterministic, no tuning advice)', () => {
    const withEff = (jpth: number) => baseInfo({ hashRate: 1000, power: jpth } as any);

    it('treats exactly 26 J/TH as excellent and just above as good', () => {
      expect(component.deriveInsights(withEff(26)).find(i => i.icon === 'pi-bolt')?.severity).toBe('ok');
      expect(component.deriveInsights(withEff(26.01)).find(i => i.icon === 'pi-bolt')?.severity).toBe('info');
    });

    it('treats exactly 34 J/TH as good and just above as a warning', () => {
      expect(component.deriveInsights(withEff(34)).find(i => i.icon === 'pi-bolt')?.severity).toBe('info');
      expect(component.deriveInsights(withEff(34.01)).find(i => i.icon === 'pi-bolt')?.severity).toBe('warn');
    });

    it('treats exactly 150 ms latency as healthy and just above as informational', () => {
      expect(component.deriveInsights(baseInfo({ responseTime: 150 } as any)).find(i => i.icon === 'pi-shield')?.severity).toBe('ok');
      const elevated = component.deriveInsights(baseInfo({ responseTime: 150.5 } as any)).find(i => i.icon === 'pi-shield');
      expect(elevated?.severity).toBe('info');
      expect(elevated?.detail).toContain('151 ms'); // formatted, not raw fractional
    });

    it('treats exactly 15% domain spread as balanced and above as imbalance', () => {
      const at15 = baseInfo({ hashrateMonitor: { asics: [{ total: 400, domains: [107.5, 92.5, 100, 100], errorCount: 0 }], hashrate: 400 } } as any);
      const above = baseInfo({ hashrateMonitor: { asics: [{ total: 400, domains: [108, 92, 100, 100], errorCount: 0 }], hashrate: 400 } } as any);
      expect(component.deriveInsights(at15).some(i => i.label === 'Domains balanced')).toBeTrue();
      expect(component.deriveInsights(above).some(i => i.label === 'Mild domain imbalance')).toBeTrue();
    });

    it('adds a version mismatch warning only for a live-verified mismatch', () => {
      component.installedWebVersion = 'v2.14.2-13-g388287da';
      component.versionState = {
        firmware: 'v2.14.2-15-g723e61dc', installedWeb: 'v2.14.2-13-g388287da',
        bootWeb: 'v2.14.2-13-g388287da', webSource: 'live', restartPending: false, status: 'mismatch',
      };
      const insights = component.deriveInsights(baseInfo());
      const mismatch = insights.find(i => i.label.includes('mismatch'));
      expect(mismatch?.severity).toBe('warn');
      expect(mismatch?.detail).toContain('one release');
    });

    it('reports a stale boot snapshot as an informational restart note, never a warning', () => {
      component.versionState = {
        firmware: 'v2.14.2-15-g723e61dc', installedWeb: 'v2.14.2-15-g723e61dc',
        bootWeb: 'v2.14.2-13-g388287da', webSource: 'live', restartPending: true, status: 'match',
      };
      const insights = component.deriveInsights(baseInfo());
      const note = insights.find(i => i.label.includes('restart pending'));
      expect(note?.severity).toBe('info');
      expect(insights.some(i => i.label.includes('mismatch'))).toBeFalse();
    });

    it('adds no version insight while the state is unknown or unverified', () => {
      component.versionState = null;
      expect(component.deriveInsights(baseInfo()).some(i => i.label.toLowerCase().includes('mismatch'))).toBeFalse();
      component.versionState = {
        firmware: 'v2.14.2', installedWeb: null, bootWeb: 'v2.14.2',
        webSource: 'boot-snapshot', restartPending: false, status: 'unverified',
      };
      expect(component.deriveInsights(baseInfo()).some(i => i.label.toLowerCase().includes('mismatch'))).toBeFalse();
    });

    it('notes a missing fallback pool as information without touching pool health', () => {
      const insights = component.deriveInsights(baseInfo()); // baseInfo has no fallbackStratumURL
      expect(insights.find(i => i.icon === 'pi-shield')?.severity).toBe('ok');
      expect(insights.find(i => i.label === 'No fallback pool')?.severity).toBe('info');

      const withFallback = component.deriveInsights(baseInfo({ fallbackStratumURL: 'solo.ckpool.org' } as any));
      expect(withFallback.some(i => i.label === 'No fallback pool')).toBeFalse();
    });

    it('never suggests tuning actions or claims AI in insight copy', () => {
      const all = [
        ...component.deriveInsights(baseInfo()),
        ...component.deriveInsights(withEff(40)),
        ...component.deriveInsights(baseInfo({ isUsingFallbackStratum: 1, overheat_mode: 1, responseTime: 900 } as any)),
      ];
      for (const insight of all) {
        const text = (insight.label + ' ' + insight.detail).toLowerCase();
        expect(text).not.toContain('tuning');
        expect(text).not.toContain(' ai ');
        expect(text).not.toContain('automatic');
      }
    });
  });
});

describe('CommandDeckComponent (hero chart ranges & overlays)', () => {
  let component: CommandDeckComponent;
  let fixture: ComponentFixture<CommandDeckComponent>;
  let systemService: SystemApiService;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [CommandDeckComponent, HashSuffixPipe, DiffSuffixPipe, AddressPipe],
      imports: [ChartModule, TooltipModule],
      providers: [provideRouter([]), provideHttpClient(), provideToastr()]
    }).compileComponents();

    fixture = TestBed.createComponent(CommandDeckComponent);
    component = fixture.componentInstance;
    systemService = TestBed.inject(SystemApiService);
    fixture.detectChanges();
  });

  function statsResponse(spanMinutes: number, stepSeconds: number = 30) {
    const points = Math.floor((spanMinutes * 60) / stepSeconds) + 1;
    const statistics: number[][] = [];
    for (let i = 0; i < points; i++) {
      statistics.push([1200 + i, 58 + (i % 3), i * stepSeconds * 1000]);
    }
    return of({
      currentTimestamp: (points - 1) * stepSeconds * 1000,
      labels: ['hashrate', 'asicTemp', 'timestamp'],
      statistics,
    } as any);
  }

  it('defaults to the live rolling series with a single hashrate dataset', () => {
    expect(component.selectedRange).toBe('live');
    expect(component.selectedOverlay).toBe('none');
    expect(component.chartData.datasets.length).toBe(1);
  });

  it('adds an overlay dataset on its own axis in live mode', () => {
    component.heroSeries = [1, 2, 3];
    component.tempSeries = [55, 56, 57];
    component.setOverlay('asicTemp');
    expect(component.chartData.datasets.length).toBe(2);
    expect(component.chartData.datasets[1].yAxisID).toBe('y1');
    expect(component.chartOptions.scales.y1).toBeTruthy();

    component.setOverlay('none');
    expect(component.chartData.datasets.length).toBe(1);
    expect(component.chartOptions.scales.y1).toBeUndefined();
  });

  it('loads a history snapshot only on explicit range selection and windows it', () => {
    const statsSpy = spyOn(systemService, 'getStatistics').and.returnValue(statsResponse(120));
    expect(statsSpy).not.toHaveBeenCalled();  // nothing on page load

    component.setRange('15m');
    expect(statsSpy).toHaveBeenCalledTimes(1);
    // 15 min at 30 s cadence → 31 points inside the window
    expect(component.chartData.datasets[0].data.length).toBe(31);
    expect(component.historyNote).toBeNull(); // fully covered window → no note
  });

  it('keeps the full buffer for the All range', () => {
    spyOn(systemService, 'getStatistics').and.returnValue(statsResponse(120));
    component.setRange('all');
    expect(component.chartData.datasets[0].data.length).toBe(241);
  });

  it('notes partial coverage honestly instead of inventing history', () => {
    spyOn(systemService, 'getStatistics').and.returnValue(statsResponse(10));
    component.setRange('1h');
    expect(component.historyNote).toContain('10 min');
  });

  it('shows an explicit empty state when no statistics exist', () => {
    spyOn(systemService, 'getStatistics').and.returnValue(of({
      currentTimestamp: 0, labels: ['hashrate', 'timestamp'], statistics: [],
    } as any));
    component.setRange('1h');
    expect(component.historyNote).toContain('No history');
    expect(component.chartData.datasets[0].data.length).toBe(0);
  });

  it('includes the overlay column in history mode when selected', () => {
    spyOn(systemService, 'getStatistics').and.returnValue(statsResponse(30));
    component.selectedOverlay = 'asicTemp';
    component.setRange('15m');
    expect(component.chartData.datasets.length).toBe(2);
    expect(component.chartData.datasets[1].data[0]).toBeGreaterThanOrEqual(58);
  });

  it('returning to Live restores the rolling series and clears notes', () => {
    spyOn(systemService, 'getStatistics').and.returnValue(statsResponse(10));
    component.setRange('1h');
    expect(component.historyNote).toBeTruthy();
    component.setRange('live');
    expect(component.historyNote).toBeNull();
    expect(component.selectedRange).toBe('live');
  });
});

describe('CommandDeckComponent (rendered with live-like data)', () => {
  let fixture: ComponentFixture<CommandDeckComponent>;

  const liveInfo = {
    hashRate: 1290, hashRate_1h: 1260, expectedHashrate: 1275,
    power: 20.9, errorPercentage: 0.81,
    temp: 59, vrTemp: 53.6, temptarget: 55,
    fanrpm: 15230, fanspeed: 100,
    voltage: 5208.75, coreVoltageActual: 1141, frequency: 625,
    sharesAccepted: 18760, sharesRejected: 153, responseTime: 106.8960037,
    isUsingFallbackStratum: 0, overheat_mode: 0,
    stratumURL: 'na.pool.example.com', stratumPort: 3333,
    stratumUser: 'bc1qexampleexampleexampleexample.worker',
    hashrateMonitor: { asics: [{ total: 1290, domains: [304, 370, 329, 321], errorCount: 2 }], hashrate: 1290 },
    blockFound: 1, blockHeight: 842763,
    freeHeap: 200504, freeHeapInternal: 200504, wifiRSSI: -42, cpuUsage: 18,
    version: 'v2.14.2-8-gabc1234', productVersion: '0.1.0-dev',
    uptimeSeconds: 218520,
  } as any;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [CommandDeckComponent, HashSuffixPipe, DiffSuffixPipe, AddressPipe],
      imports: [ChartModule, TooltipModule],
      providers: [
        provideRouter([]), provideHttpClient(), provideToastr(),
        // The real service XHRs /version.txt in its constructor — not allowed inside fakeAsync.
        { provide: WebVersionService, useValue: { installedWebVersion$: of(null) } },
      ]
    }).compileComponents();

    const systemService = TestBed.inject(SystemApiService);
    spyOn(systemService, 'getInfo').and.returnValue(of(liveInfo));
  });

  function render(): ComponentFixture<CommandDeckComponent> {
    window.localStorage.removeItem('SWARM_DATA');
    fixture = TestBed.createComponent(CommandDeckComponent);
    fixture.detectChanges();
    tick(1200);
    fixture.detectChanges();
    return fixture;
  }

  it('should keep fan RPM and percentage cleanly separated (no overlap) with long RPM values', fakeAsync(() => {
    const el: HTMLElement = render().nativeElement;
    const rpm = el.querySelector('.nx-gauge-read-fan b') as HTMLElement;
    const labels = el.querySelectorAll('.nx-gauge-read-fan small');
    expect(rpm?.textContent).toContain('15,230');
    expect(labels.length).toBe(2);
    expect(labels[1].textContent).toContain('Fan 100%');

    const rpmRect = rpm.getBoundingClientRect();
    const subRect = (labels[1] as HTMLElement).getBoundingClientRect();
    // vertical separation: percentage line starts below the RPM value line
    expect(subRect.top).toBeGreaterThanOrEqual(rpmRect.bottom - 1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('should format real-device latency and keep System Status factual', fakeAsync(() => {
    const text = (render().nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('107 ms');            // 106.8960037 rounded
    expect(text).not.toContain('106.896');
    expect(text).toContain('Free Memory (Heap)');
    expect(text).not.toContain('Up to date');
    expect(text).toContain('dBm');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('should render the Block Signal card without confetti iconography', fakeAsync(() => {
    const el: HTMLElement = render().nativeElement;
    expect(el.querySelector('.nx-block-signal')).toBeTruthy();
    expect(el.querySelector('.nx-block-hex')).toBeTruthy();
    expect((el.querySelector('.nx-block-height') as HTMLElement)?.textContent).toContain('842,763');
    expect(el.textContent).not.toContain('🎉');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

describe('CommandDeckComponent (fleet glance, Phase 2I)', () => {
  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [CommandDeckComponent, HashSuffixPipe, DiffSuffixPipe, AddressPipe],
      imports: [ChartModule, TooltipModule],
      providers: [provideRouter([]), provideHttpClient(), provideToastr()]
    }).compileComponents();
  });

  function recreate(): CommandDeckComponent {
    return TestBed.createComponent(CommandDeckComponent).componentInstance;
  }

  it('is hidden without stored fleet data', () => {
    window.localStorage.removeItem('SWARM_DATA');
    expect(recreate().fleetGlance).toBeNull();
  });

  it('is hidden when the stored list only contains this device', () => {
    window.localStorage.setItem('SWARM_DATA', JSON.stringify([
      { IP: window.location.hostname, hostname: 'self', nxReachable: true },
    ]));
    expect(recreate().fleetGlance).toBeNull();
    window.localStorage.removeItem('SWARM_DATA');
  });

  it('distinguishes never-refreshed devices from an outage (2I.1)', () => {
    // Stored list without any reachability bookkeeping = awaiting first refresh.
    window.localStorage.setItem('SWARM_DATA', JSON.stringify([
      { IP: '10.0.0.10', hostname: 'gamma-01' },
      { IP: '10.0.0.11', hostname: 'gamma-02' },
      { IP: '10.0.0.12', hostname: 'gamma-03' },
    ]));
    const glance = recreate().fleetGlance;
    expect(glance).not.toBeNull();
    expect(glance!.pending).toBe(3);
    expect(glance!.online).toBe(0);
    // 0/3 "online" must not read as a fleet outage before any data exists —
    // the template renders the awaiting-first-refresh wording instead.
    window.localStorage.removeItem('SWARM_DATA');
  });

  it('derives online/total, hashrate, alerts and data age from the stored fleet', () => {
    const now = Date.now();
    window.localStorage.setItem('SWARM_DATA', JSON.stringify([
      { IP: '10.0.0.10', hostname: 'gamma-01', nxReachable: true, nxLastSeenMs: now - 30_000, hashRate: 1230, power: 22, temp: 57 },
      { IP: '10.0.0.11', hostname: 'gamma-02', nxReachable: true, nxLastSeenMs: now - 30_000, hashRate: 1100, power: 21, temp: 66 },
      { IP: '10.0.0.12', hostname: 'gamma-03', nxReachable: false, nxLastSeenMs: now - 600_000 },
    ]));
    const glance = recreate().fleetGlance;
    expect(glance).not.toBeNull();
    expect(glance!.total).toBe(3);
    expect(glance!.online).toBe(2);
    expect(glance!.totalHashRate).toBeCloseTo(2330, 5);
    expect(glance!.alerts).toBe(1); // the 66 °C device needs attention
    expect(glance!.ageText).toBe('just now');
    window.localStorage.removeItem('SWARM_DATA');
  });
});
