import { DiffSuffixPipe } from './diff-suffix.pipe';

describe('DiffSuffixPipe', () => {
  it('create an instance', () => {
    const pipe = new DiffSuffixPipe();
    expect(pipe).toBeTruthy();
  });

  it('formats difficulties with suffixes', () => {
    expect(DiffSuffixPipe.transform(1200)).toBe('1.20 K');
    expect(DiffSuffixPipe.transform(42)).toBe('42');
  });

  it('renders a safe zero for invalid live-device input', () => {
    expect(DiffSuffixPipe.transform(null as any)).toBe('0');
    expect(DiffSuffixPipe.transform(undefined as any)).toBe('0');
    expect(DiffSuffixPipe.transform(NaN)).toBe('0');
    expect(DiffSuffixPipe.transform(Infinity)).toBe('0');
    expect(DiffSuffixPipe.transform(-1)).toBe('0');
  });
});
