import { HashSuffixPipe } from './hash-suffix.pipe';

describe('HashSuffixPipe', () => {
  it('create an instance', () => {
    const pipe = new HashSuffixPipe();
    expect(pipe).toBeTruthy();
  });

  it('formats live-device hashrates', () => {
    expect(HashSuffixPipe.transform(1289.7)).toBe('1.29 Th/s');
    expect(HashSuffixPipe.transform(950)).toBe('950 Gh/s');
  });

  it('renders a safe zero for invalid live-device input', () => {
    expect(HashSuffixPipe.transform(null as any)).toBe('0 H/s');
    expect(HashSuffixPipe.transform(undefined as any)).toBe('0 H/s');
    expect(HashSuffixPipe.transform(NaN)).toBe('0 H/s');
    expect(HashSuffixPipe.transform(Infinity)).toBe('0 H/s');
    expect(HashSuffixPipe.transform(-Infinity)).toBe('0 H/s');
    expect(HashSuffixPipe.transform(-5)).toBe('0 H/s');
  });
});
