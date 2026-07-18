import { of } from 'rxjs';
import { AppLayoutComponent } from './app.layout.component';

/**
 * Mobile navigation drawer behaviour (Phase 2J). The layout component owns the
 * open/close state machine; these unit tests exercise it directly with stubbed
 * collaborators (the full template tree is not needed to verify the logic).
 */
describe('AppLayoutComponent (mobile drawer + escape, Phase 2J)', () => {
  function make(stateOverrides: any = {}) {
    const state = {
      staticMenuMobileActive: false,
      overlayMenuActive: false,
      menuHoverActive: false,
      staticMenuDesktopInactive: false,
      ...stateOverrides,
    };
    const layoutService = {
      state,
      overlayOpen$: of(),
      config: () => ({ menuMode: 'static', colorScheme: 'dark', inputStyle: 'outlined', ripple: false, scale: 14 }),
    } as any;
    const renderer = { listen: () => () => {} } as any;
    const router = { events: of(), url: '/' } as any;
    const sensitiveData = { hidden: of(false) } as any;
    const comp = new AppLayoutComponent(layoutService, renderer, router, sensitiveData);
    return { comp, state };
  }

  it('Escape closes the mobile drawer when it is open, and returns focus safely', () => {
    const { comp, state } = make({ staticMenuMobileActive: true });
    expect(() => comp.onEscapeKey()).not.toThrow(); // appTopbar ViewChild absent -> optional chain
    expect(state.staticMenuMobileActive).toBeFalse();
  });

  it('Escape is a no-op with the drawer closed (desktop static rail unaffected)', () => {
    const { comp, state } = make({ staticMenuMobileActive: false });
    comp.onEscapeKey();
    expect(state.staticMenuMobileActive).toBeFalse();
  });

  it('hideMenu resets overlay/mobile/hover state (backdrop + navigation close path)', () => {
    const { comp, state } = make({ staticMenuMobileActive: true, overlayMenuActive: true, menuHoverActive: true });
    comp.hideMenu();
    expect(state.staticMenuMobileActive).toBeFalse();
    expect(state.overlayMenuActive).toBeFalse();
    expect(state.menuHoverActive).toBeFalse();
  });

  it('containerClass reflects the mobile-active drawer state', () => {
    const openState = make({ staticMenuMobileActive: true });
    expect(openState.comp.containerClass['layout-mobile-active']).toBeTrue();
    const closedState = make({ staticMenuMobileActive: false });
    expect(closedState.comp.containerClass['layout-mobile-active']).toBeFalse();
  });
});
