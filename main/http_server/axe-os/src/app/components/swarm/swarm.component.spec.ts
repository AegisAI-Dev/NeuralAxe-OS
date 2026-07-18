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

/** A stock Gamma 601 running AxeOS (no NeuralAxe identity) = compatible. */
function axeosCompatibleDevice(overrides: Partial<FleetDevice> = {}): FleetDevice {
  return {
    IP: '10.0.0.40', nxReachable: true, nxLastSeenMs: Date.now(),
    hostname: 'stock-gamma', deviceModel: 'Gamma', ASICModel: 'BM1370', boardVersion: '601',
    swarmColor: 'green', asicCount: 1,
    version: 'v2.9.0', axeOSVersion: 'v2.9.0',
    hashRate: 1150, power: 21.2, temp: 59, sharesAccepted: 2200, sharesRejected: 8,
    uptimeSeconds: 41000, stratumURL: 'solo.pool.example', isUsingFallbackStratum: 0,
    frequency: 600,
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
    it('shows honest fleet totals with online/offline/pending split and class chips', () => {
      loadFleet([
        gammaDevice(),
        supraHexDevice(),
        gammaDevice({ IP: '10.0.0.11', hostname: 'gamma-02', nxReachable: false }),
      ]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Fleet Command Center');
      expect(text).toContain('2 / 3');
      expect(text).toContain('1 offline');
      expect(text).toContain('Fleet Efficiency');
      expect(text).toContain('attention / critical');
      // secondary classification strip (2I.1 hierarchy)
      expect(text).toContain('2 NeuralAxe managed');
      expect(text).toContain('1 unsupported target');
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

    it('renders health states with explanations available (table view labels)', () => {
      loadFleet([
        gammaDevice(),
        gammaDevice({ IP: 'a', hostname: 'hot-gamma', temp: 66 }),
        gammaDevice({ IP: 'b', hostname: 'crit-gamma', overheat_mode: 1 }),
        gammaDevice({ IP: 'c', hostname: 'off-gamma', nxReachable: false }),
      ]);
      component.setViewMode('table');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Healthy');
      expect(text).toContain('Attention');
      expect(text).toContain('Critical');
      expect(text).toContain('Offline');
      expect(component.healthTooltip(component.swarm[1])).toContain('66');
      component.setViewMode('command'); // restore the persisted default
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

  describe('device workspace (Stages 2/7)', () => {
    it('shows the selected device persistently with tabs and health explanation', () => {
      loadFleet([gammaDevice()]);
      component.ensureSelection();
      fixture.detectChanges();

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      // tab bar + overview content (default tab)
      expect(text).toContain('Overview');
      expect(text).toContain('Thermal');
      expect(text).toContain('Tuning');
      expect(text).toContain('Active Pool');
      expect(text).toContain('Firmware pair');
      expect(text).toContain('Healthy.');

      component.setWorkspaceTab('thermal');
      fixture.detectChanges();
      const thermalText = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(thermalText).toContain('Req / Applied Fan');
      expect(thermalText).toContain('68 % / 70 %');
    });

    it('the overlay drawer reuses the same workspace and closes non-destructively', () => {
      loadFleet([gammaDevice()]);
      component.openDetail(component.swarm[0]);
      fixture.detectChanges();

      expect(fixture.nativeElement.querySelector('.nx-fleet-drawer')).toBeTruthy();
      component.closeDetail();
      fixture.detectChanges();
      expect(component.detailDevice).toBeNull();
      expect(component.swarm.length).toBe(1);
    });

    it('shows the explicit compatibility notice for board 702 and omits unreported thermal data', () => {
      loadFleet([supraHexDevice()]); // board 702 = unsupported target
      component.ensureSelection();
      component.setWorkspaceTab('thermal');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('not a NeuralAxe release target');
      expect(text).toContain('must not be installed');
      expect(text).toContain('not reported by this device'); // thermal mode absent, stated
    });

    it('flags reduced telemetry for an AxeOS-compatible device (board 601, no NeuralAxe identity)', () => {
      loadFleet([axeosCompatibleDevice()]);
      component.ensureSelection();
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('AxeOS'); // classification
      expect(text).toContain('reduced telemetry');
    });
  });

  describe('master-detail selection (2I.1)', () => {
    it('selects the first visible device by default and selection is click-only', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.ensureSelection();
      fixture.detectChanges();
      expect(component.selectedDevice?.hostname).toBe('gamma-01');

      const items = fixture.nativeElement.querySelectorAll('.nx-fleet-nav-item');
      expect(items.length).toBe(2);
      (items[1] as HTMLElement).click();
      fixture.detectChanges();
      expect(component.selectedDevice?.hostname).toBe('suprahex-lab');
      httpMock.expectNone(() => true); // selection never talks to a device
    });

    it('keyboard arrows move the selection across visible devices', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.ensureSelection();
      component.selectAdjacent(1);
      expect(component.selectedDevice?.hostname).toBe('suprahex-lab');
      component.selectAdjacent(1); // clamped at the end
      expect(component.selectedDevice?.hostname).toBe('suprahex-lab');
      component.selectAdjacent(-1);
      expect(component.selectedDevice?.hostname).toBe('gamma-01');
    });

    it('reselects the first visible device when the selection is filtered out', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.ensureSelection();
      component.selectDevice(component.swarm[1]); // suprahex
      component.setFilter('classification', 'neuralaxe');
      fixture.detectChanges();
      expect(component.selectedDevice?.hostname).toBe('gamma-01');
    });

    it('shows an explicit empty selection when everything is filtered out', () => {
      loadFleet([gammaDevice()]);
      component.ensureSelection();
      component.setFilter('text', 'nomatch');
      fixture.detectChanges();
      expect(component.selectedDevice).toBeNull();
    });

    it('view mode toggles to the optional table and persists', () => {
      loadFleet([gammaDevice()]);
      expect(component.viewMode).toBe('command');
      expect(fixture.nativeElement.querySelector('.nx-fleet-split')).toBeTruthy();
      expect(fixture.nativeElement.querySelector('.nx-fleet-table')).toBeFalsy();

      component.setViewMode('table');
      fixture.detectChanges();
      expect(fixture.nativeElement.querySelector('.nx-fleet-table')).toBeTruthy();
      expect(fixture.nativeElement.querySelector('.nx-fleet-split')).toBeFalsy();
      expect(window.localStorage.getItem('FLEET_VIEW_MODE')).toBe('table');
    });

    it('renders the visible result count', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.setFilter('text', 'gamma');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('1 of 2');
    });

    it('renders the warming-up share note for the real pilot startup sample', () => {
      loadFleet([gammaDevice({ sharesAccepted: 25, sharesRejected: 1, uptimeSeconds: 120 })]);
      component.ensureSelection();
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Healthy.');
      expect(text).toContain('warming up');
    });
  });

  describe('action menu (2I.1 Stage 6)', () => {
    it('consolidates actions behind a labeled menu and never fires on open', () => {
      loadFleet([gammaDevice()]);
      component.ensureSelection();
      fixture.detectChanges();

      component.toggleActionMenu();
      fixture.detectChanges();
      const menu: HTMLElement | null = fixture.nativeElement.querySelector('.nx-fleet-menu');
      expect(menu).toBeTruthy();
      expect(menu!.textContent).toContain('Pause Mining');
      expect(menu!.textContent).toContain('Restart…');
      expect(menu!.textContent).toContain('Identify');
      expect(menu!.textContent).toContain('Remove from list…');
      httpMock.expectNone(() => true); // opening the menu performs nothing

      // Restart from the menu still goes through the confirmation
      component.actionMenuOpen = false;
      component.confirmRestart(component.swarm[0]);
      httpMock.expectNone(`http://10.0.0.10/api/system/restart`);
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

    it('table row click opens details only — no silent device action', () => {
      loadFleet([gammaDevice()]);
      component.setViewMode('table');
      fixture.detectChanges();
      const row: HTMLElement | null = fixture.nativeElement.querySelector('.nx-fleet-row');
      row?.click();
      fixture.detectChanges();
      expect(component.detailDevice).not.toBeNull();
      httpMock.expectNone(() => true);
      component.setViewMode('command');
    });
  });

  describe('privacy (Stage 12)', () => {
    it('masks hostname and pool in the navigator, workspace, table, cards and drawer', () => {
      loadFleet([gammaDevice()]);
      component.ensureSelection();
      fixture.detectChanges();

      const navMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-nav [sensitive-data]');
      expect(navMasked.length).toBeGreaterThanOrEqual(1); // hostname
      const wsMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-workspace [sensitive-data]');
      expect(wsMasked.length).toBeGreaterThanOrEqual(2); // hostname, pool

      component.setViewMode('table');
      fixture.detectChanges();
      const tableMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-table [sensitive-data]');
      expect(tableMasked.length).toBeGreaterThanOrEqual(1); // hostname
      component.setViewMode('command');
      fixture.detectChanges();

      const cardMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-card [sensitive-data]');
      expect(cardMasked.length).toBeGreaterThanOrEqual(2); // hostname, pool (cards always rendered)

      component.openDetail(component.swarm[0]);
      fixture.detectChanges();
      const drawerMasked = fixture.nativeElement.querySelectorAll('.nx-fleet-drawer [sensitive-data]');
      expect(drawerMasked.length).toBeGreaterThanOrEqual(2); // hostname, pool
    });
  });

  describe('selection styling semantics (2I.2 Stage 4)', () => {
    function navItems(): HTMLElement[] {
      return Array.from(fixture.nativeElement.querySelectorAll('.nx-fleet-nav-item'));
    }
    function selectedItem(): HTMLElement | undefined {
      return navItems().find(el => el.classList.contains('nx-fleet-nav-on'));
    }

    it('a selected Healthy device is marked selected but NOT assigned Critical styling', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.selectDevice(component.swarm[0]); // healthy gamma
      fixture.detectChanges();

      const selected = selectedItem();
      expect(selected).toBeTruthy();
      expect(selected!.classList.contains('nx-fleet-nav-critical')).toBeFalse();
      expect(selected!.classList.contains('nx-fleet-nav-offline')).toBeFalse();
      // health dot keeps the fixed green semantic, never the accent
      expect(selected!.querySelector('.nx-health-ok')).toBeTruthy();
      // selection carries a structural (non-colour) indicator too
      expect(selected!.querySelector('.nx-fleet-nav-check')).toBeTruthy();
    });

    it('a selected unsupported-but-Healthy device (board 702) stays Healthy, not Critical', () => {
      loadFleet([supraHexDevice()]);
      component.ensureSelection();
      fixture.detectChanges();
      const selected = selectedItem();
      expect(selected!.classList.contains('nx-fleet-nav-critical')).toBeFalse();
      expect(selected!.querySelector('.nx-health-ok')).toBeTruthy();
      expect(component.health(component.swarm[0]).state).toBe('healthy');
      expect(component.deviceClass(component.swarm[0]).kind).toBe('unsupported');
    });

    it('a selected Critical device still presents its Critical health state', () => {
      loadFleet([gammaDevice({ overheat_mode: 1, temp: 74 })]);
      component.ensureSelection();
      fixture.detectChanges();
      const selected = selectedItem();
      expect(selected).toBeTruthy();
      // selection never masks Critical — both classes coexist
      expect(selected!.classList.contains('nx-fleet-nav-critical')).toBeTrue();
      expect(selected!.querySelector('.nx-health-err')).toBeTruthy();
      const pill = fixture.nativeElement.querySelector('.nx-fleet-ws-healthpill');
      expect(pill.textContent).toContain('Critical');
      expect(pill.classList.contains('nx-pill-err')).toBeTrue();
    });

    it('selection is class-driven and every item stays focusable independently', () => {
      loadFleet([gammaDevice(), supraHexDevice()]);
      component.selectDevice(component.swarm[1]);
      fixture.detectChanges();
      const items = navItems();
      // exactly one persistent selection...
      expect(items.filter(el => el.classList.contains('nx-fleet-nav-on')).length).toBe(1);
      // ...while focus (tabindex) is available on every item, separate from selection
      expect(items.every(el => el.getAttribute('tabindex') === '0')).toBeTrue();
      expect(component.selectedIp).toBe(component.swarm[1].IP);
    });
  });

  describe('fleet shares + workspace polish (2I.2 Stages 2/3/5)', () => {
    function noCounters(base: FleetDevice): FleetDevice {
      const d = { ...base };
      delete (d as any).sharesAccepted;
      delete (d as any).sharesRejected;
      return d;
    }

    it('renders the Fleet Shares summary tile with honest coverage', () => {
      loadFleet([
        gammaDevice({ sharesAccepted: 1000, sharesRejected: 10 }),
        gammaDevice({ IP: '10.0.0.11', hostname: 'gamma-02', sharesAccepted: 2000, sharesRejected: 20 }),
      ]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Fleet Shares');
      expect(text).toContain('from 2 of 2 reporting');
      expect(component.shares.accepted).toBe(3000);
      expect(component.shares.rejected).toBe(30);
    });

    it('the Fleet Shares tile shows an honest empty state when no device reports counters', () => {
      loadFleet([noCounters(axeosCompatibleDevice())]);
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('no live counters reported');
      expect(component.shares.hasData).toBeFalse();
    });

    it('a low-confidence startup sample never flags the shares tile as attention', () => {
      loadFleet([gammaDevice({ sharesAccepted: 25, sharesRejected: 1, uptimeSeconds: 120 })]);
      expect(component.shareSeverity(component.shares)).toBe('ok');
    });

    it('the Mining tab breaks shares into accepted/rejected/total/reject-rate with reset semantics', () => {
      loadFleet([gammaDevice({ sharesAccepted: 15234, sharesRejected: 42 })]);
      component.ensureSelection();
      component.setWorkspaceTab('mining');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Shares Accepted');
      expect(text).toContain('Shares Rejected');
      expect(text).toContain('Total Shares');
      expect(text).toContain('Reject Rate');
      expect(text).toContain('reset after a restart');
    });

    it('the Mining tab states clearly when a device does not report share counters', () => {
      loadFleet([noCounters(axeosCompatibleDevice())]);
      component.ensureSelection();
      component.setWorkspaceTab('mining');
      fixture.detectChanges();
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('Not reported by this device');
    });

    it('the workspace header shows a fixed-semantics Healthy badge', () => {
      loadFleet([gammaDevice()]);
      component.ensureSelection();
      fixture.detectChanges();
      const pill = fixture.nativeElement.querySelector('.nx-fleet-ws-healthpill');
      expect(pill).toBeTruthy();
      expect(pill.textContent).toContain('Healthy');
      expect(pill.classList.contains('nx-pill-ok')).toBeTrue();
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
