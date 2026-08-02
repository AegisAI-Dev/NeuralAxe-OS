/**
 * Gate B9 — request-identifier allocation.
 *
 * The allocator exists to stop one confusion: a result the device retained for
 * an EARLIER command being read as the answer to a new one. A wrapping counter
 * re-creates that confusion by construction, so the only acceptable behaviour
 * at the end of the range is to fail closed.
 */

import { MAX_CLIENT_REQUEST_ID, createRequestIdAllocator } from './timed-session-request-id';

describe('request ids: monotonic allocation', () => {
  it('starts at 1 and never repeats a number', () => {
    const alloc = createRequestIdAllocator(5);
    const issued = [alloc.next(), alloc.next(), alloc.next()];
    expect(issued).toEqual([1, 2, 3]);
    expect(new Set(issued).size).toBe(3);
  });

  it('is strictly increasing', () => {
    const alloc = createRequestIdAllocator(50);
    let prev = 0;
    for (let i = 0; i < 50; i++) {
      const id = alloc.next()!;
      expect(id).withContext(`step ${i}`).toBeGreaterThan(prev);
      prev = id;
    }
  });

  it('tracks what it has issued', () => {
    const alloc = createRequestIdAllocator(4);
    expect(alloc.lastIssued).toBe(0);
    expect(alloc.issued).toBe(0);
    alloc.next();
    alloc.next();
    expect(alloc.lastIssued).toBe(2);
    expect(alloc.issued).toBe(2);
  });
});

describe('request ids: exhaustion fails closed', () => {
  it('yields the final id exactly once', () => {
    const alloc = createRequestIdAllocator(3);
    expect(alloc.next()).toBe(1);
    expect(alloc.next()).toBe(2);
    expect(alloc.next()).withContext('the final id').toBe(3);
    expect(alloc.exhausted).toBeTrue();
  });

  it('refuses afterwards instead of returning to 1', () => {
    const alloc = createRequestIdAllocator(3);
    alloc.next(); alloc.next(); alloc.next();
    expect(alloc.next()).withContext('fails closed').toBeNull();
    expect(alloc.next()).toBeNull();
    expect(alloc.next()).toBeNull();
    expect(alloc.lastIssued).withContext('never rewinds').toBe(3);
    expect(alloc.issued).withContext('no further ids were handed out').toBe(3);
  });

  it('a degenerate range issues nothing at all rather than issuing 1', () => {
    const alloc = createRequestIdAllocator(0);
    expect(alloc.exhausted).toBeTrue();
    expect(alloc.next()).toBeNull();
    expect(alloc.lastIssued).toBe(0);
  });

  it('uses the committed backend maximum by default', () => {
    // The Gate B8 parser accepts client_request_id up to 0xFFFFFFFF and
    // reserves 0 for "no id supplied".
    expect(MAX_CLIENT_REQUEST_ID).toBe(0xffffffff);
    const alloc = createRequestIdAllocator();
    expect(alloc.next()).toBe(1);
    expect(alloc.exhausted).toBeFalse();
  });

  it('carries no information: two fresh allocators are indistinguishable', () => {
    // Nothing about the operator, the target or the clock may leak into an id.
    const a = createRequestIdAllocator(3);
    const b = createRequestIdAllocator(3);
    expect([a.next(), a.next()]).toEqual([b.next(), b.next()]);
  });
});
