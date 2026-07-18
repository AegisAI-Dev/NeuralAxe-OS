import { ComponentFixture, TestBed } from '@angular/core/testing';

import { SwarmComponent } from './swarm.component';
import { ModalComponent } from '../modal/modal.component';
import { FileUploadModule } from 'primeng/fileupload';
import { InputGroupModule } from 'primeng/inputgroup';
import { InputTextModule } from 'primeng/inputtext';
import { DropdownModule } from 'primeng/dropdown';
import { ButtonModule } from 'primeng/button';
import { TooltipModule } from 'primeng/tooltip';
import { SliderModule } from 'primeng/slider';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';
import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting, HttpTestingController } from '@angular/common/http/testing';
import { provideToastr } from 'ngx-toastr';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { DiffSuffixPipe } from 'src/app/pipes/diff-suffix.pipe';
import { HashSuffixPipe } from 'src/app/pipes/hash-suffix.pipe';
import { FleetDevice } from './fleet-intel';

/** Sanitized fixtures — no real identifiers. */
function gammaDevice(overrides: Partial<FleetDevice> = {}): FleetDevice {
  return {
    IP: '10.0.0.10', nxReachable: true, nxLastSeenMs: Date.now(),
    hostname: 'gamma-01', deviceModel: 'Gamma', ASICModel: 'BM1370', boardVersion: '601',
    swarmColor: 'green', asicCount: 1,
    productName: 'NeuralAxe OS', productVersion: '0.1.0-dev',
    version: 'v2.14.2-21-g6b4e7c74', axeOSVersion: 'v2.14.2-21-g6b4e7c74',
    hashRate: 1230, expectedHashrate: 1200, power: 22.4, temp: 57, vrTemp: 52,
    fanspeed: 70, fanrpm: 6967, errorPercentage: 0,
    sharesAccepted: 35, sharesRejected: 0, uptimeSeconds: 960,
    stratumURL: 'public-pool.example', isUsingFallbackStratum: 0,
    responseTime: 109, frequency: 625, coreVoltage: 1150,
    thermalControlMode: 'curve', requestedFanPercent: 68, appliedFanPercent: 70,
    hysteresisHolding: 1, emergencyOverrideActive: 0, overheat_mode: 0, miningPaused: false,
    ...overrides,
  };
}

function supraHexDevice(overrides: Partial<FleetDevice> = {}): FleetDevice {
  return {
    IP: '10.0.0.30', nxReachable: true, nxLastSeenMs: Date.now(),
    hostname: 'suprahex-lab', deviceModel: 'Supra', ASICModel: 'BM1368', boardVersion: '702',
    swarmColor: 'blue', asicCount: 1,
    version: 'v2.9.0', axeOSVersion: 'v2.9.0',
    hashRate: 700, power: 15.1, temp: 60, sharesAccepted: 100, sharesRejected: 1,
    uptimeSeconds: 5000, stratumURL: 'solo.pool.example', isUsingFallbackStratum: 0,
    frequency: 550,
    ...overrides,
  };
}

describe('SwarmComponent (Fleet Command Center, Phase 2I)', () => {
  let component: SwarmComponent;
  let fixture: ComponentFixture<SwarmComponent>;
  let httpMock: HttpTestingController;

  beforeEach(() => {
    window.localStorage.clear();
    // An empty stored fleet prevents ngOnInit from launching the /24 network
    // scan (253 probes) into the HTTP testing controller.
    window.localStorage.setItem('SWARM_DATA', '[]');
    TestBed.configureTestingModule({
      declarations: [SwarmComponent, ModalComponent, DateAgoPipe, DiffSuffixPipe, HashSuffixPipe],
      imports: [
        FileUploadModule, InputGroupModule, InputTextModule, DropdownModule,
        ButtonModule, TooltipModule, SliderModule, FormsModule, ReactiveFormsModule,
      ],
      providers: [provideHttpClient(), provideHttpClientTesting(), provideToastr()]
    });
    httpMock = TestBed.inject(HttpTestingController);
    fixture = TestBed.createComponent(SwarmComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  function loadFleet(devices: FleetDevice[]): void {
    component.swarm = devices;
    component.hasCompletedFirstLoad = true;
    fixture.detectChanges();
  }

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  describe('summary bar (Stage 2)', () => {
    it('shows honest fleet totals with online/offline/pending split', () => {
      loadFleet([
        gammaDevice(),
        supraHexDevice(),
        gammaDevice({ IP: '10.0.0.11', hostname: 'gamma-02', nxReachable: false }),
      ]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Fleet Command Center');
      expect(text).toContain('2 online');
      expect(text).toContain('1 offline');
      expect(text).toContain('Fleet Efficiency');
      expect(text).toContain('attention / critical');
    });

    it('never invents efficiency without both power and hashrate', () => {
      loadFleet([gammaDevice({ power: 0 })]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('needs power + hashrate');
    });
  });

  describe('classification and health rendering (Stages 3/6)', () => {
    it('labels NeuralAxe, AxeOS and unsupported devices distinctly', () => {
      loadFleet([gammaDevice(), supraHexDevice(), gammaDevice({ IP: '10.0.0.12', hostname: 'plain-gamma', productName: undefined })]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('NeuralAxe');
      expect(text).toContain('AxeOS');
      expect(text).toContain('Board 702');
    });

    it('renders health states with explanations available', () => {
      loadFleet([
        gammaDevice(),
        gammaDevice({ IP: 'a', hostname: 'hot-gamma', temp: 66 }),
        gammaDevice({ IP: 'b', hostname: 'crit-gamma', overheat_mode: 1 }),
        gammaDevice({ IP: 'c', hostname: 'off-gamma', nxReachable: false }),
      ]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Healthy');
      expect(text).toContain('Attention');
      expect(text).toContain('Critical');
      expect(text).toContain('Offline');
      expect(component.healthTooltip(component.swarm[1])).toContain('66');
    });

    it('offline devices show last-seen age, not stale values as live', () => {
      loadFleet([gammaDevice({ nxReachable: false, nxLastSeenMs: Date.now() - 120_000, hashRate: 0, temp: 0 })]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('seen 2 min ago');
    });
  });

  describe('filters (Stage 7)', () => {
    it('filters by search text, health and classification and can clear', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.setFilter('text', 'suprahex');
      fixture.detectChanges();
      expect(component.filteredSwarm.length).toBe(1);

      component.setFilter('text', '');
      component.setFilter('classification', 'neuralaxe');
      fixture.detectChanges();
      expect(component.filteredSwarm.length).toBe(1);
      expect(component.filteredSwarm[0].hostname).toBe('gamma-01');

      component.clearFilters();
      fixture.detectChanges();
      expect(component.filteredSwarm.length).toBe(2);
    });

    it('shows the filtered-empty state with a clear action', () => {
      loadFleet([gammaDevice()]);
      component.setFilter('text', 'nomatch');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('No devices match the current filters');
    });

    it('persists filters using the existing localStorage convention', () => {
      loadFleet([gammaDevice()]);
      component.setFilter('health', 'critical');
      const stored = JSON.parse(window.localStorage.getItem('FLEET_FILTERS') ?? '{}');
      expect(stored.health).toBe('critical');
    });
  });

  describe('detail drawer (Stage 5)', () => {
    it('opens with device sections and closes non-destructively', () => {
      loadFleet([gammaDevice()]);
      component.openDetail(component.swarm[0]);
      fixture.detectChanges();

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Overview');
      expect(text).toContain('Performance');
      expect(text).toContain('Thermal');
      expect(text).toContain('Pool & Network');
      expect(text).toContain('Software');
      expect(text).toContain('Tuning');
      expect(text).toContain('Req / Applied Fan');
      expect(text).toContain('68 % / 70 %');

      component.closeDetail();
      fixture.detectChanges();
      expect(component.detailDevice).toBeNull();
      expect(component.swarm.length).toBe(1);
    });

    it('shows the explicit compatibility notice for board 702 and omits unreported thermal data', () => {
      loadFleet([supraHexDevice()]);
      component.openDetail(component.swarm[0]);
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('not a NeuralAxe release target');
      expect(text).toContain('must not be installed');
      expect(text).toContain('not reported by this device'); // thermal mode absent, stated
    });
  });

  describe('safe actions (Stage 10)', () => {
    it('restart posts nothing until explicitly confirmed', () => {
      loadFleet([gammaDevice()]);
      component.confirmRestart(component.swarm[0]);
      fixture.detectChanges();
      httpMock.expectNone(`http://10.0.0.10/api/system/restart`);

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Restart device?');
      expect(text).toContain('No settings are changed');

      component.executeRestart();
      const req = httpMock.expectOne(`http://10.0.0.10/api/system/restart`);
      expect(req.request.method).toBe('POST');
      req.flush({ message: 'ok' });
    });

    it('cancelling a restart posts nothing', () => {
      loadFleet([gammaDevice()]);
      component.confirmRestart(component.swarm[0]);
      component.cancelRestart();
      httpMock.expectNone(`http://10.0.0.10/api/system/restart`);
      expect(component.pendingRestart).toBeNull();
    });

    it('remove requires its own confirmation and only edits the local list', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.confirmRemove(component.swarm[0]);
      fixture.detectChanges();
      expect(component.swarm.length).toBe(2); // nothing removed yet

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Remove from fleet list?');
      expect(text).toContain('keeps mining and is untouched');

      component.executeRemove();
      expect(component.swarm.length).toBe(1);
      httpMock.expectNone(() => true); // remove never talks to the device
    });

    it('row click opens details only — no silent device action', () => {
      loadFleet([gammaDevice()]);
      const row: HTMLElement | null = fixture.nativeElement.querySelector('.nx-fleet-row');
      row?.click();
      fixture.detectChanges();
      expect(component.detailDevice).not.toBeNull();
      httpMock.expectNone(() => true);
    });
  });

  describe('privacy (Stage 12)', () => {
    it('masks hostname, IP and pool in the table, cards and drawer', () => {
      loadFleet([gammaDevice()]);

      const tableMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-table [sensitive-data]');
      expect(tableMasked.length).toBeGreaterThanOrEqual(3); // hostname, IP, pool

      component.toggleGridView(true);
      fixture.detectChanges();
      const cardMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-card [sensitive-data]');
      expect(cardMasked.length).toBeGreaterThanOrEqual(2); // hostname, pool

      component.openDetail(component.swarm[0]);
      fixture.detectChanges();
      const drawerMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-drawer [sensitive-data]');
      expect(drawerMasked.length).toBeGreaterThanOrEqual(3); // hostname, IP, pool
    });
  });

  describe('states (Stage 9)', () => {
    it('shows the empty state after a completed scan finds nothing', () => {
      component.swarm = [];
      component.hasCompletedFirstLoad = true;
      component.scanning = false;
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('No miners found on this subnet');
    });

    it('shows the scanning state while discovering', () => {
      component.swarm = [];
      component.scanning = true;
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Scanning the local network');
    });
  });
});
