import { ComponentFixture, TestBed, fakeAsync, tick, discardPeriodicTasks } from '@angular/core/testing';
import { NO_ERRORS_SCHEMA } from '@angular/core';
import { of, BehaviorSubject } from 'rxjs';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';

import { StabilityLabComponent } from './stability-lab.component';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { StabilityProfile } from './stability-profile';
import { FRESHNESS_LIMIT_MS, SESSION_RECONNECT_GRACE_MS } from './stability-freshness';

const supportedInfo = (over: Partial<ISystemInfo> = {}): ISystemInfo => ({
  productName: 'NeuralAxe OS', productVersion: '0.1.0-dev', vendor: 'NeuralShield',
  targetDevice: 'Gamma', targetBoard: '601', targetAsic: 'BM1370', ASICModel: 'BM1370',
  version: 'v2.14.2-31', axeOSVersion: 'v2.14.2-31',
  frequency: 485, coreVoltage: 1200, thermalControlMode: 'target', temptarget: 60, minFanSpeed: 25,
  temp: 60, vrTemp: 45, controlSensorValid: 1, emergencyOverrideActive: 0, overheat_mode: 0,
  miningPaused: false, hashRate: 1290, power: 21, fanspeed: 55, errorPercentage: 0.5,
  sharesAccepted: 1000, sharesRejected: 5, hostname: 'gamma-lab-01',
  ...over,
} as unknown as ISystemInfo);

const asicMock = {
  ASICModel: 'BM1370', deviceModel: 'Gamma', asicCount: 1,
  defaultFrequency: 485, frequencyOptions: [400, 485, 500, 575],
  defaultVoltage: 1200, voltageOptions: [1100, 1150, 1200, 1300],
};

let fixtures: ComponentFixture<StabilityLabComponent>[] = [];

function build(info$: any, connected$ = of(true)) {
  window.localStorage.removeItem('NX_STABILITY_ACTIVE');
  window.localStorage.removeItem('NX_STABILITY_SESSIONS');
  window.localStorage.removeItem('NX_STABILITY_THRESHOLDS');
  TestBed.configureTestingModule({
    declarations: [StabilityLabComponent],
    providers: [
      provideHttpClient(), provideToastr(),
      { provide: LiveDataService, useValue: { info$, connected$ } },
      { provide: WebVersionService, useValue: { installedWebVersion$: of(null) } },
    ],
    schemas: [NO_ERRORS_SCHEMA],
  });
  const system = TestBed.inject(SystemApiService);
  spyOn(system, 'getAsicSettings').and.returnValue(of(asicMock as any));
  spyOn(system, 'updateSystem').and.returnValue(of(undefined));
  spyOn(system, 'restart').and.returnValue(of({ message: 'ok' } as any));
  const fixture = TestBed.createComponent(StabilityLabComponent);
  return { fixture, component: fixture.componentInstance, system };
}

/** Build, override the monotonic clock BEFORE ngOnInit, then run ngOnInit. */
function buildWithClock(info$: any, monoRef: { v: number }) {
  const ctx = build(info$);
  (ctx.component as any).nowMono = () => monoRef.v;
  ctx.fixture.detectChanges(); // ngOnInit uses the overridden clock
  fixtures.push(ctx.fixture);
  return ctx;
}

function create(info: ISystemInfo = supportedInfo()) {
  const ctx = build(of(info));
  ctx.fixture.detectChanges();
  fixtures.push(ctx.fixture);
  return ctx;
}

afterEach(() => {
  fixtures.forEach(f => { try { f.destroy(); } catch { /* ignore */ } });
  fixtures = [];
  TestBed.resetTestingModule();
});

describe('StabilityLabComponent (setup + gating)', () => {
  it('creates and recognizes a supported NeuralAxe Gamma 601 device', () => {
    const { component } = create();
    expect(component).toBeTruthy();
    expect(component.supported.supported).toBeTrue();
    expect(component.baseline).not.toBeNull();
    expect(component.baseline!.frequency).toBe(485);
    expect(component.online).toBeTrue(); // fresh telemetry
  });

  it('reports a stock AxeOS device as read-only (no execution)', () => {
    const { component } = create(supportedInfo({ productName: undefined } as any));
    expect(component.supported.supported).toBeFalse();
    expect(component.supported.kind).toBe('stock-axeos');
  });

  it('blocks an unsupported board 702 device', () => {
    const { component } = create(supportedInfo({ targetBoard: '702' } as any));
    expect(component.supported.supported).toBeFalse();
    expect(component.supported.label).toContain('702');
  });

  it('builds starter profiles from the served option lists', () => {
    const { component } = create();
    expect(component.starters.length).toBeGreaterThan(0);
    expect(component.starters[0].key).toBe('current');
  });

  it('adds a valid editor profile and rejects a duplicate', () => {
    const { component } = create();
    component.editor.name = 'Perf';
    component.editor.frequency = 575;
    component.editor.coreVoltage = 1300;
    component.editor.thermalControlMode = 'target';
    expect(component.canAddProfile()).toBeTrue();
    component.addEditorProfile();
    expect(component.profiles.length).toBe(1);
    expect(component.editorDuplicate()).toBeTrue();
    expect(component.canAddProfile()).toBeFalse();
  });

  it('surfaces validation errors for a bad profile', () => {
    const { component } = create();
    component.editor.name = '';
    expect(component.editorErrors().length).toBeGreaterThan(0);
  });

  it('clamps stop thresholds instead of accepting unsafe values', () => {
    const { component } = create();
    component.setAsicStop(95);
    expect(component.thresholds.asicC).toBe(70);
    component.setVrmStop(130);
    expect(component.thresholds.vrmC).toBe(105);
    component.setThresholds({ debounceSamples: 100 });
    expect(component.thresholds.debounceSamples).toBeLessThanOrEqual(6);
  });

  it('reports a real 0 planned restarts from the audited field diff', () => {
    const { component } = create();
    component.addStarter(component.starters.find(s => s.key !== 'current') ?? component.starters[0]);
    expect(component.plannedRestarts()).toBe(0);
    expect(component.restartAuditRows.every(r => r.live)).toBeTrue();
  });

  it('passes preflight and opens the confirmation only when a profile is queued', () => {
    const { component } = create();
    component.recomputePreflight();
    expect(component.preflightResult!.canStart).toBeFalse();
    component.addStarter(component.starters.find(s => s.key !== 'current') ?? component.starters[0]);
    component.recomputePreflight();
    expect(component.preflightResult!.canStart).toBeTrue();
    component.reviewSession();
    expect(component.showConfirm).toBeTrue();
    expect(component.snapshot.state).toBe('awaiting-confirmation');
  });

  it('will not start until every acknowledgement is checked', () => {
    const { component } = create();
    component.addStarter(component.starters[0]);
    component.reviewSession();
    expect(component.allAcked).toBeFalse();
    component.startSession();
    expect(component.snapshot.state).toBe('awaiting-confirmation');
    component.acks = { settings: true, restart: true, interrupt: true, protection: true, restore: true };
    expect(component.allAcked).toBeTrue();
  });

  it('masks the device hostname', () => {
    const { component } = create();
    expect(component.maskHost('gamma-lab-01')).not.toBe('');
    expect(component.maskHost(null)).toBe('—');
  });

  it('flags an interrupted previous session and clears the stale flag', () => {
    const { fixture, component } = build(of(supportedInfo()));
    // Set the active flag AFTER build() (which clears state) but BEFORE
    // detectChanges runs ngOnInit, which is where interruption is detected.
    window.localStorage.setItem('NX_STABILITY_ACTIVE', JSON.stringify({ id: 'x', startedAt: Date.now() }));
    fixture.detectChanges();
    fixtures.push(fixture);
    expect(component.interruptedNotice).toBeTrue();
    expect(window.localStorage.getItem('NX_STABILITY_ACTIVE')).toBeNull();
  });
});

describe('StabilityLabComponent (telemetry freshness)', () => {
  it('fresh telemetry passes the online preflight check', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component } = buildWithClock(info$, mono);
    mono.v = 102_000; // 2 s later
    (component as any).refreshFreshness();
    component.recomputePreflight();
    expect(component.freshness.state).toBe('fresh');
    expect(component.preflightResult!.checks.find(c => c.id === 'online')!.ok).toBeTrue();
  });

  it('fresh fallback-poll telemetry passes even with the WebSocket disconnected', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component } = buildWithClock(info$, mono);
    component.connected = false; // WS down…
    info$.next(supportedInfo({ hashRate: 1291 } as any)); // …but a fresh poll arrives
    mono.v = 101_000;
    (component as any).refreshFreshness();
    expect(component.online).toBeTrue();
  });

  it('retained telemetry becomes stale past the limit and blocks preflight', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component } = buildWithClock(info$, mono);
    component.addStarter(component.starters.find(s => s.key !== 'current') ?? component.starters[0]);
    mono.v = 100_000 + FRESHNESS_LIMIT_MS + 1000; // 16 s later, no new sample
    (component as any).refreshFreshness();
    component.recomputePreflight();
    expect(component.freshness.state).toBe('stale');
    expect(component.online).toBeFalse();
    expect(component.preflightResult!.canStart).toBeFalse();
    expect(component.preflightResult!.blockers.some(b => b.id === 'online')).toBeTrue();
  });

  it('a gap sample (no telemetry) does not refresh the receipt time', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component } = buildWithClock(info$, mono);
    const stamped = (component as any).lastFreshMonoMs;
    mono.v = 105_000;
    info$.next({ productName: 'NeuralAxe OS', targetBoard: '601', targetDevice: 'Gamma', targetAsic: 'BM1370', ASICModel: 'BM1370', version: 'v', axeOSVersion: 'v' } as any); // no hashrate/temp
    expect((component as any).lastFreshMonoMs).toBe(stamped); // unchanged by a gap
  });

  it('a component re-render never refreshes the receipt time', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    const stamped = (component as any).lastFreshMonoMs;
    mono.v = 108_000;
    fixture.detectChanges();
    fixture.detectChanges();
    expect((component as any).lastFreshMonoMs).toBe(stamped);
  });

  it('a genuinely new sample restores freshness after going stale', () => {
    const mono = { v: 100_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component } = buildWithClock(info$, mono);
    mono.v = 100_000 + FRESHNESS_LIMIT_MS + 5000;
    (component as any).refreshFreshness();
    expect(component.online).toBeFalse();
    info$.next(supportedInfo({ hashRate: 1288 } as any)); // new sample re-stamps at mono.v
    (component as any).refreshFreshness();
    expect(component.online).toBeTrue();
  });
});

describe('StabilityLabComponent (engine run + restore)', () => {
  const fastProfile = (warmupSec: number, measureSec: number): StabilityProfile => ({
    id: 'fast', name: 'Fast', frequency: 500, coreVoltage: 1200, thermalControlMode: 'target',
    temptarget: 60, minFanSpeed: 25, warmupSec, measureSec, cooldownSec: 0,
  });

  function startFast(component: StabilityLabComponent, warmupSec: number, measureSec: number) {
    component.profiles = [fastProfile(warmupSec, measureSec)];
    component.recomputePreflight();
    component.reviewSession();
    component.acks = { settings: true, restart: true, interrupt: true, protection: true, restore: true };
    component.startSession();
  }

  it('runs a short session end-to-end and restores the original configuration', fakeAsync(() => {
    const mono = { v: 500_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const ctx = buildWithClock(info$, mono);
    const { component, fixture } = ctx;
    const updateSpy = ctx.system.updateSystem as jasmine.Spy;
    startFast(component, 1, 1);
    expect(component.snapshot.state).toBe('warmup'); // applied live, no restart
    // Drive the phases on the MONOTONIC clock with genuine telemetry emissions.
    let hr = 1290;
    for (let i = 0; i < 10 && component.snapshot.state !== 'complete'; i++) {
      mono.v += 800; info$.next(supportedInfo({ hashRate: hr++ } as any)); tick(5000);
    }
    expect(component.snapshot.state).toBe('complete');
    expect(component.snapshot.restoreResult).toBe('ok');
    expect(updateSpy.calls.count()).toBeGreaterThanOrEqual(2);
    expect(component.results.length).toBe(1);
    expect(component.history.length).toBe(1);
    expect(JSON.stringify(component.history[0])).not.toMatch(/ipv4|ssid|wallet|stratum/i);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('aborts on owner request and restores the original', fakeAsync(() => {
    const mono = { v: 700_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    startFast(component, 60, 60);
    mono.v += 800; info$.next(supportedInfo({ hashRate: 1291 } as any)); tick(5000);
    expect(component.isActiveState()).toBeTrue();
    component.requestAbort();
    expect(component.showAbort).toBeTrue();
    component.confirmAbort();
    expect(component.snapshot.state).toBe('aborted');
    expect(component.results[0].status).toBe('aborted');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('collects evidence from genuine telemetry, NOT from the heartbeat timer', fakeAsync(() => {
    const mono = { v: 900_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    startFast(component, 1, 600); // long measurement so we stay in-window
    // Cross warm-up into measurement with one emission.
    mono.v += 1200; info$.next(supportedInfo({ hashRate: 1291 } as any)); tick(5000);
    expect(component.snapshot.state).toBe('measuring');
    const afterWarmup = (component as any).measureSamples.length;
    // Heartbeats WITHOUT new telemetry must NOT add measurement evidence.
    mono.v += 100; tick(5000);
    mono.v += 100; tick(5000);
    expect((component as any).measureSamples.length).toBe(afterWarmup);
    // Genuine new telemetry (spaced past the cadence) DOES add evidence.
    mono.v += 6000; info$.next(supportedInfo({ hashRate: 1292 } as any)); tick(5000);
    mono.v += 6000; info$.next(supportedInfo({ hashRate: 1293 } as any)); tick(5000);
    expect((component as any).measureSamples.length).toBeGreaterThan(afterWarmup);
    // Abort to restore cleanly.
    component.confirmAbort();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a full measurement window with sparse coverage is Partial with an explicit reason', fakeAsync(() => {
    const mono = { v: 1_500_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    startFast(component, 1, 600); // 600 s window → target 120 samples
    mono.v += 1200; info$.next(supportedInfo({ hashRate: 1291 } as any)); tick(5000); // → measuring
    expect(component.snapshot.state).toBe('measuring');
    // Emit genuine samples only every ~40 s across the whole 600 s window — like a
    // throttled background tab. That stays inside the 45 s reconnect grace (never
    // aborts) but yields ~15 samples against a target of 120 → low coverage.
    let hr = 1290;
    for (let i = 0; i < 20 && component.isActiveState(); i++) {
      mono.v += 40_000; info$.next(supportedInfo({ hashRate: hr++ } as any)); tick(5000);
    }
    const result = component.results[0];
    expect(result).toBeTruthy();
    expect(result.status).toBe('partial');
    expect(result.statusReason.toLowerCase()).toContain('coverage');
    expect(result.validSamples).toBeLessThan(result.expectedSamples);
    expect(result.expectedSamples).toBe(120);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('counts byte-identical genuine arrivals as separate evidence (arrival-identity, not content)', fakeAsync(() => {
    const mono = { v: 3_000_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    startFast(component, 1, 600);
    mono.v += 1200; info$.next(supportedInfo({ hashRate: 1291 } as any)); tick(5000); // → measuring
    expect(component.snapshot.state).toBe('measuring');
    const start = (component as any).measureSamples.length;
    // Emit the SAME payload three times, 5 s apart. Content is identical; each is a
    // genuinely new arrival → three new evidence samples.
    const identical = supportedInfo({ hashRate: 1290, temp: 60, power: 21 } as any);
    mono.v += 6000; info$.next({ ...identical }); tick(5000);
    mono.v += 6000; info$.next({ ...identical }); tick(5000);
    mono.v += 6000; info$.next({ ...identical }); tick(5000);
    expect((component as any).measureSamples.length).toBe(start + 3);
    component.confirmAbort();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('aborts at the MONOTONIC max-duration deadline; a wall-clock jump neither trips nor extends it', fakeAsync(() => {
    const mono = { v: 5_000_000 };
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    startFast(component, 30, 60); // cap = (90)s×2 + 600s = 780_000 ms (monotonic)
    // Freeze the wall clock at a wildly jumped value — the cap must ignore it.
    const jumped = spyOn(Date, 'now').and.returnValue(4_102_444_800_000); // year ~2100
    mono.v += 700_000; info$.next(supportedInfo({ hashRate: 1291 } as any)); tick(5000);
    expect(component.isActiveState()).toBeTrue(); // monotonic 700k < 780k → still running
    // Jump the wall clock far backwards — still must not early-abort.
    jumped.and.returnValue(1_000_000_000_000); // year ~2001
    mono.v += 50_000; info$.next(supportedInfo({ hashRate: 1292 } as any)); tick(5000);
    expect(component.isActiveState()).toBeTrue(); // monotonic 750k < 780k
    // Cross the monotonic deadline → abort + restore, regardless of Date.now.
    mono.v += 50_000; info$.next(supportedInfo({ hashRate: 1293 } as any)); tick(5000);
    expect(component.snapshot.state).toBe('aborted');
    expect(component.snapshot.reason).toContain('Maximum session time');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('stores wall-clock (Date.now) start/finish timestamps in history, not the monotonic clock', fakeAsync(() => {
    const mono = { v: 9_000_000 };            // monotonic clock is deliberately small
    const info$ = new BehaviorSubject(supportedInfo());
    const { component, fixture } = buildWithClock(info$, mono);
    const wallBefore = Date.now();            // the wall clock the record must use
    startFast(component, 1, 1);
    let hr = 1290;
    for (let i = 0; i < 10 && component.snapshot.state !== 'complete'; i++) {
      mono.v += 800; info$.next(supportedInfo({ hashRate: hr++ } as any)); tick(5000);
    }
    expect(component.history.length).toBe(1);
    const rec = component.history[0];
    // Timestamps track Date.now, NOT the ~9,000,000 ms monotonic session clock.
    expect(rec.startedAt).toBeGreaterThanOrEqual(wallBefore);
    expect(rec.startedAt).not.toBe(9_000_000);
    expect(rec.finishedAt).toBeGreaterThanOrEqual(rec.startedAt);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('aborts a running session when telemetry goes stale beyond the reconnect grace', fakeAsync(() => {
    const mono = { v: 1_000_000 };
    const info$ = new BehaviorSubject(supportedInfo({ sharesRejected: 0, sharesAccepted: 1000 } as any));
    const { component, fixture } = buildWithClock(info$, mono);
    // keep the session engine's wall clock and mono clock advancing together
    const fast: StabilityProfile = {
      id: 'f', name: 'Fast', frequency: 500, coreVoltage: 1200, thermalControlMode: 'target',
      temptarget: 60, minFanSpeed: 25, warmupSec: 600, measureSec: 600, cooldownSec: 0,
    };
    component.profiles = [fast];
    component.recomputePreflight();
    component.reviewSession();
    component.acks = { settings: true, restart: true, interrupt: true, protection: true, restore: true };
    component.startSession();
    expect(component.snapshot.state).toBe('warmup');

    // Telemetry stops: advance the monotonic clock past the freshness limit + grace
    // WITHOUT emitting a new sample. The heartbeat detects the staleness and aborts.
    mono.v += FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS + 6000;
    tick(5000);
    expect(component.snapshot.state).toBe('aborted');
    expect(component.snapshot.reason).toContain('offline');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('does not abort when telemetry recovers within the reconnect grace', fakeAsync(() => {
    const mono = { v: 2_000_000 };
    const info$ = new BehaviorSubject(supportedInfo({ sharesRejected: 0, sharesAccepted: 1000 } as any));
    const { component, fixture } = buildWithClock(info$, mono);
    const fast: StabilityProfile = {
      id: 'f2', name: 'Fast', frequency: 500, coreVoltage: 1200, thermalControlMode: 'target',
      temptarget: 60, minFanSpeed: 25, warmupSec: 600, measureSec: 600, cooldownSec: 0,
    };
    component.profiles = [fast];
    component.recomputePreflight();
    component.reviewSession();
    component.acks = { settings: true, restart: true, interrupt: true, protection: true, restore: true };
    component.startSession();

    // Stale but still inside the grace, then a fresh sample arrives.
    mono.v += FRESHNESS_LIMIT_MS + 2000;
    tick(5000);
    expect(component.snapshot.state).toBe('warmup'); // not aborted yet
    info$.next(supportedInfo({ hashRate: 1287, sharesRejected: 0, sharesAccepted: 1000 } as any)); // re-stamps freshness
    mono.v += 1000;
    tick(5000);
    expect(['warmup', 'measuring']).toContain(component.snapshot.state); // recovered, still running
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

describe('StabilityLabComponent (conservative defaults + honest starters)', () => {
  /** Build, set a stored thresholds value, then run ngOnInit (which migrates). */
  function buildWithStoredThresholds(stored: object) {
    const ctx = build(of(supportedInfo()));
    window.localStorage.setItem('NX_STABILITY_THRESHOLDS', JSON.stringify(stored));
    ctx.fixture.detectChanges();
    fixtures.push(ctx.fixture);
    return ctx;
  }

  it('applies the conservative board-601 VRM 70 default when nothing is stored', () => {
    const { component } = create();
    expect(component.thresholds.vrmC).toBe(70);
    expect(component.thresholds.asicC).toBe(68);
  });

  it('migrates an untouched legacy threshold config (VRM 100 → 70) on load', () => {
    const { component } = buildWithStoredThresholds({
      asicC: 68, vrmC: 100, errorPct: 5, rejectPct: 8, fanSaturationStop: false, fanSaturationPct: 100, debounceSamples: 3,
    });
    expect(component.thresholds.vrmC).toBe(70);
  });

  it('preserves an owner-customised threshold config on load (deliberate VRM 100 kept)', () => {
    const { component } = buildWithStoredThresholds({
      asicC: 62, vrmC: 100, errorPct: 5, rejectPct: 8, fanSaturationStop: false, fanSaturationPct: 100, debounceSamples: 3,
    });
    expect(component.thresholds.asicC).toBe(62);
    expect(component.thresholds.vrmC).toBe(100);
  });

  it('omits a misleading same-frequency/higher-voltage Performance starter, with a note', () => {
    // Baseline already at the highest served frequency (575); Performance would be
    // 575 MHz at a higher voltage — no honest hashrate gain, so it is omitted.
    const { component } = create(supportedInfo({ frequency: 575, coreVoltage: 1200 } as any));
    expect(component.starters.some(s => s.key === 'performance')).toBeFalse();
    expect(component.starterNotes.length).toBeGreaterThan(0);
    expect(component.starterNotes.join(' ')).toContain('Performance');
  });
});
