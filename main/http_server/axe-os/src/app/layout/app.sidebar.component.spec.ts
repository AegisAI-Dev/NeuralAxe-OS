import { Component } from '@angular/core';
import { ComponentFixture, TestBed, fakeAsync, tick } from '@angular/core/testing';
import { Router } from '@angular/router';
import { RouterTestingModule } from '@angular/router/testing';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { RippleModule } from 'primeng/ripple';
import { of } from 'rxjs';

import { AppSidebarComponent } from './app.sidebar.component';
import { AppMenuComponent } from './app.menu.component';
import { AppMenuitemComponent } from './app.menuitem.component';
import { LiveDataService } from '../services/live-data.service';
import { LayoutService } from './service/app.layout.service';

@Component({ template: '' })
class DummyComponent {}

/** Classes that carry SEMANTIC health/telemetry meaning — navigation must
 *  never borrow any of them (Stage 12 semantic safety). */
const HEALTH_CLASSES = [
  'nx-health-err', 'nx-health-warn', 'nx-health-ok', 'nx-health-off',
  'nx-sem-danger', 'nx-danger-text', 'nx-warn-text', 'nx-ok-text',
  'nx-pill-err', 'nx-pill-warn', 'nx-fleet-nav-critical',
];

describe('AppSidebarComponent (Neural Command Rail, Phase 2J)', () => {
  let fixture: ComponentFixture<AppSidebarComponent>;
  let component: AppSidebarComponent;
  let router: Router;

  const mockInfo = { hostname: 'gamma-lab-01', ASICModel: 'BM1370', uptimeSeconds: 1234 };
  const layoutStub = { state: { staticMenuMobileActive: false }, isDesktop: () => true } as any;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [AppSidebarComponent, AppMenuComponent, AppMenuitemComponent, DummyComponent],
      imports: [
        RippleModule,
        NoopAnimationsModule,
        RouterTestingModule.withRoutes([
          { path: '', pathMatch: 'full', component: DummyComponent },
          { path: 'swarm', component: DummyComponent },
          { path: 'update', component: DummyComponent },
          { path: 'scoreboard', component: DummyComponent },
          { path: 'pool', component: DummyComponent },
          { path: 'network', component: DummyComponent },
          { path: 'settings', component: DummyComponent },
          { path: 'design', component: DummyComponent },
          { path: 'system', component: DummyComponent },
          { path: 'logs', component: DummyComponent },
          { path: 'about', component: DummyComponent },
          { path: 'classic', component: DummyComponent },
        ]),
      ],
      providers: [
        { provide: LiveDataService, useValue: { info$: of(mockInfo) } },
        { provide: LayoutService, useValue: layoutStub },
      ],
    });
    router = TestBed.inject(Router);
    fixture = TestBed.createComponent(AppSidebarComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('creates', () => {
    expect(component).toBeTruthy();
  });

  describe('Active Unit Dock (Stage 7)', () => {
    it('presents THIS device as a hardware module using existing data only', () => {
      const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
      expect(fixture.nativeElement.querySelector('.nx-unit-dock')).toBeTruthy();
      expect(text).toContain('Active');            // "Active Unit" tag
      expect(text).toContain('Gamma 601');         // target device + board
      expect(text).toContain('BM1370');            // ASIC
      expect(text).toContain('NeuralAxe');         // classification chip
    });

    it('masks the hostname for privacy mode via [sensitive-data]', () => {
      const host = fixture.nativeElement.querySelector('.nx-unit-dock-host [sensitive-data]');
      expect(host).toBeTruthy();
      expect(host.textContent).toContain('gamma-lab-01');
    });

    it('invents no live telemetry (no hashrate/power/temp in the dock)', () => {
      const dockText = fixture.nativeElement.querySelector('.nx-unit-dock')?.textContent ?? '';
      expect(dockText).not.toContain('GH/s');
      expect(dockText).not.toContain('J/TH');
      expect(dockText).not.toContain('°C');
      expect(dockText).not.toMatch(/\d+(\.\d+)?\s*W\b/); // watts
    });
  });

  describe('command rail structure', () => {
    it('renders the six product sections and all thirteen route links', () => {
      const roots = fixture.nativeElement.querySelectorAll('.layout-menuitem-root-text');
      expect(roots.length).toBe(6);
      const leafLinks = Array.from(fixture.nativeElement.querySelectorAll('.layout-menu a'))
        .filter((a: any) => a.getAttribute('href') !== null);
      expect(leafLinks.length).toBe(13); // + Stability Lab (Phase 2K)
    });

    it('renders every navigation label in full (2J.1 readability)', () => {
      const labels = Array.from(fixture.nativeElement.querySelectorAll('.layout-menuitem-text'))
        .map((el: any) => el.textContent.trim());
      for (const expected of [
        'Command Deck', 'Scoreboard', 'Fleet', 'Pools', 'Network',
        'Tuning & Thermal', 'Stability Lab', 'Display & Appearance', 'Device Status', 'Logs',
        'Updates', 'About NeuralAxe', 'Legacy Dashboard',
      ]) {
        expect(labels).withContext(expected).toContain(expected);
      }
    });
  });

  describe('active-route semantics (Stages 4/12)', () => {
    it('marks the active route with the nav-specific class, never a health class', fakeAsync(() => {
      router.navigate(['/swarm']);
      tick();
      fixture.detectChanges();

      const active: HTMLElement | null = fixture.nativeElement.querySelector('a.active-route');
      expect(active).toBeTruthy();
      expect(active!.textContent).toContain('Fleet');
      for (const cls of HEALTH_CLASSES) {
        expect(active!.classList.contains(cls)).withContext(cls).toBeFalse();
      }
    }));

    it('activates exactly one item at a time and follows navigation', fakeAsync(() => {
      router.navigate(['/update']);
      tick();
      fixture.detectChanges();
      let actives = fixture.nativeElement.querySelectorAll('a.active-route');
      expect(actives.length).toBe(1);
      expect((actives[0] as HTMLElement).textContent).toContain('Updates');

      router.navigate(['/scoreboard']);
      tick();
      fixture.detectChanges();
      actives = fixture.nativeElement.querySelectorAll('a.active-route');
      expect(actives.length).toBe(1);
      expect((actives[0] as HTMLElement).textContent).toContain('Scoreboard');
    }));
  });
});
