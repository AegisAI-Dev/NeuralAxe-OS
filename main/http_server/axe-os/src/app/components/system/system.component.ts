import { Component, OnInit, OnDestroy } from '@angular/core';
import { Observable, Subject, combineLatest, shareReplay, first, takeUntil, map } from 'rxjs';
import { HttpErrorResponse } from '@angular/common/http';
import { ToastrService } from 'ngx-toastr';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LoadingService } from 'src/app/services/loading.service';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { DeckFmt, INVALID } from 'src/app/components/command-deck/deck-format';
import { SystemInfo as ISystemInfo, SystemAsic as ISystemASIC, GenericResponse, } from 'src/app/generated/models';
import { NEURALAXE } from 'src/app/neuralaxe';

type TableRow = {
  label: string;
  value: string;
  class?: string;
  valueClass?: string;
  isSensitiveData?: boolean;
  tooltip?: string;
}

type SystemSection = {
  title: string;
  rows: TableRow[];
}

type CombinedData = {
  info: ISystemInfo,
  asic: ISystemASIC
};

/** Non-numeric telemetry: trimmed string or em dash — never "undefined"/"null" text. */
function text(value: unknown): string {
  return typeof value === 'string' && value.trim() !== '' ? value : INVALID;
}

@Component({
  selector: 'app-system',
  templateUrl: './system.component.html',
})
export class SystemComponent implements OnInit, OnDestroy {
  public info$: Observable<ISystemInfo>;
  public asic$: Observable<ISystemASIC>;
  public combinedData$: Observable<CombinedData>;
  public isConnected$: Observable<boolean>;

  private destroy$ = new Subject<void>();

  constructor(
    private systemService: SystemApiService,
    private liveDataService: LiveDataService,
    private loadingService: LoadingService,
    private toastr: ToastrService,
  ) {
    this.info$ = this.liveDataService.info$;
    this.isConnected$ = this.liveDataService.connected$;

    this.asic$ = this.systemService.getAsicSettings().pipe(
      shareReplay({ refCount: true, bufferSize: 1 })
    );

    this.combinedData$ = combineLatest([this.info$, this.asic$]).pipe(
      map(([info, asic]) => ({ info, asic }))
    );
  }

  ngOnInit() {
    this.combinedData$
      .pipe(first(), this.loadingService.lockUIUntilComplete(), takeUntil(this.destroy$))
      .subscribe();
  }

  ngOnDestroy() {
    this.destroy$.next();
    this.destroy$.complete();
  }

  getWifiRssiColor(rssi: number): string {
    if (rssi > -50) return 'text-green-500';
    if (rssi <= -50 && rssi > -60) return 'text-blue-500';
    if (rssi <= -60 && rssi > -70) return 'text-orange-500';

    return 'text-red-500';
  }

  getWifiRssiTooltip(rssi: number): string {
    if (rssi > -50) return 'Excellent';
    if (rssi <= -50 && rssi > -60) return 'Good';
    if (rssi <= -60 && rssi > -70) return 'Fair';

    return 'Weak';
  }

  private wifiRssiRow(rssi: unknown): TableRow {
    if (typeof rssi !== 'number' || !isFinite(rssi)) {
      return { label: 'Wi-Fi RSSI', value: INVALID };
    }
    return {
      label: 'Wi-Fi RSSI',
      value: rssi + ' dBm',
      valueClass: this.getWifiRssiColor(rssi),
      tooltip: this.getWifiRssiTooltip(rssi),
    };
  }

  /**
   * Device information grouped into sections: NeuralAxe product identity,
   * upstream firmware identity, hardware, runtime, memory and network.
   * Every value is guarded — live devices may momentarily report missing
   * or invalid fields, which must never render as NaN/undefined text.
   */
  getSystemSections(data: CombinedData): SystemSection[] {
    const info = data.info;
    const asic = data.asic;

    return [
      {
        title: 'NeuralAxe Product',
        rows: [
          { label: 'Product', value: `${NEURALAXE.productName} ${info.productVersion || NEURALAXE.productVersion}` },
          { label: 'Build Channel', value: info.buildChannel || NEURALAXE.buildChannel },
          { label: 'Based On', value: `${NEURALAXE.upstreamProject} ${NEURALAXE.upstreamVersion}` },
          { label: 'Build Target', value: `${NEURALAXE.targetDevice} ${NEURALAXE.targetBoard} / ${NEURALAXE.targetAsic}`, tooltip: 'Declared build target, not runtime-detected' },
        ],
      },
      {
        title: 'Firmware & Software',
        rows: [
          { label: 'Firmware Version', value: text(info.version) },
          { label: 'Web Interface Version', value: text(info.axeOSVersion), tooltip: 'Upstream AxeOS web version' },
          { label: 'ESP-IDF Version', value: text(info.idfVersion) },
          { label: 'Running Partition', value: text(info.runningPartition), tooltip: 'Currently active OTA partition' },
        ],
      },
      {
        title: 'Hardware',
        rows: [
          { label: 'Device Model', value: asic.deviceModel || 'Other', valueClass: asic.swarmColor ? 'text-' + asic.swarmColor + '-500' : undefined },
          { label: 'Board Version', value: text(info.boardVersion) },
          { label: 'ASIC', value: (asic.asicCount > 1 ? asic.asicCount + 'x ' : '') + (asic.ASICModel || INVALID) },
        ],
      },
      {
        title: 'Runtime',
        rows: [
          { label: 'Uptime', value: typeof info.uptimeSeconds === 'number' && isFinite(info.uptimeSeconds) && info.uptimeSeconds > 0 ? DateAgoPipe.transform(info.uptimeSeconds) : INVALID },
          { label: 'Reset Reason', value: text(info.resetReason) },
          { label: 'CPU Usage', value: DeckFmt.num(info.cpuUsage, 1, ' %') },
          { label: 'ASIC Temperature', value: DeckFmt.temp(info.temp) },
          { label: 'VR Temperature', value: DeckFmt.temp(info.vrTemp), tooltip: 'Voltage regulator temperature' },
        ],
      },
      {
        title: 'Memory',
        rows: [
          { label: 'Free Heap', value: DeckFmt.bytes(info.freeHeap) },
          { label: '• Internal', value: DeckFmt.bytes(info.freeHeapInternal) },
          { label: '• SPIRAM', value: DeckFmt.bytes(info.freeHeapSpiram) },
        ],
      },
      {
        title: 'Network',
        rows: [
          { label: 'Hostname', value: text(info.hostname), isSensitiveData: true },
          { label: 'Wi-Fi SSID', value: text(info.ssid), isSensitiveData: true },
          { label: 'Wi-Fi Status', value: text(info.wifiStatus) },
          this.wifiRssiRow(info.wifiRSSI),
          { label: 'Wi-Fi IPv4', value: text(info.ipv4) },
          { label: 'Wi-Fi IPv6', value: text(info.ipv6), isSensitiveData: true },
          { label: 'MAC Address', value: text(info.macAddr), isSensitiveData: true },
        ],
      },
    ];
  }

  /** Fault rows are shown only when the firmware actually reports a fault. */
  getFaultRows(data: CombinedData): TableRow[] {
    const rows: TableRow[] = [];
    if (data.info.hardware_fault) {
      rows.push({ label: 'Hardware Fault', value: data.info.hardware_fault, valueClass: 'text-red-500' });
    }
    if (data.info.power_fault) {
      rows.push({ label: 'Power Fault', value: data.info.power_fault, valueClass: 'text-red-500' });
    }
    return rows;
  }

  identifyDevice(): void {
    this.systemService.identify()
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: (result) => {
          this.toastr.success((result as GenericResponse).message);
        },
        error: (err: HttpErrorResponse) => {
          this.toastr.error(`Could not identify device. ${err.message}`);
        }
      });
  }
}
