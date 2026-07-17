/**
 * Command Deck operational intelligence (Stage 2G).
 *
 * Pure, frontend-derived metrics computed from existing telemetry. Every
 * formula is documented and tested; invalid inputs (zero, NaN, null,
 * Infinity) yield null instead of misleading numbers. Nothing here predicts
 * anything — the solo-mining figures are statistical expectations only.
 */

const SECONDS_PER_DAY = 86400;
const SECONDS_PER_YEAR = 365.25 * SECONDS_PER_DAY;

function num(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) ? value : null;
}

function positive(value: unknown): number | null {
  const v = num(value);
  return v !== null && v > 0 ? v : null;
}

// ---------- session performance ----------

/** Accepted shares per hour of uptime; null before meaningful uptime (>60 s). */
export function sharesPerHour(sharesAccepted: unknown, uptimeSeconds: unknown): number | null {
  const shares = num(sharesAccepted);
  const uptime = positive(uptimeSeconds);
  if (shares === null || shares < 0 || uptime === null || uptime < 60) {
    return null;
  }
  return (shares / uptime) * 3600;
}

/** Rejected share percentage of all submitted shares; null before any share. */
export function rejectRatePct(sharesAccepted: unknown, sharesRejected: unknown): number | null {
  const accepted = num(sharesAccepted);
  const rejected = num(sharesRejected);
  if (accepted === null || rejected === null || accepted < 0 || rejected < 0) {
    return null;
  }
  const total = accepted + rejected;
  return total > 0 ? (rejected / total) * 100 : null;
}

// ---------- hashrate quality ----------

/** Current hashrate as a percentage of the model-expected hashrate. */
export function currentVsExpectedPct(hashRate: unknown, expectedHashrate: unknown): number | null {
  const current = num(hashRate);
  const expected = positive(expectedHashrate);
  if (current === null || current < 0 || expected === null) {
    return null;
  }
  return (current / expected) * 100;
}

/** Current hashrate as a percentage of the 1-hour average. */
export function currentVsAveragePct(hashRate: unknown, hashRateAvg: unknown): number | null {
  const current = num(hashRate);
  const avg = positive(hashRateAvg);
  if (current === null || current < 0 || avg === null) {
    return null;
  }
  return (current / avg) * 100;
}

/**
 * Recent hashrate variability: the coefficient of variation (sample standard
 * deviation / mean) of the recent samples, in percent. Needs at least 5
 * samples with a positive mean. Lower is steadier.
 */
export function recentVariabilityPct(series: ReadonlyArray<number> | undefined | null): number | null {
  if (!series) {
    return null;
  }
  const values = series.filter((v): v is number => typeof v === 'number' && isFinite(v));
  if (values.length < 5) {
    return null;
  }
  const mean = values.reduce((a, b) => a + b, 0) / values.length;
  if (mean <= 0) {
    return null;
  }
  const variance = values.reduce((acc, v) => acc + (v - mean) * (v - mean), 0) / (values.length - 1);
  return (Math.sqrt(variance) / mean) * 100;
}

// ---------- solo mining odds ----------

export interface SoloOdds {
  /** Expected hashes for one block: networkDifficulty × 2^32. */
  expectedHashesPerBlock: number;
  /** Expected time to a block at the current hashrate, in seconds. */
  expectedSecondsToBlock: number;
  /** P(at least one block in 24 h) = 1 − exp(−86400 / expectedSeconds). */
  dailyBlockProbability: number;
  /** "About 1 in N days" — expectedSeconds / 86400. */
  oneInNDays: number;
}

/**
 * Solo-mining expectation from current hashrate (GH/s) and network
 * difficulty. Statistical estimation only — mining is a Poisson process and
 * a block may come far sooner or never; this is NOT a prediction.
 */
export function soloOdds(networkDifficulty: unknown, hashRateGh: unknown): SoloOdds | null {
  const difficulty = positive(networkDifficulty);
  const gh = positive(hashRateGh);
  if (difficulty === null || gh === null) {
    return null;
  }
  const expectedHashesPerBlock = difficulty * 2 ** 32;
  const hashesPerSecond = gh * 1e9;
  const expectedSecondsToBlock = expectedHashesPerBlock / hashesPerSecond;
  const dailyBlockProbability = 1 - Math.exp(-SECONDS_PER_DAY / expectedSecondsToBlock);
  return {
    expectedHashesPerBlock,
    expectedSecondsToBlock,
    dailyBlockProbability,
    oneInNDays: expectedSecondsToBlock / SECONDS_PER_DAY,
  };
}

/** Session/all-time best difficulty as a percentage of network difficulty. */
export function bestDiffPctOfNetwork(bestDiff: unknown, networkDifficulty: unknown): number | null {
  const best = num(bestDiff);
  const network = positive(networkDifficulty);
  if (best === null || best < 0 || network === null) {
    return null;
  }
  return (best / network) * 100;
}

/** "~N years" / "~N days" / "~N hours" for an expected duration. */
export function formatExpectedTime(seconds: number | null | undefined): string {
  const s = positive(seconds);
  if (s === null) {
    return '—';
  }
  if (s >= SECONDS_PER_YEAR * 1.5) {
    const years = s / SECONDS_PER_YEAR;
    return years >= 1000 ? `~${compactNumber(years)} years` : `~${Math.round(years)} years`;
  }
  if (s >= SECONDS_PER_DAY * 1.5) {
    return `~${Math.round(s / SECONDS_PER_DAY)} days`;
  }
  if (s >= 5400) {
    return `~${Math.round(s / 3600)} hours`;
  }
  return `~${Math.max(1, Math.round(s / 60))} minutes`;
}

/**
 * Probability as a percentage with compact scientific notation for extremely
 * small odds (e.g. "1.9e-4 %"), so tiny values never render as "0.00 %".
 */
export function formatProbabilityPct(probability: number | null | undefined): string {
  const p = num(probability);
  if (p === null || p < 0) {
    return '—';
  }
  const pct = p * 100;
  if (pct === 0) {
    return '0 %';
  }
  if (pct >= 0.01) {
    return `${pct.toFixed(2)} %`;
  }
  return `${pct.toExponential(1)} %`;
}

/**
 * A percent VALUE (already ×100) with compact scientific notation for tiny
 * magnitudes, so a 1.4e-5 % best-difficulty share never renders as "0.00 %".
 */
export function formatPctCompact(pct: number | null | undefined): string {
  const p = num(pct);
  if (p === null || p < 0) {
    return '—';
  }
  if (p === 0) {
    return '0 %';
  }
  if (p >= 0.01) {
    return `${p.toFixed(2)} %`;
  }
  return `${p.toExponential(1)} %`;
}

/** Thousands-grouped integer or compact million/billion form for huge counts. */
export function compactNumber(value: number): string {
  if (!isFinite(value)) {
    return '—';
  }
  if (value >= 1e9) {
    return `${(value / 1e9).toFixed(1)}B`;
  }
  if (value >= 1e6) {
    return `${(value / 1e6).toFixed(1)}M`;
  }
  if (value >= 1e4) {
    return `${(value / 1e3).toFixed(0)}k`;
  }
  return Math.round(value).toLocaleString('en-US');
}

// ---------- thermal headroom ----------

export interface ThermalHeadroom {
  /** Signed °C distance from target (positive = above target). */
  deltaC: number | null;
  state: 'below-target' | 'at-target' | 'above-target' | 'overheat' | 'unknown';
  /** True when automatic fan control is pinned at ≥95% duty. */
  fanSaturated: boolean;
  label: string;
}

/**
 * Transparent thermal state: at/below/above the configured target (±2 °C
 * tolerance band = "at target"), with the fixed 70 °C overheat line and an
 * automatic-fan saturation flag.
 */
export function thermalHeadroom(
  temp: unknown,
  targetTemp: unknown,
  autoFan: boolean,
  fanPercent: unknown,
): ThermalHeadroom {
  const t = positive(temp);
  const target = positive(targetTemp);
  const fan = num(fanPercent);
  const fanSaturated = autoFan && fan !== null && fan >= 95;

  if (t === null) {
    return { deltaC: null, state: 'unknown', fanSaturated, label: 'No temperature reading' };
  }
  if (t >= 70) {
    return { deltaC: target !== null ? t - target : null, state: 'overheat', fanSaturated, label: 'Above safe temperature' };
  }
  if (target === null) {
    return { deltaC: null, state: 'unknown', fanSaturated, label: 'No target configured' };
  }
  const delta = t - target;
  if (delta > 2) {
    return { deltaC: delta, state: 'above-target', fanSaturated, label: fanSaturated ? 'Above target — fan near saturation' : 'Above target' };
  }
  if (delta < -2) {
    return { deltaC: delta, state: 'below-target', fanSaturated, label: 'Below target' };
  }
  return { deltaC: delta, state: 'at-target', fanSaturated, label: fanSaturated ? 'At target — fan near saturation' : 'At target — thermal control stable' };
}
