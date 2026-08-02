/**
 * Gate B9 — PURE create-form tests.
 *
 * Duration conversion is deterministic, the operator-aid validation mirrors
 * the committed Gate B1 bounds, and the request object structurally cannot
 * carry a password, a source identity or a session id.
 */

import {
  ACK_CONFIRM_STATEMENTS, CHAIN_OPTIONS, CREATE_CONFIRM_STATEMENTS, DURATION_PRESETS,
  MAX_ACCOUNT_LEN, MAX_DURATION_S, MAX_HOST_LEN, MIN_DURATION_S, PROTOCOL_OPTIONS,
  RESTORE_CONFIRM_STATEMENTS, TLS_OPTIONS, TimedSessionFormValue,
  buildCreateRequest, buildReview, checkForm, emptyForm, fromDurationSeconds, toDurationSeconds,
} from './timed-session-form';

function form(overrides: Partial<TimedSessionFormValue> = {}): TimedSessionFormValue {
  return {
    ...emptyForm(),
    targetHost: 'bch.example',
    targetPort: 3334,
    targetAccount: 'acct.worker',
    ...overrides,
  };
}

describe('form: duration conversion', () => {
  it('converts hours and minutes deterministically', () => {
    expect(toDurationSeconds(1, 0)).toBe(3600);
    expect(toDurationSeconds(0, 15)).toBe(900);
    expect(toDurationSeconds(24, 0)).toBe(86400);
    expect(toDurationSeconds(2, 30)).toBe(9000);
  });

  it('treats missing or negative parts as zero and never clamps the total', () => {
    expect(toDurationSeconds(null, null)).toBe(0);
    expect(toDurationSeconds(-5, -5)).toBe(0);
    expect(toDurationSeconds(Number.NaN, 10)).toBe(600);
    // 25 h is NOT silently clamped — validation reports it instead.
    expect(toDurationSeconds(25, 0)).toBe(90000);
  });

  it('round-trips through fromDurationSeconds', () => {
    [900, 3600, 9000, 86400].forEach((s) => {
      const { hours, minutes } = fromDurationSeconds(s);
      expect(toDurationSeconds(hours, minutes)).withContext(String(s)).toBe(s);
    });
    expect(fromDurationSeconds(-1)).toEqual({ hours: 0, minutes: 0 });
  });

  it('every preset is inside the committed bounds', () => {
    DURATION_PRESETS.forEach((p) => {
      const s = toDurationSeconds(p.hours, p.minutes);
      expect(s).withContext(p.label).toBeGreaterThanOrEqual(MIN_DURATION_S);
      expect(s).withContext(p.label).toBeLessThanOrEqual(MAX_DURATION_S);
    });
  });
});

describe('form: validation', () => {
  it('accepts a complete valid form', () => {
    const c = checkForm(form());
    expect(c.valid).toBeTrue();
    expect(c.problems.length).toBe(0);
    expect(c.durationSeconds).toBe(3600);
  });

  it('accepts the exact minimum and maximum durations', () => {
    expect(checkForm(form({ hours: 0, minutes: 15 })).valid).toBeTrue();
    expect(checkForm(form({ hours: 24, minutes: 0 })).valid).toBeTrue();
  });

  it('rejects below and above the committed bounds', () => {
    const low = checkForm(form({ hours: 0, minutes: 14 }));
    expect(low.valid).toBeFalse();
    expect(low.problems[0].field).toBe('duration');
    expect(low.problems[0].message).toContain('15 minutes');

    const high = checkForm(form({ hours: 24, minutes: 1 }));
    expect(high.valid).toBeFalse();
    expect(high.problems[0].message).toContain('24 hours');
  });

  it('requires a bounded host', () => {
    expect(checkForm(form({ targetHost: '' })).problems.some((p) => p.field === 'targetHost')).toBeTrue();
    expect(checkForm(form({ targetHost: 'h'.repeat(MAX_HOST_LEN) })).valid).toBeTrue();
    expect(checkForm(form({ targetHost: 'h'.repeat(MAX_HOST_LEN + 1) })).valid).toBeFalse();
  });

  it('requires a bounded account', () => {
    expect(checkForm(form({ targetAccount: '' })).problems.some((p) => p.field === 'targetAccount')).toBeTrue();
    expect(checkForm(form({ targetAccount: 'a'.repeat(MAX_ACCOUNT_LEN) })).valid).toBeTrue();
    expect(checkForm(form({ targetAccount: 'a'.repeat(MAX_ACCOUNT_LEN + 1) })).valid).toBeFalse();
  });

  it('requires a whole port inside 1..65535', () => {
    expect(checkForm(form({ targetPort: null })).valid).toBeFalse();
    expect(checkForm(form({ targetPort: 0 })).valid).toBeFalse();
    expect(checkForm(form({ targetPort: 65536 })).valid).toBeFalse();
    expect(checkForm(form({ targetPort: 3333.5 })).valid).toBeFalse();
    expect(checkForm(form({ targetPort: 1 })).valid).toBeTrue();
    expect(checkForm(form({ targetPort: 65535 })).valid).toBeTrue();
  });

  it('reports — never silently removes — surrounding whitespace, because it is part of the identity', () => {
    const c = checkForm(form({ targetHost: ' bch.example ' }));
    expect(c.valid).toBeFalse();
    expect(c.problems.find((p) => p.field === 'targetHost')!.message).toContain('not removed for you');
    // The value itself is untouched.
    expect(buildCreateRequest(form({ targetHost: ' bch.example ' })).target_host).toBe(' bch.example ');
  });

  it('rejects unsupported enum values', () => {
    expect(checkForm(form({ targetProtocol: 'stratum_v3' as never })).valid).toBeFalse();
    expect(checkForm(form({ targetTlsMode: 'custom' as never })).valid).toBeFalse();
    expect(checkForm(form({ targetChain: 'dogecoin' as never })).valid).toBeFalse();
  });

  it('offers only backend-supported enums, and never a custom certificate', () => {
    expect(PROTOCOL_OPTIONS.map((o) => o.value)).toEqual(['stratum_v1', 'stratum_v2']);
    expect(TLS_OPTIONS.map((o) => o.value)).toEqual(['disabled', 'bundled']);
    expect(TLS_OPTIONS.some((o) => String(o.value).includes('custom'))).toBeFalse();
    expect(CHAIN_OPTIONS.map((o) => o.value)).toEqual(['bitcoin', 'bitcoin_cash', 'custom_unknown']);
  });
});

describe('form: request construction', () => {
  it('produces exactly the eight committed fields and nothing else', () => {
    const body = buildCreateRequest(form());
    expect(Object.keys(body).sort()).toEqual([
      'duration_seconds', 'target_account', 'target_chain', 'target_host',
      'target_port', 'target_protocol', 'target_tls_mode',
    ]);
  });

  it('carries no password, source or session-identifier key by construction', () => {
    const json = JSON.stringify(buildCreateRequest(form())).toLowerCase();
    ['password', 'passwd', 'pwd', 'secret', 'token', 'source', 'session_id', 'sessionid',
      'lease', 'generation', 'restore_required', 'certificate'].forEach((k) => {
      expect(json).withContext(k).not.toContain(k);
    });
  });

  it('an empty form still produces a well-formed (but invalid) request', () => {
    const body = buildCreateRequest(emptyForm());
    expect(body.target_port).toBe(0);
    expect(checkForm(emptyForm()).valid).toBeFalse();
  });
});

describe('form: review rows and confirmation statements', () => {
  it('reviews exactly what the operator entered', () => {
    const rows = buildReview(form({ hours: 2, minutes: 30 }));
    const byLabel = new Map(rows.map((r) => [r.label, r.value]));
    expect(byLabel.get('Duration')).toBe('2 h 30 m (9000 s)');
    expect(byLabel.get('Target host')).toBe('bch.example');
    expect(byLabel.get('Target port')).toBe('3334');
    expect(byLabel.get('Target account')).toBe('acct.worker');
    expect(byLabel.get('Protocol')).toBe('Stratum V1');
    expect(byLabel.get('TLS')).toBe('Disabled');
    expect(byLabel.get('Chain')).toBe('Bitcoin');
    // No source, password or session row exists.
    expect(rows.some((r) => /source|password|session/i.test(r.label))).toBeFalse();
  });

  it('formats a sub-hour duration without an hours component', () => {
    const rows = buildReview(form({ hours: 0, minutes: 15 }));
    expect(rows[0].value).toBe('15 m (900 s)');
  });

  it('the create statements cover the mandatory disclosures', () => {
    const all = CREATE_CONFIRM_STATEMENTS.join(' ');
    expect(all).toContain('captures its OWN current pool configuration internally');
    expect(all).toContain('password is kept exactly as it is');
    expect(all).toContain('transactional');
    expect(all).toContain('mandatory');
    expect(all).toContain('cannot be edited afterwards');
    expect(all).toContain('not a hardware performance-tuning operation');
  });

  it('the restore statements explain the stop and the return', () => {
    const all = RESTORE_CONFIRM_STATEMENTS.join(' ');
    expect(all).toContain('Mining on the target pool stops');
    expect(all).toContain('returning to the pool configuration');
    expect(all).toContain('never resent automatically');
  });

  it('the acknowledgement statements make the limited effect explicit', () => {
    const all = ACK_CONFIRM_STATEMENTS.join(' ');
    expect(all).toContain('cleared from the device');
    expect(all).toContain('pool configuration is not changed');
    expect(all).toContain('durably proven');
    // It must not over-claim about audit logs.
    expect(all.toLowerCase()).not.toContain('audit log');
  });
});
