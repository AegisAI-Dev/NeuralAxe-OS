/**
 * NeuralAxe Pool Strategy service (Phase 2M).
 *
 * The single owner of Pool Strategy Center persistence and the shared, honest
 * chain context. It keeps profiles, the active-profile record, the bounded
 * restore snapshot and the sanitized switch history in localStorage, and derives
 * `chainContext$` from the stored active record verified against live telemetry.
 *
 * Command Deck and Block Intelligence consume `chainContext$` (read-only) so the
 * chain label they show is the SAME honest value the Center computes — never a
 * hostname guess.
 *
 * PRIVACY: nothing here is ever sent to any network. Profiles and the restore
 * snapshot hold credentials locally only (masked in every UI, excluded from
 * exports/history); passwords are stored only for explicit 'set' endpoints.
 */

import { Injectable } from '@angular/core';
import { BehaviorSubject, Observable } from 'rxjs';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LiveDataService } from './live-data.service';
import { LocalStorageService } from '../local-storage.service';
import {
  PoolProfile, cloneProfile, isPoolChain, nextProfileId,
} from '../components/pool-strategy/pool-profile';
import {
  ActivePoolRecord, ChainContext, deriveChainContext,
} from '../components/pool-strategy/pool-chain';
import {
  PoolHistoryStore, PoolSwitchRecord, RestoreSnapshot, isRestoreValid,
} from '../components/pool-strategy/pool-history';
import { SwitchInterruptionRecord } from '../components/pool-strategy/pool-recovery';

const PROFILES_KEY = 'NX_POOL_PROFILES';
const ACTIVE_KEY = 'NX_POOL_ACTIVE';
const RESTORE_KEY = 'NX_POOL_RESTORE_SNAPSHOT';
/** Flag proving a switch was mid-flight (browser-interruption detection). */
export const SWITCH_ACTIVE_FLAG = 'NX_POOL_SWITCH_ACTIVE';

@Injectable({ providedIn: 'root' })
export class PoolStrategyService {
  private readonly profilesSubject: BehaviorSubject<PoolProfile[]>;
  private readonly activeSubject: BehaviorSubject<ActivePoolRecord | null>;
  private readonly chainContextSubject: BehaviorSubject<ChainContext>;
  private readonly historyStore: PoolHistoryStore;

  public readonly profiles$: Observable<PoolProfile[]>;
  public readonly activeRecord$: Observable<ActivePoolRecord | null>;
  public readonly chainContext$: Observable<ChainContext>;

  private latestInfo: ISystemInfo | null = null;

  constructor(
    private liveData: LiveDataService,
    private storage: LocalStorageService,
  ) {
    this.historyStore = new PoolHistoryStore(this.storage);
    this.profilesSubject = new BehaviorSubject<PoolProfile[]>(this.loadProfiles());
    this.activeSubject = new BehaviorSubject<ActivePoolRecord | null>(this.loadActive());
    this.chainContextSubject = new BehaviorSubject<ChainContext>(deriveChainContext(this.activeSubject.value, null));

    this.profiles$ = this.profilesSubject.asObservable();
    this.activeRecord$ = this.activeSubject.asObservable();
    this.chainContext$ = this.chainContextSubject.asObservable();

    // Re-derive the chain context whenever live telemetry updates. The block
    // service already keeps info$ alive app-wide; this adds one cheap read.
    this.liveData.info$.subscribe(info => {
      this.latestInfo = info;
      this.recomputeChainContext();
    });
  }

  // ---- profiles ------------------------------------------------------------

  get profiles(): PoolProfile[] {
    return this.profilesSubject.value;
  }

  addProfile(draft: Omit<PoolProfile, 'id' | 'createdAt' | 'updatedAt'>): PoolProfile {
    const now = Date.now();
    const profile: PoolProfile = {
      ...draft,
      primary: { ...draft.primary },
      fallback: draft.fallback ? { ...draft.fallback } : null,
      id: nextProfileId(),
      createdAt: now,
      updatedAt: now,
    };
    const next = [...this.profiles, profile];
    this.persistProfiles(next);
    return profile;
  }

  updateProfile(profile: PoolProfile): void {
    const next = this.profiles.map(p => (p.id === profile.id ? { ...cloneProfile(profile), updatedAt: Date.now() } : p));
    this.persistProfiles(next);
  }

  removeProfile(id: string): void {
    this.persistProfiles(this.profiles.filter(p => p.id !== id));
  }

  private persistProfiles(list: PoolProfile[]): void {
    this.storage.setObject(PROFILES_KEY, list);
    this.profilesSubject.next(list);
  }

  private loadProfiles(): PoolProfile[] {
    const raw = this.storage.getObject(PROFILES_KEY);
    if (!Array.isArray(raw)) return [];
    // Defensive: keep only well-formed profiles with a valid chain label.
    return raw.filter(p => p && typeof p.id === 'string' && isPoolChain(p.chain) && p.primary);
  }

  // ---- active record + chain context --------------------------------------

  get activeRecord(): ActivePoolRecord | null {
    return this.activeSubject.value;
  }

  setActiveRecord(record: ActivePoolRecord): void {
    this.storage.setObject(ACTIVE_KEY, record);
    this.activeSubject.next(record);
    this.recomputeChainContext();
  }

  clearActiveRecord(): void {
    this.storage.setObject(ACTIVE_KEY, null as any);
    this.activeSubject.next(null);
    this.recomputeChainContext();
  }

  private loadActive(): ActivePoolRecord | null {
    const raw = this.storage.getObject(ACTIVE_KEY);
    if (raw && typeof raw === 'object' && typeof raw.profileId === 'string' && isPoolChain(raw.chain)) {
      return raw as ActivePoolRecord;
    }
    return null;
  }

  chainContext(): ChainContext {
    return this.chainContextSubject.value;
  }

  private recomputeChainContext(): void {
    this.chainContextSubject.next(deriveChainContext(this.activeSubject.value, this.latestInfo));
  }

  get info(): ISystemInfo | null {
    return this.latestInfo;
  }

  // ---- restore snapshot ----------------------------------------------------

  getRestoreSnapshot(now: number = Date.now()): RestoreSnapshot | null {
    const raw = this.storage.getObject(RESTORE_KEY) as RestoreSnapshot | null;
    if (isRestoreValid(raw, now)) {
      return raw;
    }
    // Expired / malformed — clear it so it never lingers.
    if (raw) this.clearRestoreSnapshot();
    return null;
  }

  setRestoreSnapshot(snapshot: RestoreSnapshot): void {
    this.storage.setObject(RESTORE_KEY, snapshot);
  }

  clearRestoreSnapshot(): void {
    this.storage.setObject(RESTORE_KEY, null as any);
  }

  // ---- history -------------------------------------------------------------

  listHistory(): PoolSwitchRecord[] {
    return this.historyStore.list();
  }

  saveHistory(record: PoolSwitchRecord): PoolSwitchRecord[] {
    return this.historyStore.save(record);
  }

  removeHistory(id: string): PoolSwitchRecord[] {
    return this.historyStore.remove(id);
  }

  clearHistory(): void {
    this.historyStore.clear();
  }

  // ---- interruption record (NON-SECRET) ------------------------------------
  // Persisted the instant a switch starts so a browser interruption can be
  // recovered honestly. It NEVER contains a password — only non-secret
  // host/port/user identities and flags. Cleared when the operation finishes.

  setInterruptionRecord(record: SwitchInterruptionRecord): void {
    this.storage.setObject(SWITCH_ACTIVE_FLAG, record);
  }

  clearSwitchActive(): void {
    this.storage.setObject(SWITCH_ACTIVE_FLAG, null as any);
  }

  getInterruptionRecord(): SwitchInterruptionRecord | null {
    const raw = this.storage.getObject(SWITCH_ACTIVE_FLAG);
    if (raw && typeof raw === 'object' && raw.original && raw.target) {
      return raw as SwitchInterruptionRecord;
    }
    return null;
  }

  wasSwitchInterrupted(): boolean {
    return !!this.storage.getObject(SWITCH_ACTIVE_FLAG);
  }
}
