import {
  sanitizeCoinbase,
  MAX_COINBASE_READABLE,
  MAX_COINBASE_HEX_BYTES,
} from './coinbase-sanitize';

const toHex = (bytes: number[]): string => bytes.map(b => b.toString(16).padStart(2, '0')).join('');
const ascii = (s: string): number[] => Array.from(s).map(c => c.charCodeAt(0));

describe('coinbase-sanitize: sanitizeCoinbase', () => {
  it('empty input → status empty, no representations', () => {
    const c = sanitizeCoinbase({});
    expect(c.status).toBe('empty');
    expect(c.readable).toBe('');
    expect(c.escaped).toBe('');
    expect(c.hex).toBeNull();
    expect(c.hasReadable).toBeFalse();
    expect(sanitizeCoinbase({ ascii: '', hex: '' }).status).toBe('empty');
  });

  it('all-printable ASCII → status clean, readable equals input', () => {
    const c = sanitizeCoinbase({ ascii: '/Foundry USA Pool/' });
    expect(c.status).toBe('clean');
    expect(c.readable).toBe('/Foundry USA Pool/');
    expect(c.escaped).toBe('/Foundry USA Pool/');
    expect(c.hasReadable).toBeTrue();
    expect(c.truncated).toBeFalse();
  });

  it('prefers the hex source over the lossy ASCII', () => {
    // ascii is deliberately wrong/noisy; hex is the source of truth.
    const c = sanitizeCoinbase({ ascii: '����', hex: toHex(ascii('/AntPool/')) });
    expect(c.readable).toBe('/AntPool/');
    expect(c.hex).toBe(toHex(ascii('/AntPool/')));
    expect(c.status).toBe('clean');
  });

  it('NUL / control bytes → readable stripped, escaped shows \\xNN', () => {
    const c = sanitizeCoinbase({ hex: toHex([0x00, 0x09, ...ascii('Luxor'), 0x1b]) });
    expect(c.readable).toBe('Luxor');
    expect(c.escaped).toContain('\\x00');
    expect(c.escaped).toContain('\\x09');
    expect(c.escaped).toContain('\\x1B');
    expect(c.status).toBe('sanitized');
  });

  it('lossy-ASCII replacement characters are stripped from readable (no � noise)', () => {
    const c = sanitizeCoinbase({ ascii: '�AntPool�' });
    expect(c.readable).toBe('AntPool');
    expect(c.readable).not.toContain('�');
    expect(c.status).toBe('sanitized');
  });

  it('backslash is escaped as \\\\ in the escaped form', () => {
    const c = sanitizeCoinbase({ hex: toHex(ascii('a\\b')) });
    expect(c.escaped).toBe('a\\\\b');
  });

  it('collapses repeated whitespace in the readable preview', () => {
    const c = sanitizeCoinbase({ hex: toHex(ascii('A    B\t\tC')) });
    expect(c.readable).toBe('A B C');
  });

  it('no printable bytes → status binary, hasReadable false', () => {
    const c = sanitizeCoinbase({ hex: toHex([0, 1, 2, 3, 255]) });
    expect(c.status).toBe('binary');
    expect(c.hasReadable).toBeFalse();
    expect(c.readable).toBe('');
    expect(c.hex).toBeTruthy();
  });

  it('bounds readable, escaped and hex, and flags truncation', () => {
    const big = ascii('x'.repeat(400));
    const c = sanitizeCoinbase({ hex: toHex(big) });
    expect(c.readable.length).toBeLessThanOrEqual(MAX_COINBASE_READABLE + 1); // +1 for the ellipsis
    expect(c.readable.endsWith('…')).toBeTrue();
    expect(c.truncated).toBeTrue();
    expect(c.originalLength).toBe(400);
    // hex bounded to MAX_COINBASE_HEX_BYTES bytes (+ ellipsis)
    expect(c.hex!.replace('…', '').length).toBe(MAX_COINBASE_HEX_BYTES * 2);
    expect(c.hex!.endsWith('…')).toBeTrue();
  });

  it('reports the original byte length from the hex source', () => {
    const c = sanitizeCoinbase({ hex: toHex(ascii('AntPool')) });
    expect(c.originalLength).toBe(7);
  });

  it('ascii-only source has no hex representation', () => {
    const c = sanitizeCoinbase({ ascii: 'Braiins' });
    expect(c.hex).toBeNull();
    expect(c.readable).toBe('Braiins');
  });

  it('HTML/script-like bytes are kept as inert text (no parsing, no execution)', () => {
    const c = sanitizeCoinbase({ ascii: '<img src=x onerror=alert(1)>' });
    // it is just text; printable ASCII is preserved verbatim, never interpreted
    expect(c.readable).toBe('<img src=x onerror=alert(1)>');
    expect(typeof c.readable).toBe('string');
  });

  it('ignores non-hex junk (delimiters) when parsing hex', () => {
    const c = sanitizeCoinbase({ hex: '4c:75:78:6f:72' }); // "Luxor" with colon junk
    expect(c.readable).toBe('Luxor');
  });

  it('handles odd-length hex by dropping the trailing nibble', () => {
    const c = sanitizeCoinbase({ hex: '4c75786f729' }); // 'Luxor' + stray nibble
    expect(c.readable).toBe('Luxor');
  });
});
