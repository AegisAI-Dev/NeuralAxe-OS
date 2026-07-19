import { Injectable } from '@angular/core';
import { CanDeactivate } from '@angular/router';

/** Components that can veto navigation (e.g. an active Stability Lab session). */
export interface CanComponentDeactivate {
  canDeactivate: () => boolean;
}

/**
 * Route guard: a component with an active session confirms before the owner
 * navigates away, so an in-progress run is never abandoned silently.
 */
@Injectable({ providedIn: 'root' })
export class StabilityLabGuard implements CanDeactivate<CanComponentDeactivate> {
  canDeactivate(component: CanComponentDeactivate): boolean {
    return component?.canDeactivate ? component.canDeactivate() : true;
  }
}
