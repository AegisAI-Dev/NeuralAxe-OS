import { HttpClient, HttpErrorResponse } from '@angular/common/http';
import { Component, OnDestroy, OnInit, ViewChild, HostListener } from '@angular/core';
import { FormBuilder, FormGroup, Validators, FormControl } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { forkJoin, catchError, from, map, mergeMap, of, take, timeout, toArray, Observable, Subscription } from 'rxjs';
import { LocalStorageService } from 'src/app/local-storage.service';
import { LayoutService } from "../../layout/service/app.layout.service";
import { SystemApiService } from 'src/app/services/system.service';
import { ModalComponent } from '../modal/modal.component';
import { DeckFmt } from 'src/app/components/command-deck/deck-format';
import {
  DEFAULT_FLEET_FILTERS,
  FleetClassification,
  FleetDevice,
  FleetFilters,
  FleetHealth,
  FleetSortField,
  FleetSummary,
  activePoolHost,
  classifyDevice,
  compareDevices,
  deviceEfficiency,
  deviceHealth,
  deviceOnline,
  filterDevices,
  fleetSummary,
  lastSeenText,
  pairMismatch,
} from './fleet-intel';

const SWARM_DATA = 'SWARM_DATA';
const SWARM_REFRESH_TIME = 'SWARM_REFRESH_TIME';
const SWARM_SORTING = 'SWARM_SORTING';
const SWARM_GRID_VIEW = 'SWARM_GRID_VIEW';
const FLEET_FILTERS = 'FLEET_FILTERS';
const FLEET_DENSITY = 'FLEET_DENSITY';

type SwarmDevice = { IP: string; ASICModel: string; deviceModel: string; swarmColor: string; asicCount: number; [key: string]: any };

@Component({
  selector: 'app-swarm',
  templateUrl: './swarm.component.html',
  styleUrls: ['./swarm.component.scss']
})
export class SwarmComponent implements OnInit, OnDestroy {

  @ViewChild('editModal') modalComponent!: ModalComponent;
  @ViewChild('restartModal') restartModal?: ModalComponent;
  @ViewChild('removeModal') removeModal?: ModalComponent;

  public swarm: any[] = [];

  public selectedAxeOs: any = null;

  public form: FormGroup;

  public scanning = false;

  public refreshIntervalRef!: number;
  public refreshIntervalTime = 30;
  public refreshTimeSet = 30;

  public isRefreshing = false;
  /** Set once the first scan/refresh cycle has completed (Stage 9 states). */
  public hasCompletedFirstLoad = false;

  public refreshIntervalControl: FormControl;

  public gridView: boolean;
  public selectedSort: { sortField: FleetSortField; sortDirection: 'asc' | 'desc' };

  public staticMenuDesktopInactive: boolean;
  private staticMenuDesktopSubscription!: Subscription;

  // ---- Fleet Command Center state (2I) ----
  public filters: FleetFilters = { ...DEFAULT_FLEET_FILTERS };
  public density: 'comfortable' | 'compact';
  /** Device shown in the detail drawer; null = closed. Typed `any` at the
   * template boundary (house style for fleet rows) — the derivation logic
   * itself is strictly typed and tested in fleet-intel.ts. */
  public detailDevice: any | null = null;
  /** Devices awaiting action confirmation; null = no dialog. */
  public pendingRestart: any | null = null;
  public pendingRemove: any | null = null;

  public readonly fmt = DeckFmt;

  @HostListener('document:keydown.esc', ['$event'])
  onEscKey() {
    if (this.pendingRemove || this.pendingRestart) {
      this.pendingRemove = null;
      this.pendingRestart = null;
    } else if (this.detailDevice) {
      this.detailDevice = null;
    } else if (this.filters.text) {
      this.filters = { ...this.filters, text: '' };
      this.persistFilters();
    }
  }

  constructor(
    private fb: FormBuilder,
    private toastr: ToastrService,
    private localStorageService: LocalStorageService,
    public layoutService: LayoutService,
    private systemService: SystemApiService,
    private httpClient: HttpClient
  ) {

    this.form = this.fb.group({
      manualAddIp: [null, [Validators.required, Validators.pattern('(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)')]]
    });

    this.gridView = this.localStorageService.getBool(SWARM_GRID_VIEW);
    this.density = this.localStorageService.getItem(FLEET_DENSITY) === 'compact' ? 'compact' : 'comfortable';
    const storedFilters = this.localStorageService.getObject(FLEET_FILTERS);
    if (storedFilters) {
      this.filters = { ...DEFAULT_FLEET_FILTERS, ...storedFilters };
    }

    const storedRefreshTime = this.localStorageService.getNumber(SWARM_REFRESH_TIME) ?? 30;
    this.refreshIntervalTime = storedRefreshTime;
    this.refreshTimeSet = storedRefreshTime;
    this.refreshIntervalControl = new FormControl(storedRefreshTime);

    this.refreshIntervalControl.valueChanges.subscribe(value => {
      this.refreshIntervalTime = value;
      this.refreshTimeSet = value;
      this.localStorageService.setNumber(SWARM_REFRESH_TIME, value);
    });

    this.selectedSort = this.localStorageService.getObject(SWARM_SORTING) ?? {
      sortField: 'IP' as FleetSortField,
      sortDirection: 'asc'
    };

    this.staticMenuDesktopInactive = this.layoutService.state.staticMenuDesktopInactive;
  }

  ngOnInit(): void {
    const swarmData = this.localStorageService.getObject(SWARM_DATA);

    if (swarmData == null) {
      this.scanNetwork();
    } else {
      this.swarm = swarmData;
      this.refreshList(true);
    }

    this.staticMenuDesktopSubscription = this.layoutService.getStaticMenuDesktopInactive$()
      .subscribe(inactive => {
        this.staticMenuDesktopInactive = inactive;
      });

    this.refreshIntervalRef = window.setInterval(() => {
      if (!this.scanning && !this.isRefreshing && this.swarm.length) {
        this.refreshIntervalTime--;
        if (this.refreshIntervalTime <= 0) {
          this.refreshList(false);
        }
      }
    }, 1000);
  }

  ngOnDestroy(): void {
    this.staticMenuDesktopSubscription.unsubscribe();
    window.clearInterval(this.refreshIntervalRef);
    this.form.reset();
  }

  private ipToInt(ip: string): number {
    return ip.split('.').reduce((acc, octet) => (acc << 8) + parseInt(octet, 10), 0) >>> 0;
  }

  private intToIp(int: number): string {
    return `${(int >>> 24) & 255}.${(int >>> 16) & 255}.${(int >>> 8) & 255}.${int & 255}`;
  }

  private calculateIpRange(ip: string, netmask: string): { start: number, end: number } {
    const ipInt = this.ipToInt(ip);
    const netmaskInt = this.ipToInt(netmask);
    const network = ipInt & netmaskInt;
    const broadcast = network | ~netmaskInt;
    return { start: network + 1, end: broadcast - 1 };
  }

  scanNetwork() {
    this.scanning = true;

    const { start, end } = this.calculateIpRange(window.location.hostname, '255.255.255.0');
    const ips = Array.from({ length: end - start + 1 }, (_, i) => this.intToIp(start + i));
    this.getAllDeviceInfo(ips, () => of(null)).subscribe({
      next: (result) => {
        // Filter out null items first
        const validResults = result.filter((item): item is SwarmDevice => item !== null);
        // Merge new results with existing swarm entries
        const existingIps = new Set(this.swarm.map(item => item.IP));
        const newItems = validResults.filter(item => !existingIps.has(item.IP));
        this.swarm = [...this.swarm, ...newItems];
        this.sortSwarm();
        this.localStorageService.setObject(SWARM_DATA, this.swarm);
      },
      complete: () => {
        this.scanning = false;
        this.hasCompletedFirstLoad = true;
        this.refreshIntervalTime = this.refreshTimeSet;
      }
    });
  }

  private getAllDeviceInfo(ips: string[], errorHandler: (error: any, ip: string) => Observable<SwarmDevice[] | null>, fetchAsic: boolean = true) {
    return from(ips).pipe(
      mergeMap(IP => forkJoin({
        info: this.httpClient.get<any>(`http://${IP}/api/system/info`),
        asic: fetchAsic ? this.httpClient.get<any>(`http://${IP}/api/system/asic`).pipe(catchError(() => of({}))) : of({})
      }).pipe(
        map(({ info, asic }) => {
          const existingDevice = this.swarm.find(device => device.IP === IP) || {};
          return this.mergeDeviceData(IP, existingDevice, info, asic);
        }),
        timeout(5000),
        catchError(error => errorHandler(error, IP))
      ),
        128
      ),
      toArray()
    ).pipe(take(1));
  }

  public add() {
    const IP = this.form.value.manualAddIp;

    // Check if IP already exists
    if (this.swarm.some(item => item.IP === IP)) {
      this.toastr.warning('Device already added to the swarm.', `Device at ${IP}`);
      return;
    }

    forkJoin({
      info: this.httpClient.get<any>(`http://${IP}/api/system/info`),
      asic: this.httpClient.get<any>(`http://${IP}/api/system/asic`).pipe(catchError(() => of({})))
    }).pipe(
      timeout(5000),
      catchError(error => {
        // Manual add keeps its explicit failure toast (the user is waiting).
        const errorMessage = error?.message || error?.statusText || error?.toString() || 'Unknown error';
        this.toastr.error(`Failed to get info: ${errorMessage}`, `Device at ${IP}`);
        return of(null);
      })
    ).subscribe((result: any) => {
      if (!result || !result.info?.ASICModel || !result.asic?.ASICModel) {
        return;
      }
      this.swarm.push(this.mergeDeviceData(IP, {}, result.info, result.asic));
      this.sortSwarm();
      this.localStorageService.setObject(SWARM_DATA, this.swarm);
    });
  }

  public edit(axe: any) {
    this.selectedAxeOs = axe;
    this.modalComponent.isVisible = true;
  }

  public postAction(axe: any, action: string) {
    this.httpClient.post(`http://${axe.IP}/api/system/${action}`, {}, { responseType: 'json' }).pipe(
      timeout(800),
      catchError(error => {
        if ((action === 'restart' || action === 'identify') && (error.status === 200 || error.status === 0 || error.name === 'HttpErrorResponse' || error.statusText === 'Unknown Error')) {
          if (action === 'restart') {
            return of({ message: 'System will restarted shortly' });
          } else {
            return of({ message: 'Identify signal sent - device should say "Hi!"' });
          }
        }
        let errorMsg = `Failed to ${action} device`;
        if (error.name === 'TimeoutError') {
          errorMsg = 'Request timed out';
        } else if (error.message) {
          errorMsg += `: ${error.message}`;
        }
        this.toastr.error(errorMsg, `Device at ${axe.IP}`);
        return of(null);
      })
    ).subscribe((res: any) => {
      if (res !== null) {
        this.toastr.success(res.message, `Device at ${axe.IP}`);
        this.refreshList(false);
      }
    });
  }

  // ---- confirmed actions (Stage 10): destructive/remote actions never run
  // from the first click; the dialogs state exactly what will happen. ----

  public confirmRestart(axe: FleetDevice): void {
    this.pendingRestart = axe;
    if (this.restartModal) {
      this.restartModal.isVisible = true;
    }
  }

  public cancelRestart(): void {
    this.pendingRestart = null;
    if (this.restartModal) {
      this.restartModal.isVisible = false;
    }
  }

  public executeRestart(): void {
    if (!this.pendingRestart) {
      return;
    }
    const device = this.pendingRestart;
    this.cancelRestart();
    this.postAction(device, 'restart');
  }

  public confirmRemove(axe: FleetDevice): void {
    this.pendingRemove = axe;
    if (this.removeModal) {
      this.removeModal.isVisible = true;
    }
  }

  public cancelRemove(): void {
    this.pendingRemove = null;
    if (this.removeModal) {
      this.removeModal.isVisible = false;
    }
  }

  public executeRemove(): void {
    if (!this.pendingRemove) {
      return;
    }
    const device = this.pendingRemove;
    this.cancelRemove();
    if (this.detailDevice?.IP === device.IP) {
      this.detailDevice = null;
    }
    this.remove(device);
  }

  public remove(axeOs: any) {
    this.swarm = this.swarm.filter(axe => axe.IP !== axeOs.IP);
    this.localStorageService.setObject(SWARM_DATA, this.swarm);
  }

  /**
   * Refresh failures mark the device unreachable (visible as the Offline
   * state with its last-seen age) instead of toasting on every cycle; the
   * previous telemetry zeroing is preserved so nothing stale renders as live.
   */
  public refreshErrorHandler = (error: any, ip: string) => {
    const existingDevice = this.swarm.find(axeOs => axeOs.IP === ip);
    return of({
      ...existingDevice,
      hashRate: 0,
      sharesAccepted: 0,
      power: 0,
      voltage: 0,
      temp: 0,
      bestDiff: 0,
      version: 0,
      uptimeSeconds: 0,
      poolDifficulty: 0,
      nxReachable: false,
    });
  };

  public refreshList(fetchAsic: boolean = true) {
    if (this.scanning) {
      return;
    }

    this.refreshIntervalTime = this.refreshTimeSet;
    const ips = this.swarm.map(axeOs => axeOs.IP);
    this.isRefreshing = true;

    this.getAllDeviceInfo(ips, this.refreshErrorHandler, fetchAsic).subscribe({
      next: (result) => {
        this.swarm = result;
        this.sortSwarm();
        this.localStorageService.setObject(SWARM_DATA, this.swarm);
        this.isRefreshing = false;
        this.hasCompletedFirstLoad = true;
        // Keep the open drawer bound to the fresh object for its device.
        if (this.detailDevice) {
          this.detailDevice = this.swarm.find(axe => axe.IP === this.detailDevice!.IP) ?? null;
        }
      },
      complete: () => {
        this.isRefreshing = false;
        this.hasCompletedFirstLoad = true;
      }
    });
  }

  sortBy(sortField: FleetSortField, sortDirection?: 'asc' | 'desc' | undefined) {
    if (sortDirection) {
      this.selectedSort = { sortField, sortDirection };
    } else if (this.selectedSort.sortField === sortField) {
      this.selectedSort = { sortField, sortDirection: this.selectedSort.sortDirection === 'asc' ? 'desc' : 'asc' };
    } else {
      this.selectedSort = { sortField, sortDirection: 'asc' };
    }

    this.localStorageService.setObject(SWARM_SORTING, this.selectedSort);
    this.sortSwarm();
  }

  private sortSwarm() {
    this.swarm.sort((a, b) => compareDevices(a, b, this.selectedSort.sortField, this.selectedSort.sortDirection));
  }

  private deriveDeviceModel(data: any): string {
    if (data.boardVersion && data.boardVersion.length > 1) {
      if (data.boardVersion[0] == "1" || data.boardVersion == "2.2") return "Max";
      if (data.boardVersion[0] == "2" || data.boardVersion == "0.11") return "Ultra";
      if (data.boardVersion[0] == "3") return "UltraHex";
      if (data.boardVersion[0] == "4") return "Supra";
      if (data.boardVersion[0] == "6") return "Gamma";
      if (data.boardVersion[0] == "8") return "GammaTurbo";
    }
    return 'Other';
  }

  private deriveSwarmColor(deviceModel: string): string {
    switch (deviceModel) {
      case 'Max':        return 'red';
      case 'Ultra':      return 'purple';
      case 'Supra':      return 'blue';
      case 'UltraHex':   return 'orange';
      case 'Gamma':      return 'green';
      case 'GammaTurbo': return 'cyan';
      default:           return 'gray';
    }
  }

  private mergeDeviceData(IP: string, existing: Partial<SwarmDevice>, info: any, asic: any): SwarmDevice {
    const merged: any = {
      IP,
      ...existing,
      power_fault: null,
      overheat_mode: null,
      isUsingFallbackStratum: null,
      blockFound: null,
      ...info,
      ...asic,
      // Reachability bookkeeping (2I): this merge only runs on a successful
      // response, so the device is online right now.
      nxReachable: true,
      nxLastSeenMs: Date.now(),
    };

    merged.deviceModel = merged.deviceModel || this.deriveDeviceModel(merged);
    merged.swarmColor = merged.swarmColor || this.deriveSwarmColor(merged.deviceModel);
    merged.asicCount = merged.asicCount || 1;

    merged.poolDifficulty = merged.poolDifficulty || info.stratumDiff;
    merged.hashRate = merged.hashRate || info.hashRate_10m;
    if (typeof merged.bestDiff === 'string') merged.bestDiff = this.parseSuffixString(merged.bestDiff);
    if (typeof merged.bestSessionDiff === 'string') merged.bestSessionDiff = this.parseSuffixString(merged.bestSessionDiff);

    return merged as SwarmDevice;
  }

  private parseSuffixString(input: string): number {
    input = input.trim();
    const value = parseFloat(input);
    const lastChar = input.charAt(input.length - 1).toUpperCase();

    const multipliers: Record<string, number> = {
      K: 1e3,
      M: 1e6,
      G: 1e9,
      T: 1e12,
      P: 1e15,
      E: 1e18,
    };

    const multiplier = multipliers[lastChar] ?? 1;

    return value * multiplier;
  }

  public stringifyDeviceLabel(data: any): string {
    const model = data.deviceModel || 'Other';
    const asicCountPart = data.asicCount > 1 ? data.asicCount + 'x ' : '';
    const asicModel = data.ASICModel || '';

    return model + ' (' + asicCountPart + asicModel + ')';
  };

  // ---- fleet-intel wrappers (pure, tested in fleet-intel.spec.ts) ----

  public deviceClass(axe: FleetDevice): FleetClassification {
    return classifyDevice(axe);
  }

  public health(axe: FleetDevice): FleetHealth {
    return deviceHealth(axe);
  }

  public healthDotClass(axe: FleetDevice): string {
    switch (deviceHealth(axe).state) {
      case 'healthy': return 'nx-health-dot nx-health-ok';
      case 'attention': return 'nx-health-dot nx-health-warn';
      case 'critical': return 'nx-health-dot nx-health-err';
      case 'offline': return 'nx-health-dot nx-health-off';
      default: return 'nx-health-dot nx-health-unknown';
    }
  }

  public healthLabel(axe: FleetDevice): string {
    switch (deviceHealth(axe).state) {
      case 'healthy': return 'Healthy';
      case 'attention': return 'Attention';
      case 'critical': return 'Critical';
      case 'offline': return 'Offline';
      default: return 'Unknown';
    }
  }

  public healthTooltip(axe: FleetDevice): string {
    return deviceHealth(axe).reasons.join(' · ');
  }

  public online(axe: FleetDevice): boolean | null {
    return deviceOnline(axe);
  }

  public lastSeen(axe: FleetDevice): string | null {
    return lastSeenText(axe);
  }

  public efficiency(axe: FleetDevice): number | null {
    return deviceEfficiency(axe);
  }

  public poolHost(axe: FleetDevice): string | null {
    return activePoolHost(axe);
  }

  public hasPairMismatch(axe: FleetDevice): boolean {
    return pairMismatch(axe);
  }

  get summary(): FleetSummary {
    return fleetSummary(this.swarm);
  }

  /** `any[]` at the template boundary (house style for fleet rows). */
  get filteredSwarm(): any[] {
    return filterDevices(this.swarm, this.filters);
  }

  get poolOptions(): string[] {
    return this.summary.pools.map(pool => pool.host);
  }

  get poolOptionItems(): Array<{ label: string; value: string }> {
    return this.poolOptions.map(host => ({ label: host, value: host }));
  }

  public setFilter<K extends keyof FleetFilters>(key: K, value: FleetFilters[K]): void {
    this.filters = { ...this.filters, [key]: value };
    this.persistFilters();
  }

  public clearFilters(): void {
    this.filters = { ...DEFAULT_FLEET_FILTERS };
    this.persistFilters();
  }

  get filtersActive(): boolean {
    return this.filters.text !== '' || this.filters.health !== 'all'
      || this.filters.classification !== 'all' || this.filters.online !== 'all'
      || this.filters.pool !== 'all';
  }

  private persistFilters(): void {
    this.localStorageService.setObject(FLEET_FILTERS, this.filters);
  }

  public setDensity(density: 'comfortable' | 'compact'): void {
    this.density = density;
    this.localStorageService.setItem(FLEET_DENSITY, density);
  }

  public openDetail(axe: FleetDevice): void {
    this.detailDevice = axe;
  }

  public closeDetail(): void {
    this.detailDevice = null;
  }

  public toggleGridView(gridView: boolean): void {
    this.localStorageService.setBool(SWARM_GRID_VIEW, this.gridView = gridView);
  }

  isThisDevice(IP: string): boolean {
    return IP === window.location.hostname;
  }
}
