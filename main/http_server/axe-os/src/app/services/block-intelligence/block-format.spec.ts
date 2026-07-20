import {
  shortHash,
  formatAgeShort,
  formatInterval,
  formatBtc,
  formatSats,
  formatBytes,
  formatWeight,
  formatCount,
  formatUtc,
  formatPerHour,
} from './block-format';

describe('block-format', () => {
  it('shortHash truncates long hashes', () => {
    expect(shortHash('0123456789abcdef0123456789abcdef')).toBe('01234567…89abcdef');
    expect(shortHash('short')).toBe('short');
    expect(shortHash(null)).toBe('—');
  });

  it('formatAgeShort', () => {
    expect(formatAgeShort(10_000)).toBe('just now');
    expect(formatAgeShort(90_000)).toBe('1m 30s');
    expect(formatAgeShort(3_600_000)).toBe('1h');
    expect(formatAgeShort(90_000_000)).toBe('1d 1h');
    expect(formatAgeShort(-1)).toBe('—');
    expect(formatAgeShort(null)).toBe('—');
  });

  it('formatInterval', () => {
    expect(formatInterval(520_000)).toBe('8m 40s');
    expect(formatInterval(3_720_000)).toBe('1h 2m');
    expect(formatInterval(45_000)).toBe('45s');
    expect(formatInterval(null)).toBe('—');
  });

  it('formatBtc / formatSats', () => {
    expect(formatBtc(312_500_000)).toBe('3.125 BTC');
    expect(formatBtc(null)).toBe('—');
    expect(formatSats(2_500_000)).toBe('2,500,000 sats');
    expect(formatSats(null)).toBe('—');
  });

  it('formatBytes / formatWeight', () => {
    expect(formatBytes(1_500_000)).toBe('1.50 MB');
    expect(formatBytes(1500)).toBe('1.5 KB');
    expect(formatBytes(500)).toBe('500 B');
    expect(formatBytes(null)).toBe('—');
    expect(formatWeight(3_990_000)).toBe('3.99 MWU');
    expect(formatWeight(null)).toBe('—');
  });

  it('formatCount', () => {
    expect(formatCount(3200)).toBe('3,200');
    expect(formatCount(null)).toBe('—');
    expect(formatCount(-5)).toBe('—');
  });

  it('formatUtc always marks UTC', () => {
    const s = formatUtc(1_733_000_000_000);
    expect(s).toContain('UTC');
    expect(s).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC$/);
    expect(formatUtc(null)).toBe('—');
    expect(formatUtc(0)).toBe('—');
  });

  it('formatPerHour', () => {
    expect(formatPerHour(6)).toBe('6.0/h');
    expect(formatPerHour(null)).toBe('—');
  });
});
