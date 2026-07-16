import { ComponentFixture, TestBed, fakeAsync, tick, discardPeriodicTasks } from '@angular/core/testing';
import { of } from 'rxjs';
import { provideHttpClient } from '@angular/common/http';
import { SystemApiService } from 'src/app/services/system.service';
import { provideRouter } from '@angular/router';
import { provideToastr } from 'ngx-toastr';
import { ChartModule } from 'primeng/chart';
import { TooltipModule } from 'primeng/tooltip';

import { CommandDeckComponent } from './command-deck.component';
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
      providers: [provideRouter([]), provideHttpClient(), provideToastr()]
    }).compileComponents();

    const systemService = TestBed.inject(SystemApiService);
    spyOn(systemService, 'getInfo').and.returnValue(of(liveInfo));
  });

  function render(): ComponentFixture<CommandDeckComponent> {
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
