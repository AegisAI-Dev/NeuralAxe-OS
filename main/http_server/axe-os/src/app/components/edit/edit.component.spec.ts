import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FormBuilder, ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { of } from 'rxjs';

import { EditComponent } from './edit.component';
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
