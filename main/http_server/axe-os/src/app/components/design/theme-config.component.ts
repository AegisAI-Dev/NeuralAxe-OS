import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subject } from 'rxjs';
import { takeUntil } from 'rxjs/operators';
import { LayoutService } from '../../layout/service/app.layout.service';
import { ThemeService, applyAccentColors } from '../../services/theme.service';

export interface ThemeOption {
  name: string;
  primaryColor: string;
  accentColors: {
    [key: string]: string;
  };
}

function accentSet(base: string, hover: string, focusRing: string): { [key: string]: string } {
  return {
    '--primary-color': base,
    '--primary-color-text': '#ffffff',
    '--highlight-bg': base,
    '--highlight-text-color': '#ffffff',
    '--focus-ring': focusRing,
    // PrimeNG Slider
    '--slider-bg': '#dee2e6',
    '--slider-range-bg': base,
    '--slider-handle-bg': base,
    // Progress Bar
    '--progressbar-bg': '#dee2e6',
    '--progressbar-value-bg': base,
    // PrimeNG Checkbox
    '--checkbox-border': base,
    '--checkbox-bg': base,
    '--checkbox-hover-bg': hover,
    // PrimeNG Button
    '--button-bg': base,
    '--button-hover-bg': hover,
    '--button-focus-shadow': `0 0 0 2px #ffffff, 0 0 0 4px ${base}`,
    // Toggle button
    '--togglebutton-bg': base,
    '--togglebutton-border': `1px solid ${base}`,
    '--togglebutton-hover-bg': hover,
    '--togglebutton-hover-border': `1px solid ${hover}`,
    '--togglebutton-text-color': '#ffffff'
  };
}

/**
 * Every selectable accent theme. Accent themes style interactive chrome only
 * (navigation highlight, buttons, sliders, checkboxes, focus) — semantic
 * operational colors (--nx-sem-*) carry fixed healthy/caution/error meanings
 * and are NOT part of an accent set; the apply path enforces this via the
 * ACCENT_COLOR_KEYS allowlist in theme.service.ts.
 */
export const THEME_PRESETS: ThemeOption[] = [
  { name: 'Orange', primaryColor: '#F7931A', accentColors: accentSet('#F7931A', '#e58617', '0 0 0 0.2rem rgba(247,147,26,0.2)') },
  { name: 'Red', primaryColor: '#F80421', accentColors: accentSet('#F80421', '#e63c2e', '0 0 0 0.2rem rgba(255,64,50,0.2)') },
  { name: 'Blue', primaryColor: '#2196f3', accentColors: accentSet('#2196f3', '#1e88e5', '0 0 0 0.2rem rgba(33,150,243,0.2)') },
  { name: 'Green (Default)', primaryColor: '#4caf50', accentColors: accentSet('#4caf50', '#43a047', '0 0 0 0.2rem rgba(76,175,80,0.2)') },
  { name: 'Purple', primaryColor: '#b340fa', accentColors: accentSet('#b340fa', '#8e24aa', '0 0 0 0.2rem rgba(156,39,176,0.2)') },
];

@Component({
  selector: 'app-theme-config',
  template: `
    <div class="card">
      <div class="grid">
        <div class="col-12">
          <h5>Color Scheme</h5>
          <div class="flex gap-3">
            <div class="flex align-items-center">
              <p-radioButton name="colorScheme" [value]="'dark'" [(ngModel)]="selectedScheme"
                (onClick)="changeColorScheme('dark')" inputId="dark"></p-radioButton>
              <label for="dark" class="ml-2">Dark</label>
            </div>
            <div class="flex align-items-center">
              <p-radioButton name="colorScheme" [value]="'light'" [(ngModel)]="selectedScheme"
                (onClick)="changeColorScheme('light')" inputId="light"></p-radioButton>
              <label for="light" class="ml-2">Light</label>
            </div>
          </div>
        </div>

        <div class="col-12 mt-4">
          <h5>Theme Colors</h5>
          <p class="text-sm text-500 mt-0 mb-3">
            The accent color styles navigation, buttons and controls. Operational
            status colors (green = healthy, amber = caution, red = error) are fixed
            and never follow the accent.
          </p>
          <div class="grid gap-2">
            <div *ngFor="let theme of themes" class="col-4 sm:col-2 theme-color">
              <button pButton [class]="'p-button-rounded p-button-text color-dot'"
                      [style.backgroundColor]="theme.primaryColor"
                      style="width: 2rem; height: 2rem; border: none;"
                      (click)="changeTheme(theme)">
                <i *ngIf="theme.primaryColor === currentColor" class="pi pi-check selected-icon"></i>
              </button>
              <div class="text-sm mt-1">{{theme.name}}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  `,
  styleUrls: ['./design-component.scss']
})
export class ThemeConfigComponent implements OnInit, OnDestroy {
  selectedScheme: string;
  currentColor: string = '';
  themes: ThemeOption[] = THEME_PRESETS;

  private destroy$ = new Subject<void>();

  constructor(
    public layoutService: LayoutService,
    private themeService: ThemeService
  ) {
    this.selectedScheme = this.layoutService.config().colorScheme;
  }

  ngOnInit() {
    this.themeService.getThemeSettings()
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: (settings) => {
          if (settings) {
            if (settings.colorScheme) {
              this.selectedScheme = settings.colorScheme;
            }
            if (settings.accentColors) {
              this.applyThemeColors(settings.accentColors);
              this.currentColor = settings.accentColors['--primary-color'];
            }
          }
        },
        error: (error) => console.error('Error loading theme settings:', error)
      });
  }

  ngOnDestroy() {
    this.destroy$.next();
    this.destroy$.complete();
  }

  private applyThemeColors(colors: { [key: string]: string }) {
    // Allowlist-filtered: accent themes style chrome only and can never
    // override semantic operational colors (--nx-sem-*).
    applyAccentColors(colors);
  }

  changeColorScheme(scheme: string) {
    this.selectedScheme = scheme;
    const config = { ...this.layoutService.config() };
    config.colorScheme = scheme;
    this.layoutService.config.set(config);

    this.themeService.saveThemeSettings({ colorScheme: scheme })
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        error: (error) => console.error('Error saving theme settings:', error)
      });
  }

  changeTheme(theme: ThemeOption) {
    this.applyThemeColors(theme.accentColors);
    this.currentColor = theme.primaryColor;

    this.themeService.saveThemeSettings({
      colorScheme: this.selectedScheme,
      accentColors: theme.accentColors
    }).pipe(takeUntil(this.destroy$))
      .subscribe({
        error: (error) => console.error('Error saving theme settings:', error)
      });
  }
}
