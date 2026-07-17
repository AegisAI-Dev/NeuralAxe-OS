import { ComponentFixture, TestBed } from '@angular/core/testing';

import { SystemComponent } from './system.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { SystemInfo, SystemAsic } from 'src/app/generated/models';

describe('SystemComponent', () => {
  let component: SystemComponent;
  let fixture: ComponentFixture<SystemComponent>;

  const asic = {
    ASICModel: 'BM1370',
    asicCount: 1,
    deviceModel: 'Gamma',
    swarmColor: 'green',
  } as SystemAsic;

  const info = {
    version: 'v2.14.2-9-gc630e1a',
    axeOSVersion: 'v2.14.2-9-gc630e1a',
    idfVersion: 'v5.4.1',
    boardVersion: '601',
    uptimeSeconds: 3600,
    resetReason: 'Power on reset',
    cpuUsage: 32.456,
    temp: 55.2,
    vrTemp: 60,
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

  const data = { info, asic };

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

  it('keeps NeuralAxe identity separate from upstream identity', () => {
    const sections = component.getSystemSections(data);
    const product = sections.find(s => s.title === 'NeuralAxe Product')!;
    expect(product.rows.some(r => r.value.includes('NeuralAxe OS'))).toBeTrue();
    expect(product.rows.some(r => r.value.includes('ESP-Miner / AxeOS'))).toBeTrue();

    const firmware = sections.find(s => s.title === 'Firmware & Software')!;
    expect(firmware.rows.find(r => r.label === 'Firmware Version')?.value).toBe('v2.14.2-9-gc630e1a');
    expect(firmware.rows.find(r => r.label === 'Running Partition')?.value).toBe('ota_0');
  });

  it('formats runtime telemetry with controlled precision', () => {
    const runtime = component.getSystemSections(data).find(s => s.title === 'Runtime')!;
    expect(runtime.rows.find(r => r.label === 'CPU Usage')?.value).toBe('32.5 %');
    expect(runtime.rows.find(r => r.label === 'ASIC Temperature')?.value).toBe('55°C');
    expect(runtime.rows.find(r => r.label === 'VR Temperature')?.value).toBe('60°C');
  });

  it('never renders NaN/undefined text when live fields are missing or invalid', () => {
    const brokenData = {
      asic: { ...asic, deviceModel: '', swarmColor: '' } as SystemAsic,
      info: {
        ...info,
        version: undefined,
        uptimeSeconds: NaN,
        cpuUsage: Infinity,
        temp: undefined,
        vrTemp: null,
        freeHeap: NaN,
        wifiRSSI: undefined,
        ipv4: undefined,
        hostname: null,
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
      asic,
      info: { ...info, hardware_fault: 'ASIC comms lost', power_fault: 'VR overtemp' } as SystemInfo,
    };
    const rows = component.getFaultRows(faultData);
    expect(rows.length).toBe(2);
    expect(rows[0].value).toBe('ASIC comms lost');
    expect(rows[1].value).toBe('VR overtemp');
  });

  it('marks private network values as sensitive data', () => {
    const network = component.getSystemSections(data).find(s => s.title === 'Network')!;
    for (const label of ['Hostname', 'Wi-Fi SSID', 'Wi-Fi IPv6', 'MAC Address']) {
      expect(network.rows.find(r => r.label === label)?.isSensitiveData)
        .withContext(label).toBeTrue();
    }
  });
});
