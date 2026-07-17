import { ByteSuffixPipe } from './byte-suffix.pipe';

describe('ByteSuffixPipe', () => {
  it('create an instance', () => {
    const pipe = new ByteSuffixPipe();
    expect(pipe).toBeTruthy();
  });

  it('formats byte counts with suffixes', () => {
    expect(ByteSuffixPipe.transform(8_400_000)).toBe('8.40 MB');
    expect(ByteSuffixPipe.transform(512)).toBe('512 B');
  });

  it('renders a safe zero for invalid live-device input', () => {
    expect(ByteSuffixPipe.transform(null as any)).toBe('0 B');
    expect(ByteSuffixPipe.transform(undefined as any)).toBe('0 B');
    expect(ByteSuffixPipe.transform(NaN)).toBe('0 B');
    expect(ByteSuffixPipe.transform(Infinity)).toBe('0 B');
    expect(ByteSuffixPipe.transform(-1)).toBe('0 B');
  });
});
