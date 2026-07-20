import { of } from 'rxjs';
import { AppMenuComponent } from 'src/app/layout/app.menu.component';

/**
 * Navigation contract (Phase 2L): the Block Intelligence workspace is reachable
 * from the Neural Command Rail at the `bitcoin` route.
 */
describe('Block Intelligence navigation', () => {
  it('registers a Block Intelligence menu item at routerLink [bitcoin]', () => {
    const menu = new AppMenuComponent({} as any, { info$: of({}) } as any);
    menu.ngOnInit();
    const items = menu.model.flatMap((group: any) => group.items ?? []);
    const entry = items.find((i: any) => i.label === 'Block Intelligence');
    expect(entry).toBeDefined();
    expect(entry.routerLink).toEqual(['bitcoin']);
    expect(entry.icon).toContain('pi-box');
  });
});
