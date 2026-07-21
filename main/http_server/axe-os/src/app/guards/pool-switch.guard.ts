import { Injectable } from '@angular/core';
import { CanDeactivate } from '@angular/router';
import { CanComponentDeactivate } from './stability-lab.guard';

/**
 * Route guard: the Pool Strategy Center confirms before the owner navigates away
 * while a pool switch (or rollback / restore) is in progress, so an in-flight
 * switch is never abandoned silently. Reuses the shared CanComponentDeactivate
 * contract.
 */
@Injectable({ providedIn: 'root' })
export class PoolSwitchGuard implements CanDeactivate<CanComponentDeactivate> {
  canDeactivate(component: CanComponentDeactivate): boolean {
    return component?.canDeactivate ? component.canDeactivate() : true;
  }
}
