/**
 * Coinbase evidence sanitization (Phase 2L.1, Stage 4).
 *
 * A block's coinbase scriptsig contains binary data (block height, extranonce)
 * interleaved with a printable pool tag. Providers expose it two ways:
 *   - `coinbaseSignatureAscii`: a LOSSY ASCII rendering that can contain Unicode
 *     replacement characters (U+FFFD �) and other noise;
 *   - `coinbaseRaw`: the exact bytes as hex.
 *
 * This pure, bounded model produces safe, honest, readable views. It PREFERS the
 * raw hex bytes (the source of truth) so it never inherits or emits the
 * provider's broken replacement-character noise. When only the lossy ASCII is
 * available, it strips the noise deterministically instead of rendering it.
 *
 * Nothing here parses HTML, uses innerHTML, or executes control sequences — the
 * outputs are plain, bounded strings safe to interpolate as text.
 */

/** Display bound for the readable ASCII preview. */
export const MAX_COINBASE_READABLE = 80;
/** Display bound for the escaped representation. */
export const MAX_COINBASE_ESCAPED = 220;
/** Byte bound for the optional hex representation. */
export const MAX_COINBASE_HEX_BYTES = 64;

export type CoinbaseStatus =
  | 'empty'      // no coinbase bytes at all
  | 'clean'      // every byte is printable ASCII
  | 'sanitized'  // mixed printable ASCII and non-printable bytes
  | 'binary';    // no printable ASCII at all

export interface SanitizedCoinbase {
  /** Printable-ASCII preview (non-printable → space, collapsed, bounded). */
  readable: string;
  /** Escaped form: printable ASCII kept, other bytes → \xNN; bounded. */
  escaped: string;
  /** Bounded lowercase hex of the source bytes, or null when no hex source. */
  hex: string | null;
  /** Original source length in bytes when known. */
  originalLength: number | null;
  /** True when any of the representations was truncated to its bound. */
  truncated: boolean;
  status: CoinbaseStatus;
  /** True when the readable preview has any content. */
  hasReadable: boolean;
}

export const EMPTY_COINBASE: SanitizedCoinbase = {
  readable: '',
  escaped: '',
  hex: null,
  originalLength: null,
  truncated: false,
  status: 'empty',
  hasReadable: false,
};

function parseHex(hex: unknown): number[] | null {
  if (typeof hex !== 'string') {
    return null;
  }
  const clean = hex.replace(/[^0-9a-fA-F]/g, '');
  if (clean.length < 2) {
    return null;
  }
  const even = clean.length % 2 === 0 ? clean : clean.slice(0, clean.length - 1);
  const bytes: number[] = [];
  for (let i = 0; i < even.length; i += 2) {
    bytes.push(parseInt(even.slice(i, i + 2), 16));
  }
  return bytes;
}

/** Char codes of a string (fallback source when no hex is available). */
function stringCodes(value: unknown): number[] | null {
  if (typeof value !== 'string' || value.length === 0) {
    return null;
  }
  const codes: number[] = [];
  for (let i = 0; i < value.length; i++) {
    codes.push(value.charCodeAt(i));
  }
  return codes;
}

function isPrintableAscii(code: number): boolean {
  return code >= 0x20 && code <= 0x7e;
}

function escapeByte(code: number): string {
  if (code === 0x5c) {
    return '\\\\'; // backslash itself
  }
  if (isPrintableAscii(code)) {
    return String.fromCharCode(code);
  }
  // \xNN for a byte; wider code points (lossy-ASCII fallback) use \uNNNN.
  if (code <= 0xff) {
    return '\\x' + code.toString(16).padStart(2, '0').toUpperCase();
  }
  return '\\u' + code.toString(16).padStart(4, '0').toUpperCase();
}

/**
 * Sanitize coinbase evidence. Prefers `hex` (exact bytes) over the lossy `ascii`.
 * Returns bounded readable / escaped / hex views and a status, all plain text.
 */
export function sanitizeCoinbase(input: { ascii?: string | null; hex?: string | null }): SanitizedCoinbase {
  const fromHex = parseHex(input?.hex);
  const source: number[] | null = fromHex ?? stringCodes(input?.ascii);
  if (!source || source.length === 0) {
    return { ...EMPTY_COINBASE };
  }

  const originalLength = source.length;
  let printableCount = 0;

  // ---- readable preview ----
  let readableRaw = '';
  for (const code of source) {
    if (isPrintableAscii(code)) {
      readableRaw += String.fromCharCode(code);
      printableCount++;
    } else {
      readableRaw += ' ';
    }
  }
  let readable = readableRaw.replace(/\s+/g, ' ').trim();
  let truncated = false;
  if (readable.length > MAX_COINBASE_READABLE) {
    readable = readable.slice(0, MAX_COINBASE_READABLE) + '…';
    truncated = true;
  }

  // ---- escaped representation ----
  let escaped = '';
  for (const code of source) {
    const piece = escapeByte(code);
    if (escaped.length + piece.length > MAX_COINBASE_ESCAPED) {
      escaped += '…';
      truncated = true;
      break;
    }
    escaped += piece;
  }

  // ---- bounded hex (only from a real hex source) ----
  let hex: string | null = null;
  if (fromHex) {
    const shown = fromHex.slice(0, MAX_COINBASE_HEX_BYTES);
    hex = shown.map(b => b.toString(16).padStart(2, '0')).join('');
    if (fromHex.length > MAX_COINBASE_HEX_BYTES) {
      hex += '…';
      truncated = true;
    }
  }

  let status: CoinbaseStatus;
  if (printableCount === source.length) {
    status = 'clean';
  } else if (printableCount === 0) {
    status = 'binary';
  } else {
    status = 'sanitized';
  }

  return {
    readable,
    escaped,
    hex,
    originalLength,
    truncated,
    status,
    hasReadable: readable.length > 0,
  };
}
