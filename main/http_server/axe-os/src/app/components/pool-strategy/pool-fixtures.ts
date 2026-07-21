/**
 * NeuralAxe Pool Strategy Center — deterministic test fixtures (Phase 2M,
 * Stage 14).
 *
 * All credentials here are SYNTHETIC and clearly fake. The privacy fixtures use
 * distinctive sentinel strings (SECRET_*) so tests can prove those values never
 * enter history, exports or the switch state machine. No real user identifiers.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { PoolEndpoint, PoolProfile } from './pool-profile';

let seq = 0;
function id(): string {
  seq += 1;
  return `fixture-pool-${seq}`;
}

export function endpoint(overrides: Partial<PoolEndpoint> = {}): PoolEndpoint {
  return {
    host: 'btc.example-pool.test',
    port: 3333,
    user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1',
    passwordMode: 'keep',
    tls: 0,
    protocol: 'SV1',
    ...overrides,
  };
}

/**
 * Session-only password sentinels. These represent values the owner would type
 * AT SWITCH TIME — they must never appear in any persisted/exported object
 * (profile, history, restore snapshot, interruption record). Distinct from the
 * synthetic wallet in a profile's `user`, which is legitimately stored.
 */
export const SECRET_PASSWORDS = {
  targetPrimary: 'SECRET_PWD_PRIMARY_DO_NOT_LEAK',
  targetFallback: 'SECRET_PWD_FALLBACK_DO_NOT_LEAK',
  original: 'SECRET_PWD_ORIGINAL_DO_NOT_LEAK',
} as const;

export const SECRET_TOKENS = [
  SECRET_PASSWORDS.targetPrimary,
  SECRET_PASSWORDS.targetFallback,
  SECRET_PASSWORDS.original,
] as const;

export function profile(overrides: Partial<PoolProfile> = {}): PoolProfile {
  const now = 1_700_000_000_000;
  return {
    id: id(),
    name: 'BTC Solo',
    chain: 'BTC',
    primary: endpoint(),
    fallback: null,
    notes: undefined,
    createdAt: now,
    updatedAt: now,
    ...overrides,
  };
}

export function btcProfile(): PoolProfile {
  return profile({
    name: 'BTC Solo CKPool',
    chain: 'BTC',
    primary: endpoint({ host: 'solo.ckpool.org', port: 3333, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1' }),
  });
}

export function bchProfile(): PoolProfile {
  return profile({
    name: 'BCH Pool',
    chain: 'BCH',
    primary: endpoint({ host: 'bch.example-pool.test', port: 3334, user: 'bitcoincash:qsyntheticbchaddrexampleonly000000000.rig1' }),
  });
}

export function customProfile(): PoolProfile {
  return profile({
    name: 'Lab Regtest',
    chain: 'custom',
    primary: endpoint({ host: 'regtest.example.test', port: 3335, user: 'worker.custom' }),
  });
}

export function primaryOnlyProfile(): PoolProfile {
  return profile({ name: 'Primary Only', chain: 'BTC', fallback: null });
}

export function primaryFallbackProfile(): PoolProfile {
  return profile({
    name: 'Primary + Fallback',
    chain: 'BTC',
    primary: endpoint({ host: 'primary.example-pool.test', port: 3333 }),
    fallback: endpoint({ host: 'fallback.example-pool.test', port: 3334, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig2' }),
  });
}

export function invalidHostProfile(): PoolProfile {
  return profile({ name: 'Bad Host', primary: endpoint({ host: 'stratum+tcp://pool.test' }) });
}

export function invalidPortProfile(): PoolProfile {
  return profile({ name: 'Bad Port', primary: endpoint({ port: 99999 as unknown as number }) });
}

export function missingCredProfile(): PoolProfile {
  return profile({ name: 'No Account', primary: endpoint({ user: '' }) });
}

/**
 * A profile whose primary + fallback are BOTH passwordMode 'set' (a password
 * replacement). The raw password is NOT stored on the profile — it is entered at
 * switch time (see SECRET_PASSWORDS). The `user` values are ordinary synthetic
 * wallets (legitimately stored on the profile).
 */
export function secretProfile(): PoolProfile {
  return profile({
    name: 'Account Pool (replace pw)',
    chain: 'BCH',
    primary: endpoint({
      host: 'account.example-pool.test', port: 3333,
      user: 'bitcoincash:qsyntheticbchaddrexampleonly000000000.rig1',
      passwordMode: 'set',
    }),
    fallback: endpoint({
      host: 'account-fallback.example-pool.test', port: 3334,
      user: 'bitcoincash:qsyntheticbchaddrexampleonly000000000.rig2',
      passwordMode: 'set',
    }),
  });
}

// ---------------------------------------------------------------------------
// Device (SystemInfo) fixtures
// ---------------------------------------------------------------------------

export function systemInfo(overrides: Partial<ISystemInfo> = {}): ISystemInfo {
  return {
    productName: 'NeuralAxe OS',
    productVersion: '0.1.0-dev',
    vendor: 'NeuralShield',
    targetDevice: 'Gamma',
    targetBoard: '601',
    targetAsic: 'BM1370',
    ASICModel: 'BM1370',
    boardVersion: '602',
    version: 'v2.14.2-39-gb3a16002',
    axeOSVersion: 'v2.14.2-39-gb3a16002',
    hashRate: 475,
    miningPaused: false,
    isUsingFallbackStratum: 0,
    sharesAccepted: 100,
    sharesRejected: 2,
    responseTime: 12,
    emergencyOverrideActive: 0,
    overheat_mode: 0,
    uptimeSeconds: 3600,
    stratumURL: 'solo.ckpool.org',
    stratumPort: 3333,
    stratumUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1',
    stratumProtocol: 'SV1',
    stratumTLS: 0,
    fallbackStratumURL: 'backup.example-pool.test',
    fallbackStratumPort: 3334,
    fallbackStratumUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig2',
    fallbackStratumProtocol: 'SV1',
    fallbackStratumTLS: 0,
    ...overrides,
  } as ISystemInfo;
}

/** Device sitting on the BTC CKPool primary (matches btcProfile). */
export function deviceOnBtc(): ISystemInfo {
  return systemInfo();
}

/** Device sitting on the BCH pool primary (matches bchProfile). */
export function deviceOnBch(): ISystemInfo {
  return systemInfo({
    stratumURL: 'bch.example-pool.test', stratumPort: 3334,
    stratumUser: 'bitcoincash:qsyntheticbchaddrexampleonly000000000.rig1',
  });
}

export function deviceMiningPaused(): ISystemInfo {
  return systemInfo({ miningPaused: true, hashRate: 0 });
}

export function deviceHostMismatch(): ISystemInfo {
  return systemInfo({ stratumURL: 'someone-else.example-pool.test' });
}

export function deviceFallbackActive(): ISystemInfo {
  return systemInfo({ isUsingFallbackStratum: 1 });
}

export function deviceEmergency(): ISystemInfo {
  return systemInfo({ emergencyOverrideActive: 1 });
}

export function deviceStockAxeos(): ISystemInfo {
  return systemInfo({ productName: '' as any });
}

export function deviceUnsupportedBoard(): ISystemInfo {
  return systemInfo({ targetBoard: '204' });
}
