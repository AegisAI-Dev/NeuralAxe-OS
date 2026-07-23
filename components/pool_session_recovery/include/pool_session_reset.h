#ifndef POOL_SESSION_RESET_H_
#define POOL_SESSION_RESET_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * NeuralAxe timed pool sessions — reset classification (Phase 2M.1B, Gate B4).
 *
 * A NeuralAxe-owned, bounded reset-class model. This header is PURE: no
 * ESP-IDF includes. The raw numeric values of esp_reset_reason_t (ESP-IDF
 * v5.5.3) are pinned below and compile-asserted against the real enum in
 * pool_session_reset_espidf.c — B4 itself NEVER calls esp_reset_reason();
 * the future boot integrator (Gate B5+) reads the reason once and feeds the
 * raw value through pool_session_reset_classify_raw().
 *
 * POLICY (Phase 2M.1A §13 reboot-loop guard; B4 Stage 3/10):
 *  - A reset class alone NEVER authorizes target continuation — the
 *    persisted state, the B2 time decision and the counter budget always
 *    decide together.
 *  - Expected restarts: POWER_ON, SOFTWARE.
 *  - Abnormal (crash-class, consumes the consecutive-failure budget):
 *    PANIC, TASK/INTERRUPT/OTHER WATCHDOG, BROWNOUT, UNKNOWN.
 *  - Conservative-restore classes (no target resume at all): DEEP_SLEEP
 *    (the product never deep-sleeps during a session), EXTERNAL (pin/SDIO/
 *    USB/JTAG — unclassifiable operator/tool intervention), UNKNOWN.
 *  - Crash-class resets may still lead to a TARGET_ACTIVE resume WITHIN the
 *    bounded budget; repeated crashes exhaust it and force operator
 *    recovery — never a reboot loop.
 */

/* Pinned raw esp_reset_reason_t values (ESP-IDF v5.5.3). Compile-asserted
 * against the real enum in pool_session_reset_espidf.c. */
#define POOL_RESET_RAW_UNKNOWN     0
#define POOL_RESET_RAW_POWERON     1
#define POOL_RESET_RAW_EXT         2
#define POOL_RESET_RAW_SW          3
#define POOL_RESET_RAW_PANIC      4
#define POOL_RESET_RAW_INT_WDT     5
#define POOL_RESET_RAW_TASK_WDT    6
#define POOL_RESET_RAW_WDT         7
#define POOL_RESET_RAW_DEEPSLEEP   8
#define POOL_RESET_RAW_BROWNOUT    9
#define POOL_RESET_RAW_SDIO        10
#define POOL_RESET_RAW_USB         11
#define POOL_RESET_RAW_JTAG        12
#define POOL_RESET_RAW_EFUSE       13
#define POOL_RESET_RAW_PWR_GLITCH  14
#define POOL_RESET_RAW_CPU_LOCKUP  15

/* NeuralAxe reset classes (stable; wire-independent). */
typedef enum {
    POOL_RESET_CLASS_POWER_ON = 0,
    POOL_RESET_CLASS_SOFTWARE,
    POOL_RESET_CLASS_PANIC,
    POOL_RESET_CLASS_TASK_WATCHDOG,
    POOL_RESET_CLASS_INTERRUPT_WATCHDOG,
    POOL_RESET_CLASS_OTHER_WATCHDOG,
    POOL_RESET_CLASS_BROWNOUT,
    POOL_RESET_CLASS_DEEP_SLEEP,
    POOL_RESET_CLASS_EXTERNAL,
    POOL_RESET_CLASS_UNKNOWN,
    POOL_RESET_CLASS__COUNT
} PoolSessionResetClass;

/*
 * Map a raw esp_reset_reason_t numeric value to a NeuralAxe class.
 * Total: every in-range value maps explicitly; any out-of-range or future
 * value maps to POOL_RESET_CLASS_UNKNOWN (conservative).
 *   POWERON->POWER_ON · SW->SOFTWARE · PANIC,CPU_LOCKUP->PANIC ·
 *   TASK_WDT->TASK_WATCHDOG · INT_WDT->INTERRUPT_WATCHDOG ·
 *   WDT->OTHER_WATCHDOG · BROWNOUT,PWR_GLITCH->BROWNOUT ·
 *   DEEPSLEEP->DEEP_SLEEP · EXT,SDIO,USB,JTAG->EXTERNAL ·
 *   UNKNOWN,EFUSE,other->UNKNOWN
 */
PoolSessionResetClass pool_session_reset_classify_raw(int32_t raw_reason);

/* Pure, total classification helpers (invalid classes behave as UNKNOWN). */
bool pool_reset_class_is_expected_restart(PoolSessionResetClass c);
bool pool_reset_class_is_abnormal_restart(PoolSessionResetClass c);
bool pool_reset_class_increments_reboot_count(PoolSessionResetClass c);
bool pool_reset_class_increments_recovery_attempt(PoolSessionResetClass c);
bool pool_reset_class_increments_consecutive_failures(PoolSessionResetClass c);
bool pool_reset_class_may_resume_target(PoolSessionResetClass c);
bool pool_reset_class_requires_conservative_restore(PoolSessionResetClass c);

/* Map to the B3 persisted PoolRecordResetClass value space
 * (0 NONE / 1 CLEAN / 2 CRASH / 3 UNKNOWN). */
uint8_t pool_reset_class_to_record_class(PoolSessionResetClass c);

/* Stable machine token (dot-free; never raw ESP-IDF reset text). */
const char *pool_reset_class_str(PoolSessionResetClass c);

_Static_assert(POOL_RESET_CLASS__COUNT == 10,
               "reset class count changed — review helpers/tests");

#endif /* POOL_SESSION_RESET_H_ */
