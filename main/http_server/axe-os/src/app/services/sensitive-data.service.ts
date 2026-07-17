import { Injectable } from '@angular/core';
import { BehaviorSubject } from 'rxjs';
import { LocalStorageService } from 'src/app/local-storage.service';

const SENSITIVE_DATA_HIDDEN = 'SENSITIVE_DATA_HIDDEN';

@Injectable({ providedIn: 'root' })
export class SensitiveData {
  private hidden$: BehaviorSubject<boolean>;

  constructor(private localStorageService: LocalStorageService) {
    // Privacy-safe default: with NO stored preference (fresh browser profile,
    // first visit, screenshot capture session) sensitive data starts HIDDEN.
    // An explicit user choice ('true'/'false') is always respected.
    // (getBool cannot express "missing", it folds missing into false — which
    // silently made fresh profiles start VISIBLE; read the raw value instead.)
    const raw = this.localStorageService.getItem(SENSITIVE_DATA_HIDDEN);
    const hidden = raw === null || raw === undefined ? true : raw === 'true';

    this.hidden$ = new BehaviorSubject<boolean>(hidden);
  }

  get hidden() {
    return this.hidden$.asObservable();
  }

  toggle() {
    const newState = !this.hidden$.value;

    this.hidden$.next(newState);
    this.localStorageService.setBool(SENSITIVE_DATA_HIDDEN, newState);
  }
}
