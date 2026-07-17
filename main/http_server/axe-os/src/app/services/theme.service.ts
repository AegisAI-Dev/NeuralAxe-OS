import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { environment } from '../../environments/environment';
import { BehaviorSubject, Observable, of } from 'rxjs';
import { catchError, tap } from 'rxjs/operators';

export interface ThemeSettings {
  colorScheme: string;
  accentColors?: {
    [key: string]: string;
  };
}

/**
 * The only CSS custom properties a user accent theme may set. Accent themes
 * style interactive chrome (buttons, sliders, checkboxes, highlights, focus)
 * — they must never be able to override semantic operational colors
 * (--nx-sem-*, --nx-green, --nx-red, …), which carry fixed meanings like
 * healthy/caution/error. Theme payloads are stored as free-form JSON in NVS,
 * so this allowlist is enforced at apply time on every path that writes
 * theme values to the document.
 */
export const ACCENT_COLOR_KEYS: ReadonlyArray<string> = [
  '--primary-color',
  '--primary-color-text',
  '--highlight-bg',
  '--highlight-text-color',
  '--focus-ring',
  '--slider-bg',
  '--slider-range-bg',
  '--slider-handle-bg',
  '--progressbar-bg',
  '--progressbar-value-bg',
  '--checkbox-border',
  '--checkbox-bg',
  '--checkbox-hover-bg',
  '--button-bg',
  '--button-hover-bg',
  '--button-focus-shadow',
  '--togglebutton-bg',
  '--togglebutton-border',
  '--togglebutton-hover-bg',
  '--togglebutton-hover-border',
  '--togglebutton-text-color',
];

/** Accent entries whose keys are on the allowlist; everything else is dropped. */
export function filterAccentColors(colors: { [key: string]: string } | undefined | null): [string, string][] {
  if (!colors) {
    return [];
  }
  return Object.entries(colors).filter(([key]) => ACCENT_COLOR_KEYS.includes(key));
}

/** Apply an accent-color payload to the document, allowlist-filtered. */
export function applyAccentColors(colors: { [key: string]: string } | undefined | null): void {
  filterAccentColors(colors).forEach(([key, value]) => {
    document.documentElement.style.setProperty(key, value);
  });
}

@Injectable({
  providedIn: 'root'
})
export class ThemeService {
  private readonly mockSettings: ThemeSettings = {
    colorScheme: 'dark',
    accentColors: {
      '--primary-color': '#4caf50',
      '--primary-color-text': '#ffffff',
      '--highlight-bg': '#4caf50',
      '--highlight-text-color': '#ffffff',
      '--focus-ring': '0 0 0 0.2rem rgba(76,175,80,0.2)',
      // PrimeNG Slider
      '--slider-bg': '#dee2e6',
      '--slider-range-bg': '#4caf50',
      '--slider-handle-bg': '#4caf50',
      // Progress Bar
      '--progressbar-bg': '#dee2e6',
      '--progressbar-value-bg': '#4caf50',
      // PrimeNG Checkbox
      '--checkbox-border': '#4caf50',
      '--checkbox-bg': '#4caf50',
      '--checkbox-hover-bg': '#43a047',
      // PrimeNG Button
      '--button-bg': '#4caf50',
      '--button-hover-bg': '#43a047',
      '--button-focus-shadow': '0 0 0 2px #ffffff, 0 0 0 4px #4caf50',
      // Toggle button
      '--togglebutton-bg': '#4caf50',
      '--togglebutton-border': '1px solid #4caf50',
      '--togglebutton-hover-bg': '#43a047',
      '--togglebutton-hover-border': '1px solid #43a047',
      '--togglebutton-text-color': '#ffffff'
    }
  };

  private themeSettingsSubject = new BehaviorSubject<ThemeSettings>(this.mockSettings);
  private themeSettings$ = this.themeSettingsSubject.asObservable();

  constructor(private http: HttpClient) {
    if (environment.production) {
      this.http.get<ThemeSettings>('/api/theme').pipe(
        catchError(() => of(this.mockSettings)),
        tap(settings => this.themeSettingsSubject.next(settings))
      ).subscribe();
    }
  }

  getThemeSettings(): Observable<ThemeSettings> {
    return this.themeSettings$;
  }

  saveThemeSettings(settings: ThemeSettings): Observable<void> {
    if (environment.production) {
      return this.http.post<void>('/api/theme', settings).pipe(
        tap(() => this.themeSettingsSubject.next(settings))
      );
    } else {
      this.themeSettingsSubject.next(settings);
      return of(void 0);
    }
  }
}
