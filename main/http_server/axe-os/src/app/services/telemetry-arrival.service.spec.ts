import { Subject, BehaviorSubject } from 'rxjs';
import { TelemetryArrivalService, TelemetryArrival } from './telemetry-arrival.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

const info = (over: Partial<ISystemInfo> = {}): ISystemInfo => ({
  version: 'v', uptimeSeconds: 1, hashRate: 1290, temp: 55, power: 21,
  ...over,
} as ISystemInfo);

/** Build the service over a controllable info$ source. */
function make(source: any) {
  return new TelemetryArrivalService({ info$: source } as any);
}

describe('TelemetryArrivalService — single monotonic arrival identity', () => {
  it('assigns a monotonically increasing id, once per source emission', () => {
    const src = new Subject<ISystemInfo>();
    const svc = make(src);
    const ids: number[] = [];
    svc.arrivals$.subscribe(a => ids.push(a.arrivalId));
    src.next(info());
    src.next(info({ hashRate: 1291 }));
    src.next(info({ hashRate: 1290 })); // identical to the first — still a new id
    expect(ids).toEqual([1, 2, 3]);
  });

  it('is the SINGLE assigner — a late subscriber shares ids, and a replay carries the SAME id', () => {
    const src = new Subject<ISystemInfo>();
    const svc = make(src);
    const ids1: number[] = [];
    svc.arrivals$.subscribe(a => ids1.push(a.arrivalId));
    src.next(info());          // id 1
    src.next(info());          // id 2 (identical payload, new id)
    const ids2: number[] = [];
    svc.arrivals$.subscribe(a => ids2.push(a.arrivalId)); // replays the buffered arrival
    expect(ids2[0]).toBe(2);   // NOT a fresh id — the same underlying arrival
    src.next(info());          // id 3 → both subscribers
    expect(ids1).toEqual([1, 2, 3]);
    expect(ids2).toEqual([2, 3]);
  });

  it('does not re-mint an id when a subscriber unsubscribes and resubscribes', () => {
    const src = new BehaviorSubject<ISystemInfo>(info());
    const svc = make(src);
    const first: TelemetryArrival[] = [];
    const sub = svc.arrivals$.subscribe(a => first.push(a)); // id 1 (BehaviorSubject initial)
    src.next(info({ hashRate: 1300 })); // id 2
    sub.unsubscribe();
    const again: TelemetryArrival[] = [];
    svc.arrivals$.subscribe(a => again.push(a)); // resubscribe → replay of id 2 (NOT a new id)
    expect(again[0].arrivalId).toBe(2);
    src.next(info({ hashRate: 1305 })); // id 3
    expect(again.map(a => a.arrivalId)).toEqual([2, 3]);
  });
});
