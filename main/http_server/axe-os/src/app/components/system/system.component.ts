import { Component, OnInit, OnDestroy } from '@angular/core';
import { Observable, Subject, combineLatest, shareReplay, first, takeUntil, map } from 'rxjs';
import { HttpErrorResponse } from '@angular/common/http';
import { ToastrService } from 'ngx-toastr';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LoadingService } from 'src/app/services/loading.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { deriveVersionState } from 'src/app/services/version-state';
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
  /** When set, a copy control is rendered that copies exactly this string. */
  copyValue?: string;
}

type SystemSection = {
  title: string;
  rows: TableRow[];
}

type CombinedData = {
  info: ISystemInfo,
  asic: ISystemASIC,
  /** Revision embedded in the currently installed www partition (/version.txt), or null when unavailable. */
  installedWebVersion: string | null,
};

/** Non-numeric telemetry: trimmed string or em dash — never "undefined"/"null" text. */
function text(value: unknown): string {
  return typeof value === 'string' && value.trim() !== '' ? value : INVALID;
}

/**
 * Short, readable labels for the firmware's verbose reset-reason sentences.
 * The raw firmware string is preserved in the tooltip; unknown values pass
 * through unchanged so nothing is ever hidden or invented.
 */
const RESET_REASON_LABELS: ReadonlyArray<{ match: RegExp; label: string }> = [
  { match: /power-on event/i, label: 'Power-on' },
  { match: /esp_restart/i, label: 'Software restart' },
  { match: /exception\/panic/i, label: 'Crash (panic)' },
  { match: /interrupt watchdog/i, label: 'Interrupt watchdog' },
  { match: /task watchdog/i, label: 'Task watchdog' },
  { match: /other watchdogs/i, label: 'Watchdog' },
  { match: /deep sleep/i, label: 'Deep-sleep wake' },
  { match: /brownout/i, label: 'Brownout (power dip)' },
  { match: /external pin/i, label: 'External reset pin' },
  { match: /USB peripheral/i, label: 'USB reset' },
  { match: /JTAG/i, label: 'JTAG reset' },
  { match: /power glitch/i, label: 'Power glitch' },
  { match: /CPU lock ?up/i, label: 'CPU lockup' },
  { match: /can not be determined/i, label: 'Undetermined' },
];

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
    private webVersionService: WebVersionService,
    private toastr: ToastrService,
  ) {
    this.info$ = this.liveDataService.info$;
    this.isConnected$ = this.liveDataService.connected$;

    this.asic$ = this.systemService.getAsicSettings().pipe(
      shareReplay({ refCount: true, bufferSize: 1 })
    );

    this.combinedData$ = combineLatest([this.info$, this.asic$, this.webVersionService.installedWebVersion$]).pipe(
      map(([info, asic, installedWebVersion]) => ({ info, asic, installedWebVersion }))
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

  /** Short reset-reason label; the raw firmware string stays in the tooltip. */
  resetReasonRow(raw: unknown): TableRow {
    const rawText = text(raw);
    if (rawText === INVALID) {
      return { label: 'Reset Reason', value: INVALID };
    }
    const mapped = RESET_REASON_LABELS.find(entry => entry.match.test(rawText));
    return {
      label: 'Reset Reason',
      value: mapped ? mapped.label : rawText,
      tooltip: mapped ? `Firmware: "${rawText}"` : undefined,
    };
  }

  /**
   * Version identity rows. Firmware revision comes from the running app image
   * (always current — an app OTA restarts the device). The installed web
   * revision comes from the live /version.txt of the www partition; the
   * firmware's boot-time snapshot (axeOSVersion) is additionally shown when it
   * disagrees, because that is exactly the state a www-only OTA leaves until
   * the next restart. Values are never substituted for one another.
   */
  private versionRows(info: ISystemInfo, installedWebVersion: string | null): TableRow[] {
    const vs = deriveVersionState(info.version, info.axeOSVersion, installedWebVersion);

    const rows: TableRow[] = [
      { label: 'Firmware Revision', value: text(info.version), copyValue: info.version || undefined, tooltip: 'From the running firmware image (esp_app_desc)' },
    ];

    if (vs.installedWeb) {
      rows.push({
        label: 'Web Revision (installed)',
        value: vs.installedWeb,
        copyValue: vs.installedWeb,
        tooltip: 'Read live from /version.txt on the www partition',
      });
      if (vs.restartPending) {
        rows.push({
          label: 'Web Revision (at boot)',
          value: vs.bootWeb!,
          valueClass: 'text-orange-500',
          tooltip: 'The firmware reports the web version it saw at boot. A web-only update does not restart the device, so this refreshes on the next restart.',
        });
      }
    } else {
      rows.push({
        label: 'Web Revision (at boot)',
        value: text(info.axeOSVersion),
        copyValue: info.axeOSVersion || undefined,
        tooltip: 'Reported by the firmware from its boot-time read of /version.txt (live file unavailable)',
      });
    }

    if (vs.status === 'mismatch') {
      rows.push({
        label: 'Pair Status',
        value: 'Version mismatch',
        valueClass: 'text-red-500',
        tooltip: 'The installed web interface and the running firmware come from different builds. Update both www.bin and esp-miner.bin from the same release.',
      });
    } else if (vs.status === 'match' && vs.restartPending) {
      rows.push({
        label: 'Pair Status',
        value: 'Match — restart pending',
        valueClass: 'text-orange-500',
        tooltip: 'Informational: the installed pair matches; the firmware boot snapshot refreshes on the next restart.',
      });
    } else if (vs.status === 'match') {
      rows.push({ label: 'Pair Status', value: 'Firmware & web match', valueClass: 'text-green-500' });
    }

    rows.push({ label: 'ESP-IDF Version', value: text(info.idfVersion) });
    rows.push({ label: 'Running Partition', value: text(info.runningPartition), tooltip: 'Currently active OTA partition' });
    return rows;
  }

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
        rows: this.versionRows(info, data.installedWebVersion),
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
          this.resetReasonRow(info.resetReason),
          { label: 'CPU Usage', value: DeckFmt.num(info.cpuUsage, 1, ' %') },
          { label: 'ASIC Temperature', value: DeckFmt.temp(info.temp) },
          { label: 'VR Temperature', value: DeckFmt.temp(info.vrTemp), tooltip: 'Voltage regulator temperature' },
          { label: 'Measured ASIC Voltage', value: DeckFmt.volts(info.coreVoltageActual), tooltip: 'Live telemetry — distinct from the configured Core Voltage (mV) in Tuning & Thermal' },
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
          { label: 'Wi-Fi IPv4', value: text(info.ipv4), isSensitiveData: true },
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

  copyToClipboard(value: string): void {
    navigator.clipboard?.writeText(value).then(
      () => this.toastr.success('Copied to clipboard'),
      () => this.toastr.error('Could not copy to clipboard'),
    );
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
