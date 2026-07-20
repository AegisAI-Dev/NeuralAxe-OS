import {
  fixtureA_lowCoverage, fixtureB_boundary121, FIXTURE_TARGET,
  fixtureE_baseline, fixtureE_frequencyOptions, fixtureE_voltageOptions,
  fixtureE_defaultFrequency, fixtureE_defaultVoltage, fixtureSample,
} from './stability-fixtures';
import { computeProfileResult } from './stability-results';
import { sampleTargetText } from './stability-coverage';
import { buildStarterProfiles } from './stability-profile';
import {
  evaluateFreshness, shouldAbortForStaleness, FRESHNESS_LIMIT_MS, SESSION_RECONNECT_GRACE_MS,
} from './stability-freshness';
import {
  initialVisibility, applyVisibility, visibilityStats,
} from './stability-visibility';
import { containsForbiddenKeys } from './stability-history';

describe('Real-hardware regression fixtures (sanitized)', () => {
  it('Fixture A — 10 valid samples / target 120 → Partial, 8.3% coverage, clean end', () => {
    const result = computeProfileResult(fixtureA_lowCoverage());
    expect(result.status).toBe('partial');
    expect(result.validSamples).toBe(10);
    expect(result.expectedSamples).toBe(120);
    expect(result.statusReason).toContain('8.3%');
    expect(result.statusReason.toLowerCase()).toContain('coverage');
    // Explicitly NOT a false Completed.
    expect(result.status).not.toBe('completed');
    // The partial badge names the coverage (never a bare "Partial").
    expect(result.badges.find(b => b.kind === 'partial')!.label).toContain('coverage');
  });

  it('Fixture B — 121 valid samples / target 120 → Completed, honest presentation, coverage 100%', () => {
    const result = computeProfileResult(fixtureB_boundary121());
    expect(result.status).toBe('completed');
    expect(result.validSamples).toBe(121);         // never trimmed to 120
    expect(result.expectedSamples).toBe(FIXTURE_TARGET);
    expect(result.coveragePct).toBe(100);          // capped, no off-by-one error
    expect(sampleTargetText(result.validSamples, result.expectedSamples)).toBe('121 valid samples · target 120');
  });

  it('Fixture C — visibility interruption with fresh telemetry continuing → hidden recorded, no fabricated gaps', () => {
    // The page hides at t=100 s and returns at t=340 s; telemetry keeps flowing.
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, true, 100_000, 'measure');
    acc = applyVisibility(acc, false, 340_000, 'measure');
    const vis = visibilityStats(acc, 600_000);
    expect(vis.interruptions).toBe(1);
    expect(vis.totalHiddenMs).toBe(240_000);
    expect(vis.hiddenDuringMeasure).toBeTrue();

    // Telemetry stayed genuinely fresh throughout (a real sample every 5 s), so a
    // full-coverage completed result stands — the hidden window did NOT fabricate
    // a coverage gap.
    const fresh = evaluateFreshness(595_000, 600_000);
    expect(fresh.online).toBeTrue();
  });

  it('Fixture D — visibility interruption then stale telemetry → abort only after the reconnect grace', () => {
    // Last genuine sample at t=100 s; the page is hidden and telemetry stops.
    const lastFreshMono = 100_000;
    // Within the grace (limit + grace) → not yet aborting.
    const withinGrace = evaluateFreshness(lastFreshMono, lastFreshMono + FRESHNESS_LIMIT_MS + 5_000);
    expect(withinGrace.online).toBeFalse();
    expect(shouldAbortForStaleness(withinGrace.ageMs)).toBeFalse();
    // Past the grace → abort.
    const pastGrace = evaluateFreshness(lastFreshMono, lastFreshMono + FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS + 1_000);
    expect(shouldAbortForStaleness(pastGrace.ageMs)).toBeTrue();
  });

  it('Fixture E — same frequency / higher voltage yields NO Performance starter', () => {
    const { specs, notes } = buildStarterProfiles(
      fixtureE_baseline, fixtureE_frequencyOptions, fixtureE_voltageOptions,
      fixtureE_defaultFrequency, fixtureE_defaultVoltage,
    );
    expect(specs.some(s => s.key === 'performance')).toBeFalse();
    // No offered starter is a same-frequency/higher-voltage of the baseline.
    for (const s of specs.filter(x => x.key !== 'current')) {
      const misleading = s.config.frequency === fixtureE_baseline.frequency && s.config.coreVoltage > fixtureE_baseline.coreVoltage;
      expect(misleading).toBeFalse();
    }
    expect(notes.some(n => /Performance starter omitted/i.test(n))).toBeTrue();
  });

  it('fixtures carry no identifying / sensitive fields', () => {
    expect(containsForbiddenKeys(fixtureA_lowCoverage())).toBeFalse();
    expect(containsForbiddenKeys(fixtureB_boundary121())).toBeFalse();
    expect(containsForbiddenKeys(fixtureE_baseline)).toBeFalse();
    expect(containsForbiddenKeys(fixtureSample(0))).toBeFalse();
  });
});
