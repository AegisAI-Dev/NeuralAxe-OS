/**
 * NeuralAxe Timed Pool Session — PURE request-identifier allocation (Gate B9).
 *
 * WHAT A REQUEST ID IS NOT
 * ------------------------
 * `client_request_id` is a best-effort correlation value. It is chosen by the
 * client, travels unauthenticated over a LAN that has no authentication at
 * all, is only as unique as one page's own counter, and resets to nothing the
 * moment the page reloads. Another client on the same network can pick the
 * same number. It is therefore NEVER authorization, ownership, proof of
 * execution, or any kind of identity — and no wording built on it may claim
 * that "this page's request" was the one the device carried out.
 *
 * What it CAN do is stop one specific confusion: a result the device retained
 * for some earlier command being read as the result of the command just sent.
 * That is worth having, and it is all this module provides.
 *
 * WHY NON-WRAPPING
 * ----------------
 * A wrapping counter re-issues an id that the device may still be reporting
 * as its last processed command, which re-creates exactly the confusion the id
 * exists to prevent. So the allocator is strictly monotonic within one
 * component lifetime and FAILS CLOSED at the end of its range rather than
 * returning to 1. Exhaustion is a reload, not a silent reuse.
 *
 * NOTHING here reads a clock, the target host, the account, or any device
 * value. The id is a plain counter and carries no information whatsoever.
 */

/**
 * Largest id the committed Gate B8 parser accepts
 * (`get_uint(root, "client_request_id", 0xFFFFFFFF, ...)`); 0 is reserved by
 * the firmware to mean "no id supplied".
 */
export const MAX_CLIENT_REQUEST_ID = 0xffffffff;

export interface RequestIdAllocator {
  /** The next id, or `null` once the range is exhausted. Never wraps. */
  next(): number | null;
  /** True once `next()` has returned the final id. */
  readonly exhausted: boolean;
  /** The most recently issued id, or 0 before the first allocation. */
  readonly lastIssued: number;
  /** How many ids this allocator has issued. */
  readonly issued: number;
}

/**
 * A strictly monotonic 1..`max` allocator that fails closed at the end.
 *
 * `max` is injectable purely so exhaustion is testable in finite time; the
 * page always constructs it with `MAX_CLIENT_REQUEST_ID`.
 */
export function createRequestIdAllocator(max: number = MAX_CLIENT_REQUEST_ID): RequestIdAllocator {
  const ceiling = Math.max(0, Math.floor(max));
  let issuedCount = 0;
  let last = 0;

  return {
    next(): number | null {
      if (last >= ceiling) {
        return null;   // fail closed — never back to 1
      }
      last += 1;
      issuedCount += 1;
      return last;
    },
    get exhausted(): boolean {
      return last >= ceiling;
    },
    get lastIssued(): number {
      return last;
    },
    get issued(): number {
      return issuedCount;
    },
  };
}
