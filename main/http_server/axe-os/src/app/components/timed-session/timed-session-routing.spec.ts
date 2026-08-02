/**
 * Gate B9 — route and navigation registration.
 *
 * Proves the operator can actually reach the page, that it sits where the
 * information architecture says it should, and that nothing Weather-Aware is
 * introduced anywhere near it.
 */

import { TestBed } from '@angular/core/testing';
import { Route, Routes } from '@angular/router';
import { of } from 'rxjs';

import { routes } from 'src/app/app-routing.module';
import { AppMenuComponent } from 'src/app/layout/app.menu.component';
import { LayoutService } from 'src/app/layout/service/app.layout.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { TimedSessionComponent } from './timed-session.component';

/** Every route in the table, flattened across `children`. */
function flatten(list: Routes): Route[] {
  return list.flatMap((r) => [r, ...(r.children ? flatten(r.children) : [])]);
}

interface MenuGroup { label: string; items: { label: string; routerLink: string[]; icon: string }[] }

describe('routing: the timed-session page is reachable', () => {
  const all = flatten(routes);

  it('registers the timed-session path with the product title', () => {
    const matches = all.filter((r) => r.path === 'timed-session');
    expect(matches.length).withContext('registered exactly once').toBe(1);
    expect(matches[0].component).toBe(TimedSessionComponent);
    expect(String(matches[0].title)).toContain('Timed Pool Session');
  });

  it('sits inside the guarded application shell, beside the other configuration pages', () => {
    const shell = routes.find((r) => r.path === '' && (r.children?.length ?? 0) > 1);
    expect(shell).withContext('the guarded shell exists').toBeDefined();
    const childPaths = (shell!.children ?? []).map((c) => c.path);
    expect(childPaths).toContain('timed-session');
    expect(childPaths).toContain('pool-strategy');
    // NOT in the AP-mode branch.
    const ap = routes.find((r) => r.path === 'ap');
    expect((ap?.children ?? []).some((c) => c.path === 'timed-session')).toBeFalse();
  });

  it('introduces no Weather-Aware route', () => {
    expect(all.some((r) => /weather/i.test(String(r.path ?? '')))).toBeFalse();
  });

  it('leaves every existing route intact', () => {
    const paths = all.map((r) => r.path);
    ['', 'ap', 'classic', 'logs', 'system', 'update', 'network', 'settings', 'stability-lab',
      'swarm', 'scoreboard', 'bitcoin', 'design', 'pool', 'pool-strategy', 'about']
      .forEach((p) => expect(paths).withContext(p).toContain(p));
  });
});

describe('navigation: the menu entry', () => {
  let model: MenuGroup[];

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [AppMenuComponent],
      providers: [
        { provide: LayoutService, useValue: {} },
        { provide: LiveDataService, useValue: { info$: of({}) } },
      ],
    });
    const fixture = TestBed.createComponent(AppMenuComponent);
    fixture.componentInstance.ngOnInit();
    model = fixture.componentInstance.model as MenuGroup[];
  });

  afterEach(() => TestBed.resetTestingModule());

  it('appears once, in the Configuration group', () => {
    const config = model.find((g) => g.label === 'Configuration');
    expect(config).toBeDefined();
    const entries = config!.items.filter((i) => i.routerLink?.[0] === 'timed-session');
    expect(entries.length).toBe(1);
    expect(entries[0].label).toBe('Timed Pool Session');
  });

  it('sits immediately after Pool Strategy', () => {
    const config = model.find((g) => g.label === 'Configuration')!;
    const strategyIndex = config.items.findIndex((i) => i.routerLink?.[0] === 'pool-strategy');
    const sessionIndex = config.items.findIndex((i) => i.routerLink?.[0] === 'timed-session');
    expect(strategyIndex).toBeGreaterThanOrEqual(0);
    expect(sessionIndex).toBe(strategyIndex + 1);
  });

  it('is not hidden inside tuning and introduces no weather entry', () => {
    const allItems = model.flatMap((g) => g.items ?? []);
    const session = allItems.find((i) => i.routerLink?.[0] === 'timed-session')!;
    expect(session.label).not.toMatch(/tuning|thermal/i);
    expect(allItems.some((i) => /weather/i.test(i.label))).toBeFalse();
  });

  it('does not disturb the existing entries', () => {
    const paths = model.flatMap((g) => g.items ?? []).map((i) => i.routerLink?.[0]);
    ['pool', 'pool-strategy', 'network', 'settings', 'stability-lab', 'design',
      'scoreboard', 'bitcoin', 'swarm', 'system', 'logs', 'update', 'about', 'classic']
      .forEach((p) => expect(paths).withContext(p).toContain(p));
  });
});
