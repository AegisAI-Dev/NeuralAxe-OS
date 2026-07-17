import { ComponentFixture, TestBed } from '@angular/core/testing';
import { ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { of, throwError } from 'rxjs';

import { InputTextModule } from 'primeng/inputtext';
import { CheckboxModule } from 'primeng/checkbox';
import { RadioButtonModule } from 'primeng/radiobutton';
import { TooltipModule } from 'primeng/tooltip';

import { PoolComponent } from './pool.component';
import { ModalComponent } from 'src/app/components/modal/modal.component';
import { AddressPipe } from 'src/app/pipes/address.pipe';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

function liveInfo(overrides: Partial<ISystemInfo> = {}): ISystemInfo {
  return {
    ASICModel: 'BM1370',
    stratumURL: 'public-pool.io',
    stratumPort: 21496,
    stratumUser: 'bc1qprimaryworker.axe',
    stratumProtocol: 'SV1',
    stratumSuggestedDifficulty: 1000,
    stratumExtranonceSubscribe: false,
    stratumTLS: 0,
    stratumCert: '',
    stratumDecodeCoinbase: true,
    stratumV2AuthorityPubkey: '',
    stratumV2ChannelType: 'extended',
    fallbackStratumURL: 'solo.ckpool.org',
    fallbackStratumPort: 3333,
    fallbackStratumUser: 'bc1qfallbackworker.axe',
    fallbackStratumProtocol: 'SV1',
    fallbackStratumSuggestedDifficulty: 500,
    fallbackStratumExtranonceSubscribe: true,
    fallbackStratumTLS: 0,
    fallbackStratumCert: '',
    fallbackStratumDecodeCoinbase: true,
    fallbackStratumV2AuthorityPubkey: '',
    fallbackStratumV2ChannelType: 'extended',
    isUsingFallbackStratum: 0,
    responseTime: 12,
    ...overrides,
  } as unknown as ISystemInfo;
}

describe('PoolComponent (pool switching)', () => {
  let component: PoolComponent;
  let fixture: ComponentFixture<PoolComponent>;
  let systemService: SystemApiService;
  let info: ISystemInfo;

  beforeEach(() => {
    info = liveInfo();
    TestBed.configureTestingModule({
      declarations: [PoolComponent, ModalComponent, AddressPipe],
      imports: [
        ReactiveFormsModule, NoopAnimationsModule,
        InputTextModule, CheckboxModule, RadioButtonModule, TooltipModule,
      ],
      providers: [
        provideHttpClient(), provideToastr(),
        { provide: LiveDataService, useValue: { info$: of(info), connected$: of(true) } },
      ]
    });
    fixture = TestBed.createComponent(PoolComponent);
    component = fixture.componentInstance;
    systemService = TestBed.inject(SystemApiService);
  });

  function modal(): ModalComponent {
    return new ModalComponent();
  }

  it('builds the pool form from live data on init without triggering any switch or save', () => {
    const patchSpy = spyOn(systemService, 'updateSystem').and.returnValue(of(undefined));
    const restartSpy = spyOn(systemService, 'restart').and.returnValue(of({ message: 'ok' }));

    fixture.detectChanges(); // ngOnInit

    expect(component.form).toBeTruthy();
    expect(component.form.get('stratumURL')?.value).toBe('public-pool.io');
    expect(component.switchPlan).toBeNull();
    expect(patchSpy).not.toHaveBeenCalled();   // no automatic switch on page load
    expect(restartSpy).not.toHaveBeenCalled();
  });

  describe('gating', () => {
    beforeEach(() => fixture.detectChanges());

    it('allows switching for two valid distinct pools', () => {
      expect(component.switchGate(info).allowed).toBeTrue();
    });

    it('blocks switching when the fallback pool is not usable', () => {
      const gate = component.switchGate(liveInfo({ fallbackStratumURL: '' }));
      expect(gate.allowed).toBeFalse();
      expect(gate.reason).toBeTruthy();
    });

    it('blocks switching when both pools are equivalent', () => {
      const gate = component.switchGate(liveInfo({
        fallbackStratumURL: 'public-pool.io',
        fallbackStratumPort: 21496,
        fallbackStratumUser: 'bc1qprimaryworker.axe',
      }));
      expect(gate.allowed).toBeFalse();
    });

    it('blocks switching while the form has unsaved edits', () => {
      component.form.get('stratumURL')?.markAsDirty();
      component.form.markAsDirty();
      expect(component.switchGate(info).allowed).toBeFalse();
    });

    it('openSwitchDialog refuses to open when gated', () => {
      const m = modal();
      component.openSwitchDialog(liveInfo({ fallbackStratumURL: '' }), m);
      expect(component.switchPlan).toBeNull();
      expect(m.isVisible).toBeFalse();
    });
  });

  describe('confirmation flow', () => {
    beforeEach(() => fixture.detectChanges());

    it('opening the dialog only snapshots a plan — nothing is sent to the device', () => {
      const patchSpy = spyOn(systemService, 'updateSystem');
      const m = modal();
      component.openSwitchDialog(info, m);
      expect(m.isVisible).toBeTrue();
      expect(component.switchPlan?.activeAfter.host).toBe('solo.ckpool.org');
      expect(patchSpy).not.toHaveBeenCalled();  // confirmation is required
    });

    it('cancel closes the dialog and changes nothing', () => {
      const patchSpy = spyOn(systemService, 'updateSystem');
      const m = modal();
      component.openSwitchDialog(info, m);
      component.cancelSwitch(m);
      expect(m.isVisible).toBeFalse();
      expect(component.switchPlan).toBeNull();
      expect(patchSpy).not.toHaveBeenCalled();
    });

    it('confirm writes ONLY useFallbackStratum and then restarts — every pool field is preserved', () => {
      const patchSpy = spyOn(systemService, 'updateSystem').and.returnValue(of(undefined));
      const restartSpy = spyOn(systemService, 'restart').and.returnValue(of({ message: 'ok' }));
      const m = modal();

      component.openSwitchDialog(info, m);
      component.confirmSwitch(m);

      expect(patchSpy).toHaveBeenCalledTimes(1);
      const [, body] = patchSpy.calls.mostRecent().args;
      expect(body).toEqual({ useFallbackStratum: true });  // exact body: no pool fields, no credentials
      expect(restartSpy).toHaveBeenCalledTimes(1);
      expect(m.isVisible).toBeFalse();
      expect(component.switchPlan).toBeNull();
    });

    it('switching back from an active fallback writes useFallbackStratum: false', () => {
      const patchSpy = spyOn(systemService, 'updateSystem').and.returnValue(of(undefined));
      spyOn(systemService, 'restart').and.returnValue(of({ message: 'ok' }));
      const m = modal();

      component.openSwitchDialog(liveInfo({ isUsingFallbackStratum: 1 }), m);
      component.confirmSwitch(m);

      expect(patchSpy.calls.mostRecent().args[1]).toEqual({ useFallbackStratum: false });
    });

    it('a failed save is non-destructive: no restart, clear message path', () => {
      const patchSpy = spyOn(systemService, 'updateSystem')
        .and.returnValue(throwError(() => new Error('boom')));
      const restartSpy = spyOn(systemService, 'restart');
      const m = modal();

      component.openSwitchDialog(info, m);
      component.confirmSwitch(m);

      expect(patchSpy).toHaveBeenCalledTimes(1);
      expect(restartSpy).not.toHaveBeenCalled();
      expect(component.switchBusy).toBeFalse();
    });

    it('confirm without an open plan does nothing', () => {
      const patchSpy = spyOn(systemService, 'updateSystem');
      component.confirmSwitch(modal());
      expect(patchSpy).not.toHaveBeenCalled();
    });
  });

  describe('existing save & restart behavior', () => {
    beforeEach(() => fixture.detectChanges());

    it('Save still sends the full pool form (all advanced fields) via the same PATCH path', () => {
      const patchSpy = spyOn(systemService, 'updateSystem').and.returnValue(of(undefined));

      component.form.get('fallbackStratumSuggestedDifficulty')?.setValue(750);
      component.form.markAsDirty();
      component.updateSystem();

      expect(patchSpy).toHaveBeenCalledTimes(1);
      const body = patchSpy.calls.mostRecent().args[1] as Record<string, unknown>;
      expect(body['stratumURL']).toBe('public-pool.io');
      expect(body['fallbackStratumSuggestedDifficulty']).toBe(750);
      expect(body['fallbackStratumExtranonceSubscribe']).toBeTrue();
      // Untouched masked passwords are never transmitted.
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
    });

    it('Restart still calls the existing restart endpoint', () => {
      const restartSpy = spyOn(systemService, 'restart').and.returnValue(of({ message: 'ok' }));
      component.restart();
      expect(restartSpy).toHaveBeenCalledTimes(1);
    });
  });
});
