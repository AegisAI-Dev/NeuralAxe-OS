#ifndef POOL_SESSION_RUNTIME_BOOT_H_
#define POOL_SESSION_RUNTIME_BOOT_H_

#include <stdbool.h>

/*
 * NeuralAxe timed pool sessions — MINIMAL boot surface for main()
 * (Phase 2M.1B, Gate B6).
 *
 * Deliberately three functions and NO types: main() must understand only
 * "did the bootstrap succeed" and "may protocol startup proceed". Every
 * runtime type, task, store, coordinator and time detail stays inside
 * components/pool_session_runtime.
 *
 * All three are compiled unconditionally but are called from main() ONLY
 * under CONFIG_NX_TIMED_SESSIONS. With the feature disabled they are
 * no-ops that touch nothing: no instance is created, no task exists, the
 * "nx_tps" namespace is never opened, SNTP is never initialized, and
 * protocol startup is byte-for-byte unchanged.
 */

/* The compile-time value of CONFIG_NX_TIMED_SESSIONS. */
bool nx_timed_sessions_enabled(void);

/*
 * Steps 2-14 of the Gate B6 boot order, synchronously.
 *
 * PRECONDITION: the existing NVS subsystem is already initialized.
 * MUST be called BEFORE the protocol-start barrier.
 *
 * Returns true when the bootstrap ran to completion. That is NOT a
 * permission: call nx_timed_sessions_protocol_start_allowed() for the
 * barrier answer. With the feature disabled this returns false and does
 * nothing at all.
 */
bool nx_timed_sessions_boot_init(void);

/*
 * THE protocol-start barrier. Returns true only for a proven-safe posture
 * (empty store, cleared store, or a safe retained terminal awaiting
 * acknowledgement). Every unresolved, uncertain, corrupt, unsupported,
 * ambiguous, active, restore-owing or recovery posture returns false, and
 * so does any initialization failure. Fail-closed for a NULL/unbooted
 * runtime. With the feature disabled it returns true (unchanged startup).
 *
 * Gate B6 never turns a false back into a true: releasing a hold after live
 * verification or restoration is Gate B7's job.
 */
bool nx_timed_sessions_protocol_start_allowed(void);

/*
 * The single bounded network-ready notification. Safe to call when the
 * feature is disabled (no-op) and before the runtime task exists (latched).
 * It performs no networking itself and never mutates pool state.
 */
void nx_timed_sessions_notify_network_ready(void);

#endif /* POOL_SESSION_RUNTIME_BOOT_H_ */
