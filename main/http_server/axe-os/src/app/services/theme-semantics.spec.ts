import { ACCENT_COLOR_KEYS, applyAccentColors, filterAccentColors } from './theme.service';
import { THEME_PRESETS } from '../components/design/theme-config.component';

/**
 * Stage 2G semantic-color guarantees: the user accent theme (including the
 * red one that triggered this work) must never change the meaning-bearing
 * operational colors. Verified at two levels:
 *  - the apply path drops any key that is not an allowlisted accent key;
 *  - with the real global stylesheet loaded, a semantic telemetry meter keeps
 *    its computed color under EVERY selectable theme, while a plain
 *    accent-driven progress bar still follows the theme.
 */
describe('theme semantics', () => {

  afterEach(() => {
    // Remove any accent properties a test applied to the real document root.
    ACCENT_COLOR_KEYS.forEach(key => document.documentElement.style.removeProperty(key));
  });

  describe('accent allowlist', () => {
    it('every built-in theme uses only allowlisted accent keys (nothing is silently dropped)', () => {
      for (const theme of THEME_PRESETS) {
        const kept = filterAccentColors(theme.accentColors).map(([key]) => key);
        expect(kept.length)
          .withContext(`theme ${theme.name}`)
          .toBe(Object.keys(theme.accentColors).length);
      }
    });

    it('drops semantic and arbitrary keys from a stored theme payload', () => {
      const hostile = {
        '--primary-color': '#F80421',
        '--nx-sem-ok': '#ff0000',        // semantic override attempt
        '--nx-green': '#ff0000',
        '--surface-card': '#ff0000',     // layout override attempt
        'background': 'red',
      };
      const kept = filterAccentColors(hostile).map(([key]) => key);
      expect(kept).toEqual(['--primary-color']);
    });

    it('applyAccentColors never writes non-allowlisted properties to the document', () => {
      applyAccentColors({ '--nx-sem-danger': '#00ff00', '--primary-color': '#F80421' });
      expect(document.documentElement.style.getPropertyValue('--nx-sem-danger')).toBe('');
      expect(document.documentElement.style.getPropertyValue('--primary-color')).toBe('#F80421');
    });

    it('handles empty and missing payloads safely', () => {
      expect(filterAccentColors(undefined)).toEqual([]);
      expect(filterAccentColors(null)).toEqual([]);
      expect(() => applyAccentColors(undefined)).not.toThrow();
    });
  });

  describe('computed colors under every selectable theme', () => {
    let host: HTMLElement;
    let semanticValue: HTMLElement;
    let accentValue: HTMLElement;

    beforeEach(() => {
      host = document.createElement('div');
      host.innerHTML = `
        <div class="p-progressbar nx-meter-ok"><div id="sem" class="p-progressbar-value" style="width:50%"></div></div>
        <div class="p-progressbar"><div id="acc" class="p-progressbar-value" style="width:50%"></div></div>
      `;
      document.body.appendChild(host);
      semanticValue = host.querySelector('#sem') as HTMLElement;
      accentValue = host.querySelector('#acc') as HTMLElement;
    });

    afterEach(() => host.remove());

    it('semantic ok meter stays green under every theme; plain accent bar follows the theme', () => {
      const semanticBaseline = getComputedStyle(semanticValue).backgroundColor;
      // Guard: the global stylesheet must actually be loaded in the test bundle.
      expect(semanticBaseline).toBe('rgb(47, 230, 160)'); // --nx-sem-ok → --nx-green

      for (const theme of THEME_PRESETS) {
        applyAccentColors(theme.accentColors);
        expect(getComputedStyle(semanticValue).backgroundColor)
          .withContext(`semantic meter under theme ${theme.name}`)
          .toBe(semanticBaseline);
        expect(getComputedStyle(accentValue).backgroundColor)
          .withContext(`accent bar under theme ${theme.name}`)
          .toBe(hexToRgb(theme.primaryColor));
      }
    });

    it('semantic danger meter is red even under the green default theme', () => {
      semanticValue.parentElement!.classList.remove('nx-meter-ok');
      semanticValue.parentElement!.classList.add('nx-meter-danger');
      const green = THEME_PRESETS.find(t => t.name.startsWith('Green'))!;
      applyAccentColors(green.accentColors);
      expect(getComputedStyle(semanticValue).backgroundColor).toBe('rgb(239, 83, 80)'); // --nx-red
    });
  });
});

function hexToRgb(hex: string): string {
  const value = hex.replace('#', '');
  const r = parseInt(value.substring(0, 2), 16);
  const g = parseInt(value.substring(2, 4), 16);
  const b = parseInt(value.substring(4, 6), 16);
  return `rgb(${r}, ${g}, ${b})`;
}
