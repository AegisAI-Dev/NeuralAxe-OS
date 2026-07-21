import {
  RESTORE_TTL_MS, buildRestoreSnapshot, buildSwitchRecord, captureConfig, captureToSettings,
  containsForbiddenKeys, exportJson, exportMarkdown, isRestoreValid, maskedHostChange,
  PoolSwitchRecord,
} from './pool-history';
import { deviceFallbackActive, secretProfile, systemInfo, SECRET_TOKENS } from './pool-fixtures';
import { fallbackProvided, PoolEndpoint } from './pool-profile';

describe('pool-history', () => {
  describe('capture', () => {
    it('captures primary + fallback + the preference flag', () => {
      const cfg = captureConfig(deviceFallbackActive());
      expect(cfg.primary.host).toBe('solo.ckpool.org');
      expect(cfg.fallback.host).toBe('backup.example-pool.test');
      expect(cfg.useFallbackStratum).toBe(1);
    });
    it('builds a restore PATCH without any password by default and restores the flag', () => {
      const body: any = captureToSettings(captureConfig(deviceFallbackActive()));
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
      expect(body.useFallbackStratum).toBe(1);
      expect(body.stratumURL).toBe('solo.ckpool.org');
    });
    it('reinstates the original password ONLY for the pools the switch replaced', () => {
      const cfg = captureConfig(systemInfo());
      const both: any = captureToSettings(cfg, { originalPassword: 'ORIG', restorePrimary: true, restoreFallback: true });
      expect(both.stratumPassword).toBe('ORIG');
      expect(both.fallbackStratumPassword).toBe('ORIG');
      const primaryOnly: any = captureToSettings(cfg, { originalPassword: 'ORIG', restorePrimary: true, restoreFallback: false });
      expect(primaryOnly.stratumPassword).toBe('ORIG');
      expect('fallbackStratumPassword' in primaryOnly).toBeFalse();
    });
    it('never sends an empty/masked original password even when flagged', () => {
      const body: any = captureToSettings(captureConfig(systemInfo()), { originalPassword: '', restorePrimary: true, restoreFallback: true });
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
    });
  });

  describe('restore snapshot', () => {
    it('builds with a default TTL and validates by expiry', () => {
      const now = 1_000_000;
      const snap = buildRestoreSnapshot({ config: captureConfig(systemInfo()), now, fromProfileName: 'BCH', passwordWasReplaced: true });
      expect(snap.expiresAt).toBe(now + RESTORE_TTL_MS);
      expect(isRestoreValid(snap, now + 1000)).toBeTrue();
      expect(isRestoreValid(snap, now + RESTORE_TTL_MS + 1)).toBeFalse();
      expect(isRestoreValid(null, now)).toBeFalse();
    });
  });

  describe('masked host change', () => {
    it('masks hosts and reports whether they changed', () => {
      const from = captureConfig(systemInfo()).primary;
      const to: PoolEndpoint = { ...from, host: 'new.example-pool.test', port: 3399 };
      const change = maskedHostChange('primary', from, to);
      expect(change.changed).toBeTrue();
      expect(change.hostFromMasked).not.toContain('ckpool'); // masked
      expect(change.portTo).toBe(3399);
    });
  });

  function secretRecord(): PoolSwitchRecord {
    const sp = secretProfile();
    const original = captureConfig(systemInfo());
    return buildSwitchRecord({
      id: 'sess-1', startedAt: 1, finishedAt: 2,
      source: { profileId: null, profileName: 'BTC Solo', chain: 'BTC' },
      target: { profileId: sp.id, profileName: sp.name, chain: sp.chain },
      changes: [
        maskedHostChange('primary', original.primary, sp.primary),
        maskedHostChange('fallback', original.fallback, fallbackProvided(sp.fallback) ? (sp.fallback as PoolEndpoint) : sp.primary),
      ],
      credentialsReplaced: { primary: true, fallback: true },
      finalState: 'complete', switchVerified: true, rollbackResult: null, restoreResult: null, reason: null,
      timeline: [{ state: 'complete', at: 2, note: 'done' }],
      device: { productName: 'NeuralAxe OS', targetBoard: '601', targetAsic: 'BM1370', firmware: 'v2.14.2' },
    });
  }

  describe('privacy (Stage 14 scenario 24)', () => {
    it('a record built from secret credentials contains no forbidden keys', () => {
      expect(containsForbiddenKeys(secretRecord())).toBeFalse();
    });
    it('JSON and Markdown exports never contain wallet / worker / password values', () => {
      const rec = secretRecord();
      const json = exportJson([rec]);
      const md = exportMarkdown([rec]);
      SECRET_TOKENS.forEach(token => {
        expect(json).not.toContain(token);
        expect(md).not.toContain(token);
      });
    });
    it('the forbidden-key guard catches an injected sensitive key', () => {
      expect(containsForbiddenKeys({ stratumPassword: 'x' })).toBeTrue();
      expect(containsForbiddenKeys({ nested: [{ wallet: 'x' }] })).toBeTrue();
      expect(containsForbiddenKeys({ ok: 1, note: 'fine', host: 'masked' })).toBeFalse();
    });
    it('exports carry the disclaimer', () => {
      expect(exportJson([secretRecord()])).toContain('does not cryptographically');
      expect(exportMarkdown([secretRecord()])).toContain('does not cryptographically');
    });
  });

  describe('store bounds', () => {
    it('markdown handles an empty history', () => {
      expect(exportMarkdown([])).toContain('No switch sessions recorded');
    });
  });
});
