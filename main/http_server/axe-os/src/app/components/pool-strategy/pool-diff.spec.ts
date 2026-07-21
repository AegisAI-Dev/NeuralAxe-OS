import { buildSwitchReview, SWITCH_CONFIRM_STATEMENTS, tlsLabel } from './pool-diff';
import { deriveChainContext, ActivePoolRecord } from './pool-chain';
import { bchProfile, btcProfile, endpoint, profile, secretProfile, systemInfo } from './pool-fixtures';

function btcRecord(): ActivePoolRecord {
  return {
    profileId: 'p1', profileName: 'BTC Solo CKPool', chain: 'BTC',
    primaryHost: 'solo.ckpool.org', primaryPort: 3333,
    primaryUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1', appliedAt: 1,
  };
}

describe('pool-diff', () => {
  it('labels TLS modes', () => {
    expect(tlsLabel(0)).toContain('No TLS');
    expect(tlsLabel(1)).toContain('system');
    expect(tlsLabel(2)).toContain('custom');
  });

  it('exposes the required confirm statements including no-funds and BTC/BCH payout', () => {
    const joined = SWITCH_CONFIRM_STATEMENTS.join(' ');
    expect(joined).toContain('does not convert');
    expect(joined).toContain('rolls back');
    expect(joined.toLowerCase()).toContain('reconnect');
    expect(joined).toContain('payout');
  });

  it('builds a masked review that never exposes a full account or password', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    const review = buildSwitchReview(systemInfo(), ctx, bchProfile());
    const flat = JSON.stringify(review);
    // The synthetic BCH account/worker must be masked out of the review.
    expect(flat).not.toContain('bitcoincash:qsyntheticbchaddrexampleonly000000000');
    const userRow = review.primary.find(r => r.field === 'user');
    expect(userRow?.toDisplay).not.toContain('qsyntheticbchaddr');
    const passRow = review.primary.find(r => r.field === 'password');
    expect(passRow?.toDisplay).toMatch(/keep|replace/);
  });

  it('detects a chain change and warns about payout details', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    const review = buildSwitchReview(systemInfo(), ctx, bchProfile());
    expect(review.chainChanges).toBeTrue();
    expect(review.warnings.some(w => /BTC and BCH/.test(w))).toBeTrue();
  });

  it('warns when the fallback will be mirrored from the primary', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    const review = buildSwitchReview(systemInfo(), ctx, profile({ fallback: null }));
    expect(review.fallbackMirrored).toBeTrue();
    expect(review.warnings.some(w => /mirrored/.test(w))).toBeTrue();
  });

  it('reports credential replacement and warns the password is entered at switch time', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    const review = buildSwitchReview(systemInfo(), ctx, secretProfile());
    expect(review.credentialsReplaced.primary).toBeTrue();
    expect(review.credentialsReplaced.fallback).toBeTrue();
    expect(review.warnings.some(w => /kept as-is/.test(w))).toBeFalse();
    expect(review.warnings.some(w => /replaces a pool password/.test(w))).toBeTrue();
    expect(review.warnings.some(w => /stored anywhere/i.test(w))).toBeTrue();
  });

  it('warns that the password is kept for a keep-mode profile', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    const review = buildSwitchReview(systemInfo(), ctx, btcProfile());
    expect(review.credentialsReplaced.primary).toBeFalse();
    expect(review.warnings.some(w => /kept as-is/.test(w))).toBeTrue();
  });

  it('detects no meaningful change when the profile matches the device', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    // Profile identical to the current device primary/fallback.
    const same = profile({
      chain: 'BTC',
      primary: endpoint({ host: 'solo.ckpool.org', port: 3333, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1' }),
      fallback: endpoint({ host: 'backup.example-pool.test', port: 3334, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig2' }),
    });
    const review = buildSwitchReview(systemInfo(), ctx, same);
    expect(review.anyChange).toBeFalse();
  });

  it('always requires a restart', () => {
    const ctx = deriveChainContext(btcRecord(), systemInfo());
    expect(buildSwitchReview(systemInfo(), ctx, bchProfile()).restartExpected).toBeTrue();
  });
});
