import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FormBuilder, ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { of } from 'rxjs';

import { EditComponent } from './edit.component';
import { SystemApiService } from 'src/app/services/system.service';
import { buildTuningPresets, firmwareRangeValidator } from './tuning';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { DropdownModule } from 'primeng/dropdown';
import { CheckboxModule } from 'primeng/checkbox';
import { SliderModule } from 'primeng/slider';
import { InputTextModule } from 'primeng/inputtext';
import { MessageModule } from 'primeng/message';
import { TooltipModule } from 'primeng/tooltip';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { provideRouter } from '@angular/router';
import { SystemInfo } from 'src/app/generated/models';

describe('EditComponent', () => {
  let component: EditComponent;
  let fixture: ComponentFixture<EditComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [EditComponent, DateAgoPipe],
      imports: [
        ReactiveFormsModule, NoopAnimationsModule,
        DropdownModule, CheckboxModule, SliderModule,
        InputTextModule, MessageModule, TooltipModule,
      ],
      providers: [provideHttpClient(), provideToastr(), provideRouter([])]
    });
    fixture = TestBed.createComponent(EditComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  describe('voltage semantics (configured value, board-601 evidence)', () => {
    const fb = new FormBuilder();

    function setup(coreVoltage: number, options: number[], defaultVoltage: number) {
      component.voltageOptions = options;
      component.defaultVoltage = defaultVoltage;
      // Full control set: the template reads every one of these.
      component.form = fb.group({
        coreVoltage: [coreVoltage],
        frequency: [625],
        autofanspeed: [true],
        minfanspeed: [25],
        manualFanSpeed: [50],
        temptarget: [55],
        overheat_mode: [0],
        display: ['NONE'],
        rotation: [0],
        displayTimeout: [-1],
        invertscreen: [false],
        statsFrequency: [0],
      });
    }

    it('labels 1150 as (Default) for the BM1370 board default, without altering the value', () => {
      setup(1150, [1000, 1100, 1150, 1200], 1150);
      const options = component.dropdownVoltage;
      expect(options.find(o => o.value === 1150)?.name).toBe('1150 (Default)');
      expect(options.find(o => o.value === 1100)?.name).toBe('1100');
      expect(component.form.get('coreVoltage')?.value).toBe(1150);
    });

    it('presents a configured 1100 mV as a plain option when the API offers it', () => {
      setup(1100, [1000, 1100, 1150, 1200], 1150);
      const options = component.dropdownVoltage;
      expect(options.find(o => o.value === 1100)?.name).toBe('1100');
      expect(options.some(o => o.name.includes('Custom'))).toBeFalse();
      expect(component.form.get('coreVoltage')?.value).toBe(1100);
    });

    it('keeps an off-list configured value visible as (Custom) instead of changing it', () => {
      setup(1137, [1000, 1100, 1150, 1200], 1150);
      const options = component.dropdownVoltage;
      expect(options.find(o => o.value === 1137)?.name).toBe('1137 (Custom)');
      expect(component.form.get('coreVoltage')?.value).toBe(1137);
    });

    it('builds no options when the API provides none (no invented defaults)', () => {
      setup(1150, [], 1150);
      expect(component.dropdownVoltage).toEqual([]);
    });

    it('shows measured telemetry as V without ever writing into the configured mV form value', () => {
      setup(1150, [1000, 1100, 1150, 1200], 1150);
      component.measured$ = of({
        coreVoltageActual: 1140, actualFrequency: 625,
        temp: 55, vrTemp: 60, fanrpm: 3583,
      } as SystemInfo);

      fixture.detectChanges();

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).toContain('1.14 V');
      expect(text).toContain('625 MHz');
      // Configured value untouched by the display of measured telemetry.
      expect(component.form.get('coreVoltage')?.value).toBe(1150);
      expect(component.form.dirty).toBeFalse();
    });

    it('presets only fill the pending controls — nothing is saved or restarted', () => {
      setup(1150, [1100, 1150, 1200, 1250, 1300], 1150);
      component.frequencyOptions = [400, 425, 485, 575];
      component.defaultFrequency = 485;
      component.presets = buildTuningPresets([400, 425, 485, 575], [1100, 1150, 1200, 1250, 1300], 485, 1150);

      const systemService = TestBed.inject(SystemApiService);
      const saveSpy = spyOn(systemService, 'updateSystem');
      const restartSpy = spyOn(systemService, 'restart');

      const eco = component.presets.find(p => p.id === 'eco')!;
      component.applyPreset(eco);

      expect(component.form.get('frequency')?.value).toBe(400);
      expect(component.form.get('coreVoltage')?.value).toBe(1100);
      expect(component.form.dirty).toBeTrue();
      expect(saveSpy).not.toHaveBeenCalled();
      expect(restartSpy).not.toHaveBeenCalled();
      expect(component.activePreset).toBe('eco');
    });

    it('activePreset falls back to custom when pending values diverge from every preset', () => {
      setup(1150, [1100, 1150, 1200, 1250, 1300], 1150);
      component.presets = buildTuningPresets([400, 425, 485, 575], [1100, 1150, 1200, 1250, 1300], 485, 1150);
      component.form.patchValue({ frequency: 625, coreVoltage: 1150 });
      expect(component.activePreset).toBe('custom');
    });

    it('pendingList reports Current → Pending with restart classification', () => {
      setup(1150, [1100, 1150, 1200], 1150);
      component.baseline = component.form.getRawValue();
      component.form.patchValue({ frequency: 485, display: 'SSD1306 (128x32)' });

      const changes = component.pendingList;
      const freq = changes.find(c => c.field === 'frequency')!;
      const display = changes.find(c => c.field === 'display')!;
      expect(freq.current).toBe('625 MHz');
      expect(freq.pending).toBe('485 MHz');
      expect(freq.restartRequired).toBeFalse();     // applied live by the firmware
      expect(display.restartRequired).toBeTrue();   // display change needs a restart
    });

    it('revertChanges restores the stored baseline and leaves the form pristine', () => {
      setup(1150, [1100, 1150, 1200], 1150);
      component.baseline = component.form.getRawValue();
      component.form.patchValue({ frequency: 400, coreVoltage: 1300, temptarget: 60 });
      component.form.markAsDirty();

      component.revertChanges();

      expect(component.form.get('frequency')?.value).toBe(625);
      expect(component.form.get('coreVoltage')?.value).toBe(1150);
      expect(component.form.get('temptarget')?.value).toBe(55);
      expect(component.form.dirty).toBeFalse();
      expect(component.pendingList).toEqual([]);
    });

    it('Apply & Restart saves first and restarts only after a successful save', () => {
      setup(1150, [1100, 1150, 1200], 1150);
      component.baseline = component.form.getRawValue();
      component.form.patchValue({ frequency: 485 });
      component.form.markAsDirty();

      const systemService = TestBed.inject(SystemApiService);
      const calls: string[] = [];
      spyOn(systemService, 'updateSystem').and.callFake(() => { calls.push('save'); return of(undefined); });
      spyOn(systemService, 'restart').and.callFake(() => { calls.push('restart'); return of({ message: 'ok' }); });

      component.applyAndRestart();

      expect(calls).toEqual(['save', 'restart']);
      // After a successful save the pending values become the new baseline.
      expect(component.baseline?.['frequency']).toBe(485);
      expect(component.form.dirty).toBeFalse();
    });

    it('a plain Save never restarts and re-baselines the form', () => {
      setup(1150, [1100, 1150, 1200], 1150);
      component.baseline = component.form.getRawValue();
      component.form.patchValue({ temptarget: 60 });
      component.form.markAsDirty();

      const systemService = TestBed.inject(SystemApiService);
      spyOn(systemService, 'updateSystem').and.returnValue(of(undefined));
      const restartSpy = spyOn(systemService, 'restart');

      component.updateSystem();

      expect(restartSpy).not.toHaveBeenCalled();
      expect(component.baseline?.['temptarget']).toBe(60);
      expect(component.pendingList).toEqual([]);
    });

    it('firmware-range validation invalidates the form for out-of-range or NaN values', () => {
      setup(1150, [1100, 1150, 1200], 1150);
      const temp = component.form.get('temptarget')!;
      temp.setValidators([firmwareRangeValidator(35, 66, true)]);

      temp.setValue(80);
      temp.updateValueAndValidity();
      expect(temp.invalid).toBeTrue();
      expect(component.form.invalid).toBeTrue();   // Save & Apply buttons disable on form.invalid

      temp.setValue(NaN);
      temp.updateValueAndValidity();
      expect(temp.invalid).toBeTrue();

      temp.setValue(55);
      temp.updateValueAndValidity();
      expect(temp.valid).toBeTrue();
    });

    it('guards invalid measured telemetry (no unit confusion, no NaN text)', () => {
      setup(1150, [1000, 1100, 1150, 1200], 1150);
      component.measured$ = of({
        coreVoltageActual: NaN, actualFrequency: undefined,
      } as unknown as SystemInfo);

      fixture.detectChanges();

      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(text).not.toContain('NaN');
      expect(text).not.toContain('undefined');
    });
  });
});
