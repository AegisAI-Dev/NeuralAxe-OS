import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit, OnDestroy, OnChanges, SimpleChanges } from '@angular/core';
import { AbstractControl, FormBuilder, FormGroup, FormControl, ValidationErrors, ValidatorFn, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { forkJoin, startWith, Subject, takeUntil, pairwise, BehaviorSubject, Observable, first } from 'rxjs';
import { LoadingService } from 'src/app/services/loading.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { DeckFmt } from 'src/app/components/command-deck/deck-format';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { ActivatedRoute } from '@angular/router';
import {
  TUNING_BOUNDS,
  TuningPreset,
  PendingChange,
  activeTuningPreset,
  buildTuningPresets,
  firmwareRangeValidator,
  pendingChanges,
  FAN_CURVE_BOUNDS,
  FanCurvePoint,
  ThermalControlMode,
  ThermalProfile,
  THERMAL_PROFILES,
  CURVE_TEMP_CONTROLS,
  CURVE_FAN_CONTROLS,
  CurvePreviewModel,
  activeThermalProfile,
  buildSettingsPayload,
  curveFromFormValue,
  curvePreviewModel,
  curveSegmentLabel,
  thermalModeLabel,
  validateFanCurve,
} from './tuning';
import { SemanticSeverity, asicTempSeverity, fanSaturationSeverity, thermalDeltaSeverity, vrTempSeverity } from 'src/app/services/semantic-status';

type Dropdown = {
  name: string;
  value: number;
}[]

const DISPLAY_TIMEOUT_STEPS = [0, 1, 2, 5, 15, 30, 60, 60 * 2, 60 * 4, 60* 8, -1];
const STATS_FREQUENCY_STEPS = [0, 1, 2, 5, 10, 30, 60, 60 * 2, 60 * 6, 60 * 14, 60 * 28, 60 * 60];

@Component({
  selector: 'app-edit',
  templateUrl: './edit.component.html'
})

export class EditComponent implements OnInit, OnDestroy, OnChanges {
  private formSubject = new BehaviorSubject<FormGroup | null>(null);
  public form$: Observable<FormGroup | null> = this.formSubject.asObservable();

  public form!: FormGroup;

  public savedChanges: boolean = false;
  public settingsUnlocked: boolean = false;

  @Input() uri = '';

  // Store frequency and voltage options from API
  public defaultFrequency: number = 0;
  public frequencyOptions: number[] = [];
  public defaultVoltage: number = 0;
  public voltageOptions: number[] = [];

  private destroy$ = new Subject<void>();

  public displays = ["NONE", "SSD1306 (128x32)", "SSD1309 (128x64)", "SH1107 (64x128)", "SH1107 (128x128)"];
  public rotations = [0, 90, 180, 270];

  /**
   * Configuration currently stored on the device — the "Current" side of the
   * Current/Pending review. Captured when the form loads and refreshed after
   * every successful save; never mutated by edits.
   */
  public baseline: { [key: string]: any } | null = null;

  /** Presets built ONLY from the device-served option lists; [] when unavailable. */
  public presets: TuningPreset[] = [];
  public displayTimeoutControl: FormControl;
  public statsFrequencyControl: FormControl;
  public statsLimit: number = 720;

  /**
   * Live telemetry for the read-only "measured right now" line. Display only —
   * nothing here ever writes into the form: the configured values (mV / MHz)
   * and the measured values (V / MHz actual) are deliberately separate.
   * Only shown for the local device (remote fleet editing has no live stream).
   */
  public measured$: Observable<ISystemInfo>;
  public readonly fmt = DeckFmt;

  constructor(
    private fb: FormBuilder,
    private systemService: SystemApiService,
    private liveDataService: LiveDataService,
    private toastr: ToastrService,
    private loadingService: LoadingService,
    private route: ActivatedRoute,
  ) {
    // Check URL parameter for settings unlock
    this.route.queryParams.subscribe(params => {
      const urlOcParam = params['oc'] !== undefined;
      if (urlOcParam) {
        // If ?oc is in URL, enable overclock and save to NVS
        this.settingsUnlocked = true;
        this.saveOverclockSetting(1);
        console.log(
          '🎉 The ancient seals have been broken!\n' +
          '⚡ Unlimited power flows through your miner...\n' +
          '🔧 You can now set custom frequency and voltage values.\n' +
          '⚠️ Remember: with great power comes great responsibility!'
        );
      } else {
        // If ?oc is not in URL, check NVS setting (will be loaded in ngOnInit)
        console.log('🔒 Here be dragons! Advanced settings are locked for your protection. \n' +
          'Only the bravest miners dare to venture forth... \n' +
          'If you wish to unlock dangerous overclocking powers, add: %c?oc',
          'color: #ff4400; text-decoration: underline; cursor: pointer; font-weight: bold;',
          'to the current URL'
        );
      }
    });

    this.measured$ = this.liveDataService.info$;

    this.displayTimeoutControl = new FormControl();
    this.displayTimeoutControl.valueChanges.pipe(pairwise()).subscribe(([prev, next]) => {
      if (prev === next) {
        return;
      }

      this.form.patchValue({ displayTimeout: DISPLAY_TIMEOUT_STEPS[next] });
      this.form.controls['displayTimeout'].markAsDirty();
    });

    this.statsFrequencyControl = new FormControl();
    this.statsFrequencyControl.valueChanges.pipe(pairwise()).subscribe(([prev, next]) => {
      if (prev === next) {
        return;
      }

      this.form.patchValue({ statsFrequency: STATS_FREQUENCY_STEPS[next] });
      this.form.controls['statsFrequency'].markAsDirty();
    });
  }

  private saveOverclockSetting(enabled: number) {
    const deviceUri = this.uri || '';
    this.systemService.updateSystem(deviceUri, { overclockEnabled: enabled })
      .subscribe({
        next: () => {
          console.log(`Overclock setting saved: ${enabled === 1 ? 'enabled' : 'disabled'}`);
        },
        error: (err) => {
          console.error(`Failed to save overclock setting: ${err.message}`);
        }
      });
  }

  ngOnInit(): void {
    this.loadDeviceSettings();
  }

  ngOnChanges(changes: SimpleChanges): void {
    // When URI changes, reload the device settings
    if (changes['uri'] && changes['uri'].currentValue && !changes['uri'].firstChange) {
      this.loadDeviceSettings();
    }
  }

  private loadDeviceSettings(): void {
    const deviceUri = this.uri || '';

    const info$ = deviceUri
      ? this.systemService.getInfo(deviceUri)
      : this.liveDataService.info$.pipe(first());

    // Fetch both system info and ASIC settings in parallel
    forkJoin({
      info: info$,
      asic: this.systemService.getAsicSettings(deviceUri)
    })
    .pipe(
      this.loadingService.lockUIUntilComplete(),
      takeUntil(this.destroy$)
    )
    .subscribe(({ info, asic }) => {
      // Store the frequency and voltage options from the API
      this.defaultFrequency = asic.defaultFrequency;
      this.frequencyOptions = asic.frequencyOptions;
      this.defaultVoltage = asic.defaultVoltage;
      this.voltageOptions = asic.voltageOptions;
      this.statsLimit = info.statsLimit || 720;

      // Check if overclock is enabled in NVS
      if (info.overclockEnabled) {
        this.settingsUnlocked = true;
        console.log(
          '🎉 Overclock mode is enabled from NVS settings!\n' +
          '⚡ Custom frequency and voltage values are available.'
        );
      }

        this.form = this.fb.group({
          display: [info.display, [Validators.required]],
          rotation: [info.rotation, [Validators.required]],
          invertscreen: [info.invertscreen == 1],
          displayTimeout: [info.displayTimeout, [
            Validators.required,
            Validators.min(-1),
            Validators.max(this.displayTimeoutMaxValue)
          ]],
          // Numeric bounds mirror the firmware's NVS validation (nvs_config.c):
          // the form blocks exactly what the device would reject, including
          // NaN/null/non-finite submissions.
          coreVoltage: [info.coreVoltage, [
            firmwareRangeValidator(TUNING_BOUNDS.coreVoltage.min, TUNING_BOUNDS.coreVoltage.max, true)
          ]],
          frequency: [info.frequency, [
            firmwareRangeValidator(TUNING_BOUNDS.frequency.min, TUNING_BOUNDS.frequency.max)
          ]],
          // Three explicit thermal modes (Phase 2H). The device already
          // resolves legacy installs (autofanspeed) to target/manual, but we
          // keep the same derivation as a defensive fallback.
          thermalControlMode: [
            (info.thermalControlMode as ThermalControlMode) ?? (info.autofanspeed == 1 ? 'target' : 'manual'),
            [Validators.required]
          ],
          minfanspeed: [info.minFanSpeed, [
            firmwareRangeValidator(TUNING_BOUNDS.minFanSpeed.min, TUNING_BOUNDS.minFanSpeed.max, true)
          ]],
          manualFanSpeed: [info.manualFanSpeed, [
            firmwareRangeValidator(TUNING_BOUNDS.manualFanSpeed.min, TUNING_BOUNDS.manualFanSpeed.max, true)
          ]],
          temptarget: [info.temptarget, [
            firmwareRangeValidator(TUNING_BOUNDS.temptarget.min, TUNING_BOUNDS.temptarget.max, true)
          ]],
          // Flat fan-curve point controls (structured editor only — the
          // firmware receives a validated fanCurve array, never free text).
          ...this.buildCurveControls(info.fanCurve as FanCurvePoint[] | undefined),
          fanCurveHysteresis: [info.fanCurveHysteresis ?? 2, [
            firmwareRangeValidator(FAN_CURVE_BOUNDS.hysteresis.min, FAN_CURVE_BOUNDS.hysteresis.max, true)
          ]],
          overheat_mode: [info.overheat_mode, [Validators.required]],
          statsFrequency: [info.statsFrequency, [
            Validators.required,
            Validators.min(0),
            Validators.max(this.statsFrequencyMaxValue)
          ]]
        }, { validators: [this.curveGroupValidator] });

        this.baseline = this.form.getRawValue();
        this.presets = buildTuningPresets(
          asic.frequencyOptions, asic.voltageOptions,
          asic.defaultFrequency, asic.defaultVoltage,
        );

        this.formSubject.next(this.form);

      this.form.controls['thermalControlMode'].valueChanges.pipe(
        startWith(this.form.controls['thermalControlMode'].value),
        takeUntil(this.destroy$)
      ).subscribe((mode: ThermalControlMode) => {
        const set = (name: string, enabled: boolean) => {
          const control = this.form.controls[name];
          if (enabled) {
            control.enable({ emitEvent: false });
          } else {
            control.disable({ emitEvent: false });
          }
        };
        // target: PID setpoint + min fan; curve: curve editor + min fan
        // (the firmware keeps min fan authoritative as a floor); manual:
        // fan slider only. Emergency protection is independent of all three.
        set('temptarget', mode === 'target');
        set('manualFanSpeed', mode === 'manual');
        set('minfanspeed', mode === 'target' || mode === 'curve');
        for (const name of [...CURVE_TEMP_CONTROLS, ...CURVE_FAN_CONTROLS, 'fanCurveHysteresis']) {
          set(name, mode === 'curve');
        }
        this.form.updateValueAndValidity({ emitEvent: false });
      });

      // Add custom value to predefined steps
      if (DISPLAY_TIMEOUT_STEPS.filter(x => x === info.displayTimeout).length === 0) {
        DISPLAY_TIMEOUT_STEPS.push(info.displayTimeout);
        DISPLAY_TIMEOUT_STEPS.sort((a, b) => a - b);
        DISPLAY_TIMEOUT_STEPS.push(DISPLAY_TIMEOUT_STEPS.shift() as number);
      }

      this.displayTimeoutControl.setValue(
        DISPLAY_TIMEOUT_STEPS.findIndex(x => x === info.displayTimeout)
      );

      // Add custom value to predefined steps
      if (STATS_FREQUENCY_STEPS.filter(x => x === info.statsFrequency).length === 0) {
        STATS_FREQUENCY_STEPS.push(info.statsFrequency);
        STATS_FREQUENCY_STEPS.sort((a, b) => a - b);
      }

      this.statsFrequencyControl.setValue(
        STATS_FREQUENCY_STEPS.findIndex(x => x === info.statsFrequency)
      );
    });
  }

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  public updateSystem(restartAfter: boolean = false) {
    const form = this.form.getRawValue();

    if (form.stratumPassword === '*****') {
      delete form.stratumPassword;
    }

    // The flat curve editor controls become a validated fanCurve array; the
    // legacy autofanspeed flag is kept in sync with the selected mode.
    const payload = buildSettingsPayload(form);

    const restartWasNeeded = this.isRestartRequired;
    const deviceUri = this.uri || '';
    this.systemService.updateSystem(deviceUri, payload)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Saved settings for ${this.uri}` : 'Saved settings';
          if (restartWasNeeded && !restartAfter) {
            this.toastr.warning('You must restart this device after saving for changes to take effect.');
          }
          this.toastr.success(successMessage);
          this.savedChanges = true;
          // The device now stores the pending values: they are the new baseline.
          this.baseline = this.form.getRawValue();
          this.form.markAsPristine();
          if (restartAfter) {
            this.restart();
          }
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Could not save settings for ${this.uri}. ${err.message}` : `Could not save settings. ${err.message}`;
          this.toastr.error(errorMessage);
          this.savedChanges = false;
        }
      });
  }

  /** Explicit Save-then-Restart — never triggered silently. */
  public applyAndRestart() {
    this.updateSystem(true);
  }

  /** Discard unsaved edits and return every control to the stored baseline. */
  public revertChanges() {
    if (!this.baseline) {
      return;
    }
    // Aux slider controls first: their valueChanges handlers patch the form
    // and mark it dirty, so the final patch + markAsPristine must come last.
    this.displayTimeoutControl.setValue(
      DISPLAY_TIMEOUT_STEPS.findIndex(x => x === this.baseline!['displayTimeout'])
    );
    this.statsFrequencyControl.setValue(
      STATS_FREQUENCY_STEPS.findIndex(x => x === this.baseline!['statsFrequency'])
    );
    this.form.patchValue(this.baseline);
    this.form.markAsPristine();
  }

  // ---- thermal control (Phase 2H) ----

  /** Flat controls for the 4 curve points, seeded from the device curve. */
  private buildCurveControls(curve: FanCurvePoint[] | undefined): { [key: string]: any } {
    const fallback = THERMAL_PROFILES.find(p => p.id === 'balanced')!.points;
    const points = Array.isArray(curve) && curve.length === FAN_CURVE_BOUNDS.points ? curve : fallback;
    const controls: { [key: string]: any } = {};
    points.forEach((point, i) => {
      controls[CURVE_TEMP_CONTROLS[i]] = [point.tempC, [
        firmwareRangeValidator(FAN_CURVE_BOUNDS.tempC.min, FAN_CURVE_BOUNDS.tempC.max, true)
      ]];
      controls[CURVE_FAN_CONTROLS[i]] = [point.fanPercent, [
        firmwareRangeValidator(FAN_CURVE_BOUNDS.fanPercent.min, FAN_CURVE_BOUNDS.fanPercent.max, true)
      ]];
    });
    return controls;
  }

  /**
   * Cross-field curve validation (ordering), active only while curve mode is
   * pending. Blocks Save on exactly what the firmware would reject.
   */
  private curveGroupValidator: ValidatorFn = (group: AbstractControl): ValidationErrors | null => {
    const raw = (group as FormGroup).getRawValue ? (group as FormGroup).getRawValue() : group.value;
    if (raw['thermalControlMode'] !== 'curve') {
      return null;
    }
    const errors = validateFanCurve(curveFromFormValue(raw));
    return errors.length ? { fanCurve: errors } : null;
  };

  get thermalMode(): ThermalControlMode {
    return this.form?.get('thermalControlMode')?.value ?? 'target';
  }

  public setThermalMode(mode: ThermalControlMode) {
    if (this.thermalMode === mode) {
      return;
    }
    this.form.patchValue({ thermalControlMode: mode });
    this.form.get('thermalControlMode')?.markAsDirty();
  }

  public thermalModeLabelFor(mode: unknown): string {
    return thermalModeLabel(mode);
  }

  public curveSegmentText(segment: unknown): string {
    return curveSegmentLabel(segment);
  }

  /** The pending curve as points (for validation display and the preview). */
  get pendingCurve(): FanCurvePoint[] {
    return curveFromFormValue(this.form.getRawValue());
  }

  /** Human-readable validation errors for the pending curve; [] = valid. */
  get curveErrors(): string[] {
    const groupErrors = this.form?.errors?.['fanCurve'];
    return Array.isArray(groupErrors) ? groupErrors : [];
  }

  get curvePreview(): CurvePreviewModel {
    return curvePreviewModel(this.pendingCurve);
  }

  public readonly thermalProfiles: ThermalProfile[] = THERMAL_PROFILES;
  public readonly curveBounds = FAN_CURVE_BOUNDS;
  public readonly curveTempControls = CURVE_TEMP_CONTROLS;
  public readonly curveFanControls = CURVE_FAN_CONTROLS;

  /** Template matching the pending curve, or 'custom' the moment it diverges. */
  get activeThermalProfile(): string {
    return activeThermalProfile(this.pendingCurve);
  }

  /**
   * Fill the pending curve editor with a template's exact documented values.
   * Nothing is saved and nothing restarts — Save stays an explicit action.
   */
  public applyThermalProfile(profile: ThermalProfile) {
    const patch: { [key: string]: number } = {};
    profile.points.forEach((point, i) => {
      patch[CURVE_TEMP_CONTROLS[i]] = point.tempC;
      patch[CURVE_FAN_CONTROLS[i]] = point.fanPercent;
    });
    this.form.patchValue(patch);
    for (const name of [...CURVE_TEMP_CONTROLS, ...CURVE_FAN_CONTROLS]) {
      this.form.get(name)?.markAsDirty();
    }
  }

  // ---- presets (existing valid board values only; display + pending fill) ----

  /** Preset matching the pending values, or 'custom' when they diverge. */
  get activePreset(): string {
    return activeTuningPreset(
      this.form?.get('frequency')?.value,
      this.form?.get('coreVoltage')?.value,
      this.presets,
    );
  }

  /**
   * Fill the pending frequency/voltage controls with a preset's exact values.
   * Nothing is saved and nothing restarts — Save / Apply & Restart stay
   * explicit user actions.
   */
  public applyPreset(preset: TuningPreset) {
    this.form.patchValue({ frequency: preset.frequency, coreVoltage: preset.coreVoltage });
    this.form.get('frequency')?.markAsDirty();
    this.form.get('coreVoltage')?.markAsDirty();
  }

  // ---- Current/Pending review ----

  get pendingList(): PendingChange[] {
    if (!this.baseline || !this.form) {
      return [];
    }
    return pendingChanges(this.baseline, this.form.getRawValue(), this.noRestartFields);
  }

  // ---- current operating state (read-only live telemetry helpers) ----

  /** J/TH derived from live power and hashrate; null when hashrate is 0/invalid. */
  public liveEfficiency(info: ISystemInfo): number | null {
    const th = (typeof info.hashRate === 'number' && isFinite(info.hashRate) ? info.hashRate : 0) / 1000;
    if (th <= 0 || typeof info.power !== 'number' || !isFinite(info.power)) {
      return null;
    }
    return info.power / th;
  }

  /** Signed °C distance from the configured target; null without valid data. */
  public thermalDelta(info: ISystemInfo): number | null {
    if (typeof info.temp !== 'number' || !isFinite(info.temp) || info.temp <= 0
      || typeof info.temptarget !== 'number' || !isFinite(info.temptarget) || info.temptarget <= 0) {
      return null;
    }
    return info.temp - info.temptarget;
  }

  public thermalDeltaText(info: ISystemInfo): string {
    const delta = this.thermalDelta(info);
    if (delta === null) {
      return DeckFmt.INVALID;
    }
    const rounded = Math.round(delta);
    if (rounded === 0) {
      return 'At target';
    }
    return `${rounded > 0 ? '+' : ''}${rounded} °C vs target`;
  }

  public tempSeverityClass(info: ISystemInfo): string {
    return this.severityText(thermalDeltaSeverity(info.temp, info.temptarget));
  }

  public vrTempSeverityClass(info: ISystemInfo): string {
    return this.severityText(vrTempSeverity(info.vrTemp));
  }

  public fanSeverityClass(info: ISystemInfo): string {
    return this.severityText(fanSaturationSeverity((info.autofanspeed ?? 0) == 1, info.fanspeed));
  }

  public asicTempClass(temp: number | undefined): string {
    return this.severityText(asicTempSeverity(temp));
  }

  private severityText(severity: SemanticSeverity): string {
    switch (severity) {
      case 'ok': return 'nx-ok-text';
      case 'warn': return 'nx-warn-text';
      case 'danger': return 'nx-danger-text';
      case 'info': return 'nx-info-text';
      default: return 'nx-neutral-text';
    }
  }

  disableOverheatMode() {
    this.form.patchValue({ overheat_mode: 0 });
    this.updateSystem();
  }

  toggleOverclockMode(enable: boolean) {
    this.settingsUnlocked = enable;
    this.saveOverclockSetting(enable ? 1 : 0);

    if (enable) {
      console.log(
        '🎉 Overclock mode enabled!\n' +
        '⚡ Custom frequency and voltage values are now available.'
      );
    } else {
      console.log('🔒 Overclock mode disabled. Using safe preset values only.');
    }
  }

  public restart() {
    this.systemService.restart(this.uri)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Device at ${this.uri} restarted` : 'Device restarted';
          this.toastr.success(successMessage);
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Failed to restart device at ${this.uri}. ${err.message}` : `Failed to restart device. ${err.message}`;
          this.toastr.error(errorMessage);
        }
      });
  }

  get dropdownFrequency(): Dropdown {
    return this.buildDropdown('frequency', this.frequencyOptions, this.defaultFrequency);
  }

  get dropdownVoltage(): Dropdown {
    return this.buildDropdown('coreVoltage', this.voltageOptions, this.defaultVoltage);
  }

  get displayTimeoutMaxSteps(): number {
    return DISPLAY_TIMEOUT_STEPS.length - 1;
  }

  get displayTimeoutMaxValue(): number {
    return DISPLAY_TIMEOUT_STEPS[this.displayTimeoutMaxSteps - 1];
  }

  get statsFrequencyMaxSteps(): number {
    return STATS_FREQUENCY_STEPS.length - 1;
  }

  get statsFrequencyMaxValue(): number {
    return STATS_FREQUENCY_STEPS[this.statsFrequencyMaxSteps];
  }

  buildDropdown(formField: string, apiOptions: number[], defaultValue: number): Dropdown {
    if (!apiOptions.length) {
      return [];
    }

    // Convert options from API to dropdown format
    const options = apiOptions.map(option => {
      return {
        name: defaultValue === option ? `${option} (Default)` : `${option}`,
        value: option
      };
    });

    // Get current field value from form
    const currentValue = this.form?.get(formField)?.value;

    // If current field value exists and isn't in the options
    if (currentValue && !options.some(opt => opt.value === currentValue)) {
      options.push({
        name: `${currentValue} (Custom)`,
        value: currentValue
      });
      // Sort options by value
      options.sort((a, b) => a.value - b.value);
    }

    return options;
  }

  get noRestartFields(): string[] {
    // Everything the firmware applies live. The fan controller re-reads the
    // thermal configuration (mode, curve, hysteresis, min fan) within one
    // second of a save — no restart involved.
    return [
      'displayTimeout',
      'coreVoltage',
      'frequency',
      'autofanspeed',
      'thermalControlMode',
      'manualFanSpeed',
      'temptarget',
      'minfanspeed',
      ...CURVE_TEMP_CONTROLS,
      ...CURVE_FAN_CONTROLS,
      'fanCurveHysteresis',
      'overheat_mode',
      'statsFrequency'
    ];
  }

  get isRestartRequired(): boolean {
    return !! Object.entries(this.form.controls)
      .filter(([field, control]) => control.dirty && !this.noRestartFields.includes(field)).length
  }
}
