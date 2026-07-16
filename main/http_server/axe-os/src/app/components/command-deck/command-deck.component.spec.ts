import { ComponentFixture, TestBed } from '@angular/core/testing';
import { provideHttpClient } from '@angular/common/http';
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
});
