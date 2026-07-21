import {
  classifyHeightRelationship,
  heightRelationshipView,
} from './height-relationship';

describe('height-relationship: classifyHeightRelationship', () => {
  it('expected-next when work == mined + 1 (the real-device 958983 / 958984 case)', () => {
    expect(classifyHeightRelationship(958983, 958984)).toBe('expected-next');
  });
  it('same-height when work == mined', () => {
    expect(classifyHeightRelationship(100, 100)).toBe('same-height');
  });
  it('miner-behind when work < mined', () => {
    expect(classifyHeightRelationship(100, 98)).toBe('miner-behind');
  });
  it('miner-ahead-by-more-than-one when work > mined + 1', () => {
    expect(classifyHeightRelationship(100, 105)).toBe('miner-ahead-by-more-than-one');
  });
  it('unavailable for missing / invalid inputs', () => {
    expect(classifyHeightRelationship(null, 5)).toBe('unavailable');
    expect(classifyHeightRelationship(5, undefined)).toBe('unavailable');
    expect(classifyHeightRelationship(NaN, 5)).toBe('unavailable');
    expect(classifyHeightRelationship(5, Infinity)).toBe('unavailable');
    expect(classifyHeightRelationship(-1, 5)).toBe('unavailable');
    expect(classifyHeightRelationship(5, 5.5)).toBe('unavailable');
    expect(classifyHeightRelationship('5' as any, 5)).toBe('unavailable');
  });
});

describe('height-relationship: heightRelationshipView', () => {
  it('the expected N+1 case is neutral/healthy — never an alarm', () => {
    const v = heightRelationshipView(958983, 958984);
    expect(v.state).toBe('expected-next');
    expect(v.severity).toBe('ok');
    expect(v.detail).toContain('958984');
    expect(v.detail).toContain('N+1');
  });

  it('same-height is informational, not an error', () => {
    const v = heightRelationshipView(500, 500);
    expect(v.state).toBe('same-height');
    expect(v.severity).toBe('info');
  });

  it('behind / ahead states are restrained info, never danger', () => {
    expect(heightRelationshipView(100, 98).severity).toBe('info');
    expect(heightRelationshipView(100, 105).severity).toBe('info');
  });

  it('ahead state never diagnoses a reorg', () => {
    const v = heightRelationshipView(100, 110);
    expect(v.detail?.toLowerCase()).toContain('not a reorg');
  });

  it('never returns the danger severity for any input', () => {
    const cases: Array<[unknown, unknown]> = [[958983, 958984], [5, 5], [10, 2], [10, 99], [null, 1], [NaN, NaN]];
    for (const [m, w] of cases) {
      expect(['ok', 'info', 'neutral']).toContain(heightRelationshipView(m, w).severity);
    }
  });

  it('unavailable view carries neutral severity and null detail', () => {
    const v = heightRelationshipView(null, null);
    expect(v.state).toBe('unavailable');
    expect(v.severity).toBe('neutral');
    expect(v.detail).toBeNull();
    expect(v.latestMinedHeight).toBeNull();
    expect(v.currentWorkHeight).toBeNull();
  });

  it('carries the parsed heights', () => {
    const v = heightRelationshipView(958983, 958984);
    expect(v.latestMinedHeight).toBe(958983);
    expect(v.currentWorkHeight).toBe(958984);
  });
});
