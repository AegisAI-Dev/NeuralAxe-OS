import { of } from 'rxjs';
import { AppMenuComponent } from './app.menu.component';

describe('AppMenuComponent (shell route consistency)', () => {
  let component: AppMenuComponent;

  beforeEach(() => {
    component = new AppMenuComponent({} as any, { info$: of() } as any);
    component.ngOnInit();
  });

  function items(): any[] {
    return component.model[0].items;
  }

  it('should expose every NeuralAxe shell route exactly once', () => {
    const links = items().filter(i => i.routerLink).map(i => String(i.routerLink[0]));
    expect(links).toEqual(['/', 'classic', 'scoreboard', 'swarm', 'logs', 'system', 'pool', 'network', 'design', 'settings', 'update']);
    expect(new Set(links).size).toBe(links.length);
  });

  it('should lead with Overview and keep the whitepaper accessible', () => {
    expect(items()[0].label).toBe('Overview');
    expect(items().some(i => i.label === 'Whitepaper' && typeof i.command === 'function')).toBeTrue();
  });

  it('should not carry upstream primary branding in labels', () => {
    for (const item of items()) {
      expect(item.label ?? '').not.toContain('AxeOS');
      expect(item.label ?? '').not.toContain('Bitaxe');
    }
  });
});
