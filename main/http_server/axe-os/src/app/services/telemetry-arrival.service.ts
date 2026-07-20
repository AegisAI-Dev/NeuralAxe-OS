import { Injectable } from '@angular/core';
import { Observable } from 'rxjs';
import { map, shareReplay } from 'rxjs/operators';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LiveDataService } from './live-data.service';

/**
 * A single genuine telemetry arrival, stamped with a monotonic ARRIVAL IDENTITY
 * at the shared telemetry boundary.
 *
 * The `arrivalId` — not the payload content — is the identity used to deduplicate
 * evidence in the Stability Lab. This is the reliability contract that lets the
 * Lab tell apart:
 *   - a genuinely new telemetry arrival whose payload happens to be byte-identical
 *     to the previous one (identical temps/power/fan/counters) → a NEW id;
 *   - a shareReplay replay, a component re-subscription, or a reconnect replay of
 *     the last arrival → the SAME id (never double-counted);
 *   - an Angular re-render → no arrival at all (the stream does not emit).
 *
 * Using content difference would silently drop genuine-but-identical readings and
 * undercount coverage; using arrival identity does not.
 */
export interface TelemetryArrival {
  info: ISystemInfo;
  /** Monotonic per-process arrival sequence number (starts at 1). */
  arrivalId: number;
}

@Injectable({ providedIn: 'root' })
export class TelemetryArrivalService {
  private seq = 0;

  /**
   * The ONE stream that mints arrival identities. `map(++seq)` sits above a
   * `shareReplay({ bufferSize: 1, refCount: false })`, which guarantees exactly
   * ONE upstream subscription to `LiveDataService.info$` for the whole app
   * lifetime. Therefore:
   *   - the counter advances exactly once per genuine source emission;
   *   - every downstream subscriber (and every replay to a late/re-subscriber)
   *     observes the SAME id for the SAME underlying emission;
   *   - `refCount: false` is essential — with refCount the upstream would be torn
   *     down when the last subscriber leaves and RE-subscribed on the next
   *     subscribe, which would re-run the map and mint a NEW id for a replayed
   *     arrival, breaking the dedup contract.
   *
   * No second miner polling loop is created — this only wraps the existing shared
   * telemetry stream.
   */
  public readonly arrivals$: Observable<TelemetryArrival>;

  constructor(private liveData: LiveDataService) {
    this.arrivals$ = this.liveData.info$.pipe(
      map((info) => ({ info, arrivalId: ++this.seq })),
      shareReplay({ bufferSize: 1, refCount: false }),
    );
  }
}
