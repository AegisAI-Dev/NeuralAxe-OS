/**
 * Miner-vs-network height relationship (Phase 2L.1, Stage 3).
 *
 * Real-device validation showed Block Intelligence reporting the latest MINED
 * network block (e.g. 958983) while the miner dashboard showed its CURRENT WORK
 * HEIGHT (958984) — the candidate block it is hashing toward. That is expected
 * Bitcoin behavior: after network block N is found, miners normally work on
 * candidate height N+1.
 *
 * This pure helper classifies the relationship HONESTLY: the ordinary N+1 case
 * is neutral/healthy, same-height is not an error, and behind/ahead states use
 * restrained informational wording. It never diagnoses a chain reorganization
 * from these two numbers alone, and never raises an alarm from a transient
 * one-block difference. NaN / Infinity / negative inputs yield 'unavailable'.
 */

export type HeightRelationship =
  | 'expected-next'              // currentWork == latestMined + 1  (normal, healthy)
  | 'same-height'               // currentWork == latestMined       (not an error)
  | 'miner-behind'              // currentWork <  latestMined
  | 'miner-ahead-by-more-than-one' // currentWork >  latestMined + 1
  | 'unavailable';              // either value missing/invalid

export interface HeightRelationshipView {
  state: HeightRelationship;
  label: string;
  /** Short supporting sentence; null when nothing useful can be said. */
  detail: string | null;
  /** Accent-independent severity — never an alarm. */
  severity: 'ok' | 'info' | 'neutral';
  latestMinedHeight: number | null;
  currentWorkHeight: number | null;
}

/** A finite, non-negative integer, or null. */
function height(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) && value >= 0 && Number.isInteger(value)
    ? value
    : null;
}

export function classifyHeightRelationship(
  latestMinedHeight: unknown,
  currentWorkHeight: unknown,
): HeightRelationship {
  const mined = height(latestMinedHeight);
  const work = height(currentWorkHeight);
  if (mined === null || work === null) {
    return 'unavailable';
  }
  if (work === mined + 1) {
    return 'expected-next';
  }
  if (work === mined) {
    return 'same-height';
  }
  if (work < mined) {
    return 'miner-behind';
  }
  return 'miner-ahead-by-more-than-one';
}

export function heightRelationshipView(
  latestMinedHeight: unknown,
  currentWorkHeight: unknown,
): HeightRelationshipView {
  const state = classifyHeightRelationship(latestMinedHeight, currentWorkHeight);
  const mined = height(latestMinedHeight);
  const work = height(currentWorkHeight);
  const base = { state, latestMinedHeight: mined, currentWorkHeight: work };

  switch (state) {
    case 'expected-next':
      return {
        ...base,
        severity: 'ok',
        label: 'Working on the expected next block',
        detail: `After network block ${mined} was found, miners normally work on candidate height ${work} (N+1).`,
      };
    case 'same-height':
      return {
        ...base,
        severity: 'info',
        label: 'Working at the latest mined height',
        detail: `Candidate height equals the latest mined block (${mined}) — normal around a fresh block.`,
      };
    case 'miner-behind':
      return {
        ...base,
        severity: 'info',
        label: 'Work height is behind the network',
        detail: `Candidate height ${work} is below the latest mined block ${mined}; it should catch up on the next job.`,
      };
    case 'miner-ahead-by-more-than-one':
      return {
        ...base,
        severity: 'info',
        label: 'Work height is ahead by more than one',
        detail: `Candidate height ${work} is more than one above the latest mined block ${mined} — informational, not a reorg diagnosis.`,
      };
    default:
      return {
        ...base,
        severity: 'neutral',
        label: 'Height comparison unavailable',
        detail: null,
      };
  }
}
