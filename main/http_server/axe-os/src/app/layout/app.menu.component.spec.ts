import { of } from 'rxjs';
import { AppMenuComponent } from './app.menu.component';

describe('AppMenuComponent (NeuralAxe navigation architecture)', () => {
  let component: AppMenuComponent;

  beforeEach(() => {
    component = new AppMenuComponent({} as any, { info$: of() } as any);
    component.ngOnInit();
  });

  function sections(): any[] {
    return component.model;
  }

  function flatItems(): any[] {
    return sections().flatMap(s => s.items ?? []);
  }

  it('groups navigation into the NeuralAxe product sections', () => {
    expect(sections().map(s => s.label)).toEqual([
      'Overview', 'Mining', 'Configuration', 'System', 'Product', 'Advanced',
    ]);
    for (const section of sections()) {
      expect(section.items.length).toBeGreaterThan(0);
    }
  });

  it('keeps every existing route reachable exactly once (deep-link compatibility)', () => {
    const links = flatItems().filter(i => i.routerLink).map(i => String(i.routerLink[0]));
    expect(links.sort()).toEqual([
      '/', 'about', 'classic', 'design', 'logs', 'network',
      'pool', 'scoreboard', 'settings', 'swarm', 'system', 'update',
    ].sort());
    expect(new Set(links).size).toBe(links.length);
  });

  it('uses NeuralAxe product names, not the upstream page list', () => {
    const byRoute = new Map(flatItems().filter(i => i.routerLink)
      .map(i => [String(i.routerLink[0]), i.label]));
    expect(byRoute.get('/')).toBe('Command Deck');
    expect(byRoute.get('swarm')).toBe('Fleet');
    expect(byRoute.get('settings')).toBe('Tuning & Thermal');
    expect(byRoute.get('design')).toBe('Display & Appearance');
    expect(byRoute.get('system')).toBe('Device Status');
    expect(byRoute.get('update')).toBe('Updates');
    expect(byRoute.get('about')).toBe('About NeuralAxe');
  });

  it('demotes Classic to a clearly labeled legacy location, not a second home', () => {
    const advanced = sections().find(s => s.label === 'Advanced')!;
    const legacy = advanced.items.find((i: any) => String(i.routerLink?.[0]) === 'classic');
    expect(legacy?.label).toBe('Legacy Dashboard');
    expect(legacy?.title).toContain('upstream-compatible');
    const overview = sections().find(s => s.label === 'Overview')!;
    expect(overview.items.length).toBe(1);
  });

  it('has no top-level whitepaper entry (reachable from About instead)', () => {
    expect(flatItems().some(i => i.label === 'Whitepaper')).toBeFalse();
  });

  it('carries no upstream primary branding in labels', () => {
    for (const item of [...sections(), ...flatItems()]) {
      expect(item.label ?? '').not.toContain('AxeOS');
      expect(item.label ?? '').not.toContain('Bitaxe');
    }
  });
});
