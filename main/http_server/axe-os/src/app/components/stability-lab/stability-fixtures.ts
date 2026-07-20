/**
 * NeuralAxe Stability Lab — sanitized real-hardware regression fixtures
 * (Phase 2K.1).
 *
 * These reproduce the exact shapes the real Gamma-601 pilot exposed, with NO
 * identifying values (no hostname, IP, SSID, pool, worker or credentials — only
 * synthetic telemetry numbers and tuning fields). They exist so the reliability
 * behaviour is pinned by tests: a low-coverage background run must be Partial
 * with an honest reason, a 121-vs-120 boundary must read cleanly, visibility and
 * staleness must be handled, and a same-frequency/higher-voltage "Performance"
 * starter must never be offered.
 */

import { LabSample } from './stability-telemetry';
import { ProfileRun } from './stability-results';
import { VisibilityStats } from './stability-visibility';
import { TuningConfig } from './stability-profile';

/** Standard cadence and window used by the fixtures. */
export const FIXTURE_CADENCE_MS = 5000;
export const FIXTURE_WINDOW_MS = 600_000; // 600 s
export const FIXTURE_TARGET = 120;        // 600 s / 5 s

/** A synthetic, fully-sanitized measurement sample at a window-relative time. */
export function fixtureSample(tMs: number, over: Partial<LabSample> = {}): LabSample {
  return {
    tMs,
    phase: 'measure',
    profileIndex: 0,
    hashRate: 1273.9,
    expectedHashrate: 1275,
    power: 21.7,
    efficiency: 17.0,
    asicTemp: 55.5,
    vrmTemp: 48,
    requestedFan: 60,
    appliedFan: 62,
    rpm: 4200,
    errorPercentage: 0.4,
    sharesAccepted: 1000,
    sharesRejected: 3,
    poolLatency: 40,
    thermalControlMode: 'curve',
    controlTemp: 55.5,
    emergencyOverride: false,
    sensorValid: true,
    miningPaused: false,
    gap: false,
    ...over,
  };
}

/** N evenly-spaced valid samples across a window. */
export function evenSamples(count: number, windowMs = FIXTURE_WINDOW_MS): LabSample[] {
  if (count <= 0) return [];
  if (count === 1) return [fixtureSample(0)];
  const step = windowMs / (count - 1);
  return Array.from({ length: count }, (_, i) => fixtureSample(Math.round(i * step), { hashRate: 1270 + (i % 9) }));
}

const noVisibility: VisibilityStats = {
  currentlyHidden: false, interruptions: 0, totalHiddenMs: 0, longestHiddenMs: 0,
  hiddenDuringWarmup: false, hiddenDuringMeasure: false,
};

function baseRun(over: Partial<ProfileRun>): ProfileRun {
  return {
    profileId: 'fixture', profileName: 'Baseline 625/1150 Fan Curve', thermalControlMode: 'curve',
    warmupSamples: [], measureSamples: [],
    requestedMeasureMs: FIXTURE_WINDOW_MS, measuredMeasureMs: FIXTURE_WINDOW_MS,
    cadenceMs: FIXTURE_CADENCE_MS, measureStartTMs: 0, visibility: noVisibility,
    restartOccurred: false, countersReset: false,
    ranFullWindow: true, aborted: false, failed: false,
    ...over,
  };
}

/**
 * Fixture A — the real low-coverage session: a 600 s window that ran to the end
 * but captured only 10 genuine emissions (page not continuously foregrounded).
 * Must be Partial with an explicit 8.3 % coverage reason.
 */
export function fixtureA_lowCoverage(): ProfileRun {
  return baseRun({
    measureSamples: evenSamples(10),
    visibility: { currentlyHidden: false, interruptions: 1, totalHiddenMs: 540_000, longestHiddenMs: 540_000, hiddenDuringWarmup: false, hiddenDuringMeasure: true },
  });
}

/**
 * Fixture B — a full-coverage session that landed 121 samples against a target
 * of 120 due to cadence/boundary jitter. Must be Completed and presented as
 * "121 valid samples · target 120" with no off-by-one invalidation.
 */
export function fixtureB_boundary121(): ProfileRun {
  return baseRun({ measureSamples: evenSamples(121) });
}

/**
 * Fixture E — a candidate whose only change is a higher voltage at the same
 * frequency (625 MHz / 1150 mV → 625 MHz / 1250 mV). Baseline plus served
 * option lists for buildStarterProfiles; NO Performance starter may result.
 */
export const fixtureE_baseline: TuningConfig = {
  frequency: 625, coreVoltage: 1150, thermalControlMode: 'curve',
  minFanSpeed: 25, fanCurveHysteresis: 2,
  fanCurve: [{ tempC: 45, fanPercent: 40 }, { tempC: 55, fanPercent: 60 }, { tempC: 62, fanPercent: 80 }, { tempC: 68, fanPercent: 100 }],
};
export const fixtureE_frequencyOptions = [575, 600, 625];
export const fixtureE_voltageOptions = [1150, 1200, 1250];
export const fixtureE_defaultFrequency = 600;
export const fixtureE_defaultVoltage = 1200;
