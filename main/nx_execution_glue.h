#ifndef NX_EXECUTION_GLUE_H_
#define NX_EXECUTION_GLUE_H_

#include <stdbool.h>

/*
 * NeuralAxe timed pool sessions — Gate B7 production glue for main()
 * (Phase 2M.1B). Board 601 / BM1370 only.
 *
 * Two functions, no types: main() only wires the executor and reports the
 * mining runtime readiness. Everything else (adapters, executor singleton,
 * runtime hook) stays inside nx_execution_glue.c.
 *
 * Both are compiled unconditionally but do REAL work only under
 * CONFIG_NX_TIMED_SESSIONS_EXECUTION; with the flag off they are no-ops,
 * no executor storage exists, no adapter exists, no hook is registered and
 * the Gate B6 hold-only behavior is byte-for-byte unchanged.
 */

/*
 * Bind the Gate B7 executor to the booted B6 runtime, install the
 * production configuration/protocol adapters and register the runtime
 * hook. MUST be called after nx_timed_sessions_boot_init() and before the
 * protocol-start barrier. Returns true when the executor is bound (always
 * false with the flag off or when the B6 runtime did not boot).
 *
 * `gs` is the application GlobalState (opaque here: the type is an
 * anonymous-struct typedef, and this header stays deliberately thin).
 */
bool nx_pool_execution_boot_init(void *gs);

/*
 * The mining runtime (NVS, Wi-Fi association, ASIC init, job pipeline and
 * the protocol coordinator init) is available: the executor may begin
 * controlled actions. Safe no-op with the flag off or before boot init.
 */
void nx_pool_execution_notify_system_ready(void);

/*
 * Gate B8: bind the timed-session API command processor to the booted B6
 * runtime, reusing the SAME audited configuration adapter this file already
 * owns (a second, unaudited source-capture path would be a real safety
 * regression). MUST be called after nx_pool_execution_boot_init() and
 * before the protocol-start barrier.
 *
 * Returns true when the processor is bound. It is compiled unconditionally
 * but does real work ONLY under CONFIG_NX_TIMED_SESSIONS_API; with that
 * flag off no processor storage, no command queue and no runtime hook
 * exists, and Gates B6/B7 behave exactly as committed.
 */
bool nx_pool_session_api_boot_init(void);

#endif /* NX_EXECUTION_GLUE_H_ */
