import {
  DEFAULT_FLEET_FILTERS,
  FleetDevice,
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
  rejectRatePct,
  rejectSampleConfident,
  shareSampleNote,
} from './fleet-intel';

/** A healthy online NeuralAxe Gamma as the fleet page holds it. */
function neuralaxeDevice(overrides: Partial<FleetDevice> = {}): FleetDevice {
  return {
    IP: '10.0.0.10', nxReachable: true, nxLastSeenMs: Date.now(),
    hostname: 'gamma-01', deviceModel: 'Gamma', ASICModel: 'BM1370', boardVersion: '601',
    productName: 'NeuralAxe OS', productVersion: '0.1.0-dev',
    version: 'v2.14.2-21-g6b4e7c74', axeOSVersion: 'v2.14.2-21-g6b4e7c74',
    hashRate: 1230, expectedHashrate: 1200, power: 22.4, temp: 57, vrTemp: 52,
    fanspeed: 70, fanrpm: 6967, errorPercentage: 0.0,
    sharesAccepted: 35, sharesRejected: 0, uptimeSeconds: 960,
    stratumURL: 'public-pool.io', isUsingFallbackStratum: 0,
    responseTime: 109, frequency: 625, coreVoltage: 1150,
    thermalControlMode: 'curve', effectiveControlTemperature: 57,
    requestedFanPercent: 68, appliedFanPercent: 70, activeCurveSegment: 2,
    hysteresisHolding: 1, controlSensor: 'asic', controlSensorValid: 1,
    emergencyOverrideActive: 0, overheat_mode: 0, miningPaused: false,
    ...overrides,
  };
}

/** An upstream AxeOS device: no NeuralAxe identity, no 2H thermal fields. */
function axeosDevice(overrides: Partial<FleetDevice> = {}): FleetDevice {
  return {
    IP: '10.0.0.20', nxReachable: true, nxLastSeenMs: Date.now(),
    hostname: 'bitaxe-supra', deviceModel: 'Supra', ASICModel: 'BM1368', boardVersion: '402',
    version: 'v2.9.0', axeOSVersion: 'v2.9.0',
    hashRate: 700, power: 15.1, temp: 60, sharesAccepted: 100, sharesRejected: 1,
    uptimeSeconds: 5000, stratumURL: 'solo.ckpool.org', isUsingFallbackStratum: 0,
    frequency: 550,
    ...overrides,
  };
}

describe('fleet-intel (Phase 2I)', () => {
  describe('reachability', () => {
    it('derives online/offline from the refresh bookkeeping, unknown before first contact', () => {
      expect(deviceOnline(neuralaxeDevice())).toBeTrue();
      expect(deviceOnline(neuralaxeDevice({ nxReachable: false }))).toBeFalse();
      expect(deviceOnline({ IP: '10.0.0.9' })).toBeNull();
    });

    it('formats last-seen age and returns null without a timestamp', () => {
      const now = 1_000_000_000;
      expect(lastSeenText({ IP: 'x', nxLastSeenMs: now - 41_000 }, now)).toBe('41 s ago');
      expect(lastSeenText({ IP: 'x', nxLastSeenMs: now - 10 * 60_000 }, now)).toBe('10 min ago');
      expect(lastSeenText({ IP: 'x', nxLastSeenMs: now - 3 * 3_600_000 }, now)).toBe('3 h ago');
      expect(lastSeenText({ IP: 'x' }, now)).toBeNull();
    });
  });

  describe('classification (Stage 6)', () => {
    it('classifies NeuralAxe-managed, AxeOS-compatible, unsupported and unknown', () => {
      expect(classifyDevice(neuralaxeDevice()).kind).toBe('neuralaxe');
      expect(classifyDevice(axeosDevice({ boardVersion: '601' })).kind).toBe('compatible');
      expect(classifyDevice(axeosDevice()).kind).toBe('unsupported'); // board 402
      expect(classifyDevice({ IP: '10.0.0.9' }).kind).toBe('unknown');
    });

    it('board 702 is unsupported and the tooltip forbids NeuralAxe installation', () => {
      const cls = classifyDevice(axeosDevice({ boardVersion: '702', ASICModel: 'BM1368' }));
      expect(cls.kind).toBe('unsupported');
      expect(cls.label).toBe('Board 702');
      expect(cls.tooltip).toContain('not a NeuralAxe release target');
      expect(cls.tooltip).toContain('must not be installed');
    });

    it('a NeuralAxe identity on an unsupported board still reports the board as unsupported', () => {
      expect(classifyDevice(neuralaxeDevice({ boardVersion: '702' })).kind).toBe('unsupported');
    });
  });

  describe('health model (Stage 3)', () => {
    it('healthy device with explanation', () => {
      const health = deviceHealth(neuralaxeDevice());
      expect(health.state).toBe('healthy');
      expect(health.reasons.length).toBe(1);
    });

    it('offline when unreachable, with last-seen in the reason', () => {
      const health = deviceHealth(neuralaxeDevice({ nxReachable: false, nxLastSeenMs: Date.now() - 60_000 }));
      expect(health.state).toBe('offline');
      expect(health.reasons[0]).toContain('Unreachable');
    });

    it('absence of telemetry is never healthy', () => {
      // reachable but zeroed telemetry (the old error-path artifact)
      const health = deviceHealth({ IP: '10.0.0.9', nxReachable: true, temp: 0, hashRate: null } as any);
      expect(health.state).toBe('unknown');
      // never refreshed at all
      expect(deviceHealth({ IP: '10.0.0.9' }).state).toBe('unknown');
    });

    it('critical states: overheat, emergency override, power fault, 70 °C ASIC, 105 °C VRM', () => {
      expect(deviceHealth(neuralaxeDevice({ overheat_mode: 1 })).state).toBe('critical');
      expect(deviceHealth(neuralaxeDevice({ emergencyOverrideActive: 1 })).state).toBe('critical');
      expect(deviceHealth(neuralaxeDevice({ power_fault: 'VIN_UV' })).state).toBe('critical');
      expect(deviceHealth(neuralaxeDevice({ temp: 71 })).state).toBe('critical');
      expect(deviceHealth(neuralaxeDevice({ vrTemp: 106 })).state).toBe('critical');
    });

    it('attention states with explanations', () => {
      const hot = deviceHealth(neuralaxeDevice({ temp: 66 }));
      expect(hot.state).toBe('attention');
      expect(hot.reasons[0]).toContain('66 °C');

      expect(deviceHealth(neuralaxeDevice({ fanspeed: 96 })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ errorPercentage: 3.5 })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ sharesAccepted: 90, sharesRejected: 10 })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ isUsingFallbackStratum: 1 })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ miningPaused: true })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ fanCurveError: 'malformed' })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ controlSensorValid: 0 })).state).toBe('attention');
      expect(deviceHealth(neuralaxeDevice({ axeOSVersion: 'v2.14.2-19-g88ccb619' })).state).toBe('attention');
    });

    it('critical outranks attention and reasons list every fired rule', () => {
      const health = deviceHealth(neuralaxeDevice({ temp: 71, isUsingFallbackStratum: 1 }));
      expect(health.state).toBe('critical');
      expect(health.reasons.some(r => r.includes('70 °C'))).toBeTrue();
      expect(health.reasons.some(r => r.includes('fallback'))).toBeTrue();
    });

    it('an unsupported board is not automatically unhealthy', () => {
      expect(deviceHealth(axeosDevice({ boardVersion: '702' })).state).toBe('healthy');
    });

    describe('reject-rate sample confidence (2I.1)', () => {
      it('the real pilot sample (1 reject / 26 total, ~2 min uptime) is NOT Attention from reject rate alone', () => {
        const pilot = neuralaxeDevice({ sharesAccepted: 25, sharesRejected: 1, uptimeSeconds: 120 });
        expect(deviceHealth(pilot).state).toBe('healthy');
        expect(shareSampleNote(pilot)).toContain('warming up');
        expect(shareSampleNote(pilot)).toContain('1 rejected of 26');
      });

      it('1 reject / 100 total is below 2% — no Attention, no warming-up note', () => {
        const device = neuralaxeDevice({ sharesAccepted: 99, sharesRejected: 1 });
        expect(deviceHealth(device).state).toBe('healthy');
        expect(shareSampleNote(device)).toBeNull();
      });

      it('3 rejects / 100 total (3%) is Attention — confident sample over threshold', () => {
        const device = neuralaxeDevice({ sharesAccepted: 97, sharesRejected: 3 });
        const health = deviceHealth(device);
        expect(health.state).toBe('attention');
        expect(health.reasons[0]).toContain('reject rate');
      });

      it('3 rejects / 30 total is Attention — repeated rejection at any sample size', () => {
        expect(deviceHealth(neuralaxeDevice({ sharesAccepted: 27, sharesRejected: 3 })).state).toBe('attention');
      });

      it('zero-share startup carries no reject signal and no note', () => {
        const device = neuralaxeDevice({ sharesAccepted: 0, sharesRejected: 0, uptimeSeconds: 20 });
        expect(deviceHealth(device).state).toBe('healthy');
        expect(shareSampleNote(device)).toBeNull();
        expect(rejectSampleConfident(device)).toBeFalse();
      });

      it('another genuine Attention reason remains authoritative during a small sample', () => {
        const health = deviceHealth(neuralaxeDevice({ sharesAccepted: 25, sharesRejected: 1, temp: 66 }));
        expect(health.state).toBe('attention');
        expect(health.reasons.some(r => r.includes('66 °C'))).toBeTrue();
        expect(health.reasons.some(r => r.includes('reject'))).toBeFalse();
      });

      it('Critical always overrides sample-confidence logic', () => {
        expect(deviceHealth(neuralaxeDevice({ sharesAccepted: 25, sharesRejected: 1, overheat_mode: 1 })).state).toBe('critical');
      });
    });
  });

  describe('derived metrics', () => {
    it('efficiency only when both power and hashrate are reported', () => {
      expect(deviceEfficiency(neuralaxeDevice())).toBeCloseTo(22.4 / 1.23, 1);
      expect(deviceEfficiency(neuralaxeDevice({ power: 0 }))).toBeNull();
      expect(deviceEfficiency(neuralaxeDevice({ hashRate: 0 }))).toBeNull();
      expect(deviceEfficiency({ IP: 'x' })).toBeNull();
    });

    it('reject rate and pair mismatch guard missing data', () => {
      expect(rejectRatePct(neuralaxeDevice({ sharesAccepted: 90, sharesRejected: 10 }))).toBeCloseTo(10, 5);
      expect(rejectRatePct({ IP: 'x' })).toBeNull();
      expect(pairMismatch(neuralaxeDevice())).toBeFalse();
      expect(pairMismatch(neuralaxeDevice({ axeOSVersion: 'other' }))).toBeTrue();
      expect(pairMismatch({ IP: 'x', version: 'v1' })).toBeFalse();
    });

    it('active pool host respects the fallback flag and missing data', () => {
      expect(activePoolHost(neuralaxeDevice())).toBe('public-pool.io');
      expect(activePoolHost(neuralaxeDevice({ isUsingFallbackStratum: 1, fallbackStratumURL: 'solo.ckpool.org' }))).toBe('solo.ckpool.org');
      expect(activePoolHost({ IP: 'x' })).toBeNull();
    });
  });

  describe('fleet summary (Stage 2)', () => {
    it('aggregates only derivable values with no double counting', () => {
      const devices = [
        neuralaxeDevice(),
        axeosDevice(),
        axeosDevice({ IP: '10.0.0.21', boardVersion: '702', hostname: 'suprahex' }),
        neuralaxeDevice({ IP: '10.0.0.11', nxReachable: false, hostname: 'gamma-02' }),
        { IP: '10.0.0.30' }, // never contacted
      ];
      const summary = fleetSummary(devices);
      expect(summary.total).toBe(5);
      expect(summary.online).toBe(3);
      expect(summary.offline).toBe(1);
      expect(summary.pendingFirstContact).toBe(1);
      expect(summary.online + summary.offline + summary.pendingFirstContact).toBe(summary.total);
      expect(summary.neuralaxe).toBe(2);
      expect(summary.unsupported).toBe(2); // boards 402 + 702
      expect(summary.unknownClass).toBe(1);
      expect(summary.neuralaxe + summary.compatible + summary.unsupported + summary.unknownClass).toBe(summary.total);
      // totals exclude the offline gamma and the uncontacted device:
      // 1230 + 700 + 700 GH/s and 22.4 + 15.1 + 15.1 W from the 3 online devices
      expect(summary.totalHashRate).toBeCloseTo(2630, 5);
      expect(summary.totalPower).toBeCloseTo(52.6, 5);
      expect(summary.hashRateDevices).toBe(3);
      expect(summary.efficiency).toBeCloseTo(52.6 / 2.63, 1);
      expect(summary.pools).toEqual([
        { host: 'public-pool.io', devices: 1 },
        { host: 'solo.ckpool.org', devices: 2 },
      ].sort((a, b) => b.devices - a.devices || a.host.localeCompare(b.host)));
    });

    it('reports null efficiency when no device pairs power with hashrate', () => {
      const summary = fleetSummary([axeosDevice({ power: 0 })]);
      expect(summary.efficiency).toBeNull();
      expect(summary.totalPower).toBe(0);
      expect(summary.powerDevices).toBe(0);
    });

    it('counts warnings and criticals from the health model', () => {
      const summary = fleetSummary([
        neuralaxeDevice(),
        neuralaxeDevice({ IP: 'a', temp: 66 }),
        neuralaxeDevice({ IP: 'b', overheat_mode: 1 }),
      ]);
      expect(summary.attention).toBe(1);
      expect(summary.critical).toBe(1);
    });
  });

  describe('filters and sorting (Stage 7)', () => {
    const devices = [
      neuralaxeDevice(),
      axeosDevice(),
      neuralaxeDevice({ IP: '10.0.0.11', hostname: 'gamma-02', nxReachable: false }),
      axeosDevice({ IP: '10.0.0.21', hostname: 'suprahex', boardVersion: '702' }),
    ];

    it('filters by text, health, classification, online state and pool', () => {
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, text: 'gamma' }).length).toBe(2);
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, health: 'offline' }).length).toBe(1);
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, classification: 'unsupported' }).length).toBe(2);
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, online: 'online' }).length).toBe(3);
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, online: 'offline' }).length).toBe(1);
      expect(filterDevices(devices, { ...DEFAULT_FLEET_FILTERS, pool: 'solo.ckpool.org' }).length).toBe(2);
      expect(filterDevices(devices, DEFAULT_FLEET_FILTERS).length).toBe(4);
    });

    it('sorts by numeric fields, IP octets, derived efficiency and health severity', () => {
      const byHash = [...devices].sort((a, b) => compareDevices(a, b, 'hashRate', 'desc'));
      expect(byHash[0].hostname).toBe('gamma-01');

      const byIp = [...devices].sort((a, b) => compareDevices(a, b, 'IP', 'asc'));
      expect(byIp[0].IP).toBe('10.0.0.10');
      expect(byIp[3].IP).toBe('10.0.0.21');

      // null efficiency (offline gamma has values but 702 also does) — force one null
      const withNull = [...devices, { IP: '10.0.0.40', nxReachable: true, temp: 50 } as FleetDevice];
      const byEff = withNull.sort((a, b) => compareDevices(a, b, 'efficiency', 'asc'));
      expect(byEff[byEff.length - 1].IP).toBe('10.0.0.40'); // null last in asc

      const byHealth = [...devices].sort((a, b) => compareDevices(a, b, 'health', 'asc'));
      expect(deviceHealth(byHealth[byHealth.length - 1]).state).toBe('healthy');
    });
  });
});
