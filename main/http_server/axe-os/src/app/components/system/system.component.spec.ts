import { ComponentFixture, TestBed } from '@angular/core/testing';

import { SystemComponent } from './system.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { SystemInfo, SystemAsic } from 'src/app/generated/models';

describe('SystemComponent', () => {
  let component: SystemComponent;
  let fixture: ComponentFixture<SystemComponent>;

  const FW = 'v2.14.2-13-g388287da';
  const OLD = 'v2.14.2-9-gc630e1a';

  const asic = {
    ASICModel: 'BM1370',
    asicCount: 1,
    deviceModel: 'Gamma',
    swarmColor: 'green',
  } as SystemAsic;

  const info = {
    version: FW,
    axeOSVersion: FW,
    idfVersion: 'v5.4.1',
    boardVersion: '601',
    uptimeSeconds: 3600,
    resetReason: 'Reset due to power-on event',
    cpuUsage: 32.456,
    temp: 55.2,
    vrTemp: 60,
    coreVoltageActual: 1140,
    freeHeap: 8_400_000,
    freeHeapInternal: 130_000,
    freeHeapSpiram: 8_200_000,
    hostname: 'neuralaxe',
    ssid: 'testnet',
    wifiStatus: 'Connected!',
    wifiRSSI: -52,
    ipv4: '192.168.1.50',
    macAddr: 'AA:BB:CC:DD:EE:FF',
    runningPartition: 'ota_0',
  } as SystemInfo;

  const data = { info, asic, installedWebVersion: FW };

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [SystemComponent],
      providers: [provideHttpClient(), provideToastr()]
    })
    .compileComponents();

    fixture = TestBed.createComponent(SystemComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('separates product, firmware, hardware, runtime, memory and network sections', () => {
    const titles = component.getSystemSections(data).map(s => s.title);
    expect(titles).toEqual([
      'NeuralAxe Product',
      'Firmware & Software',
      'Hardware',
      'Runtime',
      'Memory',
      'Network',
    ]);
  });

  describe('version identity rows', () => {
    function firmwareRows(d: { info: SystemInfo; asic: SystemAsic; installedWebVersion: string | null }) {
      return component.getSystemSections(d).find(s => s.title === 'Firmware & Software')!.rows;
    }

    it('shows installed web revision from the live artifact source, with copy actions', () => {
      const rows = firmwareRows(data);
      const fwRow = rows.find(r => r.label === 'Firmware Revision')!;
      const webRow = rows.find(r => r.label === 'Web Revision (installed)')!;
      expect(fwRow.value).toBe(FW);
      expect(fwRow.copyValue).toBe(FW);
      expect(webRow.value).toBe(FW);
      expect(webRow.copyValue).toBe(FW);
      expect(rows.some(r => r.label === 'Web Revision (at boot)')).toBeFalse();
    });

    it('shows the stale boot snapshot separately when it disagrees (restart pending) without substituting values', () => {
      const staleBoot = { ...data, info: { ...info, axeOSVersion: OLD } as SystemInfo };
      const rows = firmwareRows(staleBoot);
      expect(rows.find(r => r.label === 'Web Revision (installed)')?.value).toBe(FW);
      const bootRow = rows.find(r => r.label === 'Web Revision (at boot)')!;
      expect(bootRow.value).toBe(OLD);
      expect(bootRow.tooltip).toContain('restart');
    });

    it('falls back to the boot-reported revision when the live file is unavailable', () => {
      const noLive = { ...data, installedWebVersion: null, info: { ...info, axeOSVersion: OLD } as SystemInfo };
      const rows = firmwareRows(noLive);
      expect(rows.some(r => r.label === 'Web Revision (installed)')).toBeFalse();
      const bootRow = rows.find(r => r.label === 'Web Revision (at boot)')!;
      expect(bootRow.value).toBe(OLD);
    });
  });

  describe('reset reason', () => {
    it('maps the verbose firmware sentence to a readable label and keeps the raw value in the tooltip', () => {
      const row = component.resetReasonRow('Reset due to power-on event');
      expect(row.value).toBe('Power-on');
      expect(row.tooltip).toContain('Reset due to power-on event');
    });

    it('passes unknown reset reasons through unchanged', () => {
      const row = component.resetReasonRow('Some future reset cause');
      expect(row.value).toBe('Some future reset cause');
      expect(row.tooltip).toBeUndefined();
    });
  });

  it('shows measured ASIC voltage in volts, distinct from the configured value', () => {
    const runtime = component.getSystemSections(data).find(s => s.title === 'Runtime')!;
    const measured = runtime.rows.find(r => r.label === 'Measured ASIC Voltage')!;
    expect(measured.value).toBe('1.14 V');
    expect(measured.tooltip).toContain('configured');
  });

  it('formats runtime telemetry with controlled precision', () => {
    const runtime = component.getSystemSections(data).find(s => s.title === 'Runtime')!;
    expect(runtime.rows.find(r => r.label === 'CPU Usage')?.value).toBe('32.5 %');
    expect(runtime.rows.find(r => r.label === 'ASIC Temperature')?.value).toBe('55°C');
    expect(runtime.rows.find(r => r.label === 'VR Temperature')?.value).toBe('60°C');
  });

  it('never renders NaN/undefined/null text when live fields are missing or invalid', () => {
    const brokenData = {
      asic: { ...asic, deviceModel: '', swarmColor: '' } as SystemAsic,
      installedWebVersion: null,
      info: {
        ...info,
        version: undefined,
        axeOSVersion: undefined,
        uptimeSeconds: NaN,
        cpuUsage: Infinity,
        temp: undefined,
        vrTemp: null,
        coreVoltageActual: NaN,
        freeHeap: NaN,
        wifiRSSI: undefined,
        ipv4: undefined,
        hostname: null,
        resetReason: undefined,
      } as unknown as SystemInfo,
    };

    const allValues = component.getSystemSections(brokenData)
      .flatMap(s => s.rows)
      .map(r => r.value);

    for (const value of allValues) {
      expect(value).not.toContain('NaN');
      expect(value).not.toContain('undefined');
      expect(value).not.toContain('null');
      expect(value).not.toContain('Infinity');
    }
    expect(allValues).toContain('—');
  });

  it('reports fault rows only when the firmware reports a fault', () => {
    expect(component.getFaultRows(data).length).toBe(0);

    const faultData = {
      ...data,
      info: { ...info, hardware_fault: 'ASIC comms lost', power_fault: 'VR overtemp' } as SystemInfo,
    };
    const rows = component.getFaultRows(faultData);
    expect(rows.length).toBe(2);
    expect(rows[0].value).toBe('ASIC comms lost');
    expect(rows[1].value).toBe('VR overtemp');
  });

  it('marks private network values as sensitive data (including IPv4)', () => {
    const network = component.getSystemSections(data).find(s => s.title === 'Network')!;
    for (const label of ['Hostname', 'Wi-Fi SSID', 'Wi-Fi IPv4', 'Wi-Fi IPv6', 'MAC Address']) {
      expect(network.rows.find(r => r.label === label)?.isSensitiveData)
        .withContext(label).toBeTrue();
    }
  });
});
