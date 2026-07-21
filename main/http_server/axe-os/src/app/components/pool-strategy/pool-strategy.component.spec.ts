import { ComponentFixture, TestBed } from '@angular/core/testing';
import { NO_ERRORS_SCHEMA } from '@angular/core';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { BehaviorSubject, of, throwError } from 'rxjs';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';

import { PoolStrategyComponent } from './pool-strategy.component';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { PoolStrategyService } from 'src/app/services/pool-strategy.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { bchProfile, secretProfile, systemInfo, SECRET_PASSWORDS, SECRET_TOKENS } from './pool-fixtures';
import { PoolProfile } from './pool-profile';

const POOL_KEYS = ['NX_POOL_PROFILES', 'NX_POOL_ACTIVE', 'NX_POOL_RESTORE_SNAPSHOT', 'NX_POOL_SWITCH_HISTORY', 'NX_POOL_SWITCH_ACTIVE', 'NX_STABILITY_ACTIVE'];

let fixtures: ComponentFixture<PoolStrategyComponent>[] = [];

interface Ctx {
  fixture: ComponentFixture<PoolStrategyComponent>;
  component: PoolStrategyComponent;
  system: SystemApiService;
  pool: PoolStrategyService;
  info$: BehaviorSubject<ISystemInfo>;
  mono: { v: number };
}

function sampleInterruptionRecord(passwordWasReplaced = false) {
  return {
    sessionId: 's', at: 1, targetProfileName: 'BCH Demo', targetChain: 'BCH',
    original: {
      primary: { host: 'solo.ckpool.org', port: 3333, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1', passwordMode: 'keep', tls: 0, protocol: 'SV1' },
      fallback: { host: 'backup.example-pool.test', port: 3334, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig2', passwordMode: 'keep', tls: 0, protocol: 'SV1' },
      useFallbackStratum: 0,
    },
    target: { host: 'bch.example-pool.test', port: 3334, user: 'x' },
    passwordWasReplaced,
  };
}

function build(initial: ISystemInfo = systemInfo(), opts: { applyError?: boolean; interruptionRecord?: any } = {}): Ctx {
  POOL_KEYS.forEach(k => window.localStorage.removeItem(k));
  const info$ = new BehaviorSubject<ISystemInfo>(initial);
  TestBed.configureTestingModule({
    declarations: [PoolStrategyComponent],
    imports: [NoopAnimationsModule],
    providers: [
      provideHttpClient(), provideToastr(),
      { provide: LiveDataService, useValue: { info$, connected$: of(true) } },
      { provide: WebVersionService, useValue: { installedWebVersion$: of(initial.version) } },
    ],
    schemas: [NO_ERRORS_SCHEMA],
  });
  const system = TestBed.inject(SystemApiService);
  spyOn(system, 'updateSystem').and.returnValue(opts.applyError ? throwError(() => new Error('apply failed')) : of(undefined));
  spyOn(system, 'restart').and.returnValue(of({ message: 'ok' } as any));
  const pool = TestBed.inject(PoolStrategyService);
  const fixture = TestBed.createComponent(PoolStrategyComponent);
  const component = fixture.componentInstance;
  const mono = { v: 1000 };
  (component as any).nowMono = () => mono.v;
  // Simulate a browser-interrupted switch BEFORE ngOnInit reads the record.
  if (opts.interruptionRecord) {
    window.localStorage.setItem('NX_POOL_SWITCH_ACTIVE', JSON.stringify(opts.interruptionRecord));
  }
  fixture.detectChanges(); // ngOnInit
  fixtures.push(fixture);
  return { fixture, component, system, pool, info$, mono };
}

function ackAll(c: PoolStrategyComponent) {
  c.acks = { interrupt: true, change: true, rollback: true, funds: true };
}

/** The PATCH bodies submitted to updateSystem, in order. */
function bodies(system: SystemApiService): any[] {
  return (system.updateSystem as unknown as jasmine.Spy).calls.allArgs().map(args => args[1]);
}

/** Fill the confirm-dialog secret form with the session passwords. */
function setSecrets(c: PoolStrategyComponent, over: Partial<{ target: string; targetFallback: string; current: string; keepOverride: boolean }> = {}) {
  c.pwd = { target: SECRET_PASSWORDS.targetPrimary, targetFallback: SECRET_PASSWORDS.targetFallback, current: SECRET_PASSWORDS.original, keepOverride: false, ...over };
}

/** Drive a device that reconnected on the given profile's primary, to completion. */
function reconnectOn(ctx: Ctx, p: PoolProfile) {
  const on = systemInfo({ stratumURL: p.primary.host, stratumPort: p.primary.port!, stratumUser: p.primary.user, sharesAccepted: 5 });
  ctx.mono.v += 5; ctx.info$.next(on);
  ctx.mono.v += 5; ctx.info$.next(on);
}

afterEach(() => {
  fixtures.forEach(f => { try { f.destroy(); } catch { /* ignore */ } });
  fixtures = [];
  POOL_KEYS.forEach(k => window.localStorage.removeItem(k));
  TestBed.resetTestingModule();
});

describe('PoolStrategyComponent', () => {
  it('creates', () => {
    expect(build().component).toBeTruthy();
  });

  it('saves a new profile through the editor', () => {
    const { component, pool } = build();
    component.openNewProfile();
    component.editor.name = 'My BTC';
    component.editor.chain = 'BTC';
    component.editor.primary.host = 'pool.example.test';
    component.editor.primary.port = 3333;
    component.editor.primary.user = 'bc1qexample.rig';
    expect(component.canSaveEditor()).toBeTrue();
    component.saveEditor();
    expect(pool.profiles.map(p => p.name)).toContain('My BTC');
    expect(component.editorOpen).toBeFalse();
  });

  it('blocks review when the target does not differ from the current configuration', () => {
    const { component, pool } = build();
    const same = pool.addProfile({
      name: 'Same', chain: 'BTC',
      primary: { host: 'solo.ckpool.org', port: 3333, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1', passwordMode: 'keep', tls: 0, protocol: 'SV1' },
      fallback: { host: 'backup.example-pool.test', port: 3334, user: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig2', passwordMode: 'keep', tls: 0, protocol: 'SV1' },
    });
    component.reviewSwitch(same);
    expect(component.showConfirm).toBeFalse();
    expect(component.preflightResult?.blockers.some(b => b.id === 'differs')).toBeTrue();
  });

  it('opens the confirm dialog when preflight passes', () => {
    const { component, pool } = build();
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    expect(component.preflightResult?.canStart).toBeTrue();
    expect(component.showConfirm).toBeTrue();
    expect(component.snapshot.state).toBe('awaiting-confirmation');
  });

  it('reaches reconnecting after a confirmed apply + restart', () => {
    const { component, pool } = build();
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch();
    expect(component.snapshot.state).toBe('reconnecting');
    expect(pool.wasSwitchInterrupted()).toBeTrue();
  });

  it('completes a verified switch and records the active chain context', () => {
    const ctx = build();
    const { component, pool, info$, mono } = ctx;
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch();
    expect(component.snapshot.state).toBe('reconnecting');

    // Device reconnects on the new BCH pool.
    const onBch = systemInfo({ stratumURL: bp.primary.host, stratumPort: bp.primary.port!, stratumUser: bp.primary.user, sharesAccepted: 5 });
    mono.v = 1005; info$.next(onBch);          // → verifying
    expect(component.snapshot.state).toBe('verifying');
    mono.v = 1010; info$.next(onBch);          // → verified → complete
    expect(component.snapshot.state).toBe('complete');
    expect(component.snapshot.switchVerified).toBeTrue();
    expect(pool.chainContext().chain).toBe('BCH');
    expect(pool.chainContext().verified).toBeTrue();
    expect(pool.getRestoreSnapshot()).toBeTruthy();
    expect(pool.wasSwitchInterrupted()).toBeFalse();
  });

  it('rolls back and ends failed when the apply fails', () => {
    const { component, pool } = build(systemInfo(), { applyError: true }); // updateSystem throws
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch();
    // apply fails → rolling-back → rollback apply also fails → failed
    expect(component.snapshot.state).toBe('failed');
    expect(component.snapshot.rollbackResult).toBe('failed');
    expect(pool.wasSwitchInterrupted()).toBeFalse();
  });

  it('keeps session passwords out of every persisted store (Blocker 2 / Stage 14 scenario 24)', () => {
    const ctx = build();
    const { component, pool } = ctx;
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    setSecrets(component);
    component.confirmSwitch();
    reconnectOn(ctx, sp);
    expect(component.snapshot.state).toBe('complete');

    // None of the session password sentinels may appear in ANY persisted store.
    const stores = [
      JSON.stringify(pool.listHistory()),
      window.localStorage.getItem('NX_POOL_PROFILES') || '',
      window.localStorage.getItem('NX_POOL_RESTORE_SNAPSHOT') || '',
      window.localStorage.getItem('NX_POOL_ACTIVE') || '',
      window.localStorage.getItem('NX_POOL_SWITCH_ACTIVE') || '',
    ];
    stores.forEach(s => SECRET_TOKENS.forEach(t => expect(s).not.toContain(t)));
    // The replacement is recorded as a boolean only.
    expect(pool.listHistory()[0].credentialsReplaced.primary).toBeTrue();
  });

  // ---- Blocker 3: PATCH payload proofs ----

  it('keep-mode switch omits both password fields in the PATCH body', () => {
    const { component, pool } = build();
    const bp = pool.addProfile(dropId(bchProfile())); // keep mode
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch();
    const applyBody = bodies(component['systemService'])[0];
    expect('stratumPassword' in applyBody).toBeFalse();
    expect('fallbackStratumPassword' in applyBody).toBeFalse();
  });

  it('set-mode switch sends the owner-entered target password only', () => {
    const ctx = build();
    const { component, pool } = ctx;
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    setSecrets(component);
    component.confirmSwitch();
    const applyBody = bodies(component['systemService'])[0];
    expect(applyBody.stratumPassword).toBe(SECRET_PASSWORDS.targetPrimary);
    expect(applyBody.fallbackStratumPassword).toBe(SECRET_PASSWORDS.targetFallback);
    // Never a masked/placeholder value.
    expect(applyBody.stratumPassword).not.toMatch(/\*|•|hidden/);
  });

  it('rollback sends the original password only when the owner supplied it this session', () => {
    const ctx = build(systemInfo(), { applyError: false });
    const { component, pool } = ctx;
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    setSecrets(component);
    component.confirmSwitch();
    // Never reconnects on the target → verify times out → rollback.
    for (let i = 0; i < 40; i++) { ctx.mono.v += 10000; (component as any).onHeartbeat(); }
    expect(component.snapshot.state).toBe('failed');
    const all = bodies(component['systemService']);
    const rollbackBody = all[all.length - 1];
    // The rollback restores the ORIGINAL password the owner entered for this session.
    expect(rollbackBody.stratumPassword).toBe(SECRET_PASSWORDS.original);
  });

  it('a keep switch never sends a password on rollback', () => {
    const ctx = build();
    const { component, pool } = ctx;
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch();
    for (let i = 0; i < 40; i++) { ctx.mono.v += 10000; (component as any).onHeartbeat(); }
    expect(component.snapshot.state).toBe('failed');
    bodies(component['systemService']).forEach(b => {
      expect('stratumPassword' in b).toBeFalse();
      expect('fallbackStratumPassword' in b).toBeFalse();
    });
  });

  // ---- Blocker 1/2: replace-password gate ----

  it('blocks a replace-password switch until the current password is entered', () => {
    const { component, pool } = build();
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    // No secrets entered yet.
    expect(component.secretsSatisfied).toBeFalse();
    expect(component.replacePasswordBlockerText).toContain('current password cannot be read from the device');
    component.confirmSwitch();               // must NOT proceed
    expect(component.snapshot.state).toBe('awaiting-confirmation');
  });

  it('Keep-current-password override lets a set profile switch without secrets (no replacement)', () => {
    const { component, pool } = build();
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    component.useKeepCurrentPassword();
    expect(component.secretsSatisfied).toBeTrue();
    expect(component.replacePasswordBlockerText).toBeNull();
    component.confirmSwitch();
    expect(component.snapshot.state).toBe('reconnecting');
    const applyBody = bodies(component['systemService'])[0];
    expect('stratumPassword' in applyBody).toBeFalse(); // kept, not replaced
  });

  // ---- Blocker 4: interruption recovery ----

  it('records a NON-SECRET interruption record while a switch is active', () => {
    const { component, pool } = build();
    const sp = pool.addProfile(dropId(secretProfile()));
    component.reviewSwitch(sp);
    ackAll(component);
    setSecrets(component);
    component.confirmSwitch();
    const rec = pool.getInterruptionRecord();
    expect(rec).toBeTruthy();
    const flat = JSON.stringify(rec);
    SECRET_TOKENS.forEach(t => expect(flat).not.toContain(t));
    expect(rec!.passwordWasReplaced).toBeTrue();
  });

  it('after reload with an unresolved switch, shows interrupted recovery (never Complete)', () => {
    const { component } = build(systemInfo(), { interruptionRecord: sampleInterruptionRecord(true) });
    expect(component.interruptedNotice).toBeTrue();
    expect(component.interruptionRecovery).toBeTruthy();
    expect(component.snapshot.state).not.toBe('complete');
    expect(component.interruptionRecovery!.situation).toBe('on-original'); // device is on solo.ckpool.org
    expect(component.interruptionRecovery!.originalPasswordRecoverable).toBeFalse(); // password was replaced
    expect(component.interruptionRecovery!.passwordNote).toBeTruthy();
  });

  it('interruption recovery: retry rollback reapplies the original config with NO password', () => {
    const ctx = build(systemInfo({ stratumURL: 'bch.example-pool.test', stratumPort: 3334, stratumUser: 'x' }), { interruptionRecord: sampleInterruptionRecord(true) });
    const { component } = ctx;
    expect(component.interruptionRecovery!.situation).toBe('on-target');
    component.retryInterruptionRollback();
    expect(component.snapshot.state).toBe('restoring');
    // The reapply body carries the original host/user but NEVER a password.
    const body = bodies(component['systemService'])[0];
    expect(body.stratumURL).toBe('solo.ckpool.org');
    expect('stratumPassword' in body).toBeFalse();
  });

  it('shows the interrupted notice when a previous switch was cut off', () => {
    const { component } = build(systemInfo(), { interruptionRecord: sampleInterruptionRecord() });
    expect(component.interruptedNotice).toBeTrue();
  });

  it('canDeactivate is true when idle and confirms while active', () => {
    const { component, pool } = build();
    expect(component.canDeactivate()).toBeTrue();
    const bp = pool.addProfile(dropId(bchProfile()));
    component.reviewSwitch(bp);
    ackAll(component);
    component.confirmSwitch(); // → reconnecting (active)
    spyOn(window, 'confirm').and.returnValue(true);
    expect(component.canDeactivate()).toBeTrue();
    expect(window.confirm).toHaveBeenCalled();
  });
});

/** Strip id/timestamps so the fixture profile can be added through the service. */
function dropId(p: PoolProfile): Omit<PoolProfile, 'id' | 'createdAt' | 'updatedAt'> {
  const { id, createdAt, updatedAt, ...rest } = p;
  return rest;
}
