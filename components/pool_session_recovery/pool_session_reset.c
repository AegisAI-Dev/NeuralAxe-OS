/*
 * NeuralAxe timed pool sessions — reset classification (Gate B4).
 * PURE: no ESP-IDF includes, no esp_reset_reason() call, no I/O, no state.
 */

#include "pool_session_reset.h"

PoolSessionResetClass pool_session_reset_classify_raw(int32_t raw_reason)
{
    switch (raw_reason) {
    case POOL_RESET_RAW_POWERON:    return POOL_RESET_CLASS_POWER_ON;
    case POOL_RESET_RAW_SW:         return POOL_RESET_CLASS_SOFTWARE;
    case POOL_RESET_RAW_PANIC:      return POOL_RESET_CLASS_PANIC;
    case POOL_RESET_RAW_CPU_LOCKUP: return POOL_RESET_CLASS_PANIC;
    case POOL_RESET_RAW_TASK_WDT:   return POOL_RESET_CLASS_TASK_WATCHDOG;
    case POOL_RESET_RAW_INT_WDT:    return POOL_RESET_CLASS_INTERRUPT_WATCHDOG;
    case POOL_RESET_RAW_WDT:        return POOL_RESET_CLASS_OTHER_WATCHDOG;
    case POOL_RESET_RAW_BROWNOUT:   return POOL_RESET_CLASS_BROWNOUT;
    case POOL_RESET_RAW_PWR_GLITCH: return POOL_RESET_CLASS_BROWNOUT;
    case POOL_RESET_RAW_DEEPSLEEP:  return POOL_RESET_CLASS_DEEP_SLEEP;
    case POOL_RESET_RAW_EXT:        return POOL_RESET_CLASS_EXTERNAL;
    case POOL_RESET_RAW_SDIO:       return POOL_RESET_CLASS_EXTERNAL;
    case POOL_RESET_RAW_USB:        return POOL_RESET_CLASS_EXTERNAL;
    case POOL_RESET_RAW_JTAG:       return POOL_RESET_CLASS_EXTERNAL;
    case POOL_RESET_RAW_UNKNOWN:    return POOL_RESET_CLASS_UNKNOWN;
    case POOL_RESET_RAW_EFUSE:      return POOL_RESET_CLASS_UNKNOWN;
    default:                        return POOL_RESET_CLASS_UNKNOWN;
    }
}

/* Treat any invalid class value as UNKNOWN (conservative, total). */
static PoolSessionResetClass norm(PoolSessionResetClass c)
{
    return ((unsigned)c < (unsigned)POOL_RESET_CLASS__COUNT) ? c
                                                             : POOL_RESET_CLASS_UNKNOWN;
}

bool pool_reset_class_is_expected_restart(PoolSessionResetClass c)
{
    switch (norm(c)) {
    case POOL_RESET_CLASS_POWER_ON:
    case POOL_RESET_CLASS_SOFTWARE:
        return true;
    default:
        return false;
    }
}

bool pool_reset_class_is_abnormal_restart(PoolSessionResetClass c)
{
    switch (norm(c)) {
    case POOL_RESET_CLASS_PANIC:
    case POOL_RESET_CLASS_TASK_WATCHDOG:
    case POOL_RESET_CLASS_INTERRUPT_WATCHDOG:
    case POOL_RESET_CLASS_OTHER_WATCHDOG:
    case POOL_RESET_CLASS_BROWNOUT:
    case POOL_RESET_CLASS_UNKNOWN:
        return true;
    default:
        return false;
    }
}

bool pool_reset_class_increments_reboot_count(PoolSessionResetClass c)
{
    (void)norm(c);
    return true; /* every boot with a session record consumes reboot budget */
}

bool pool_reset_class_increments_recovery_attempt(PoolSessionResetClass c)
{
    /* Helper view: crash-class boots consume recovery budget. The engine
     * additionally increments the attempt counter when the emitted plan is
     * itself a recovery action (restore/verify) — see the engine contract. */
    return pool_reset_class_is_abnormal_restart(c);
}

bool pool_reset_class_increments_consecutive_failures(PoolSessionResetClass c)
{
    return pool_reset_class_is_abnormal_restart(c);
}

bool pool_reset_class_may_resume_target(PoolSessionResetClass c)
{
    switch (norm(c)) {
    case POOL_RESET_CLASS_POWER_ON:
    case POOL_RESET_CLASS_SOFTWARE:
        return true;
    /* Crash-class resets may resume ONLY within the bounded consecutive-
     * failure budget (the engine enforces the budget; repeated crashes
     * exhaust it and force operator recovery — never a reboot loop). */
    case POOL_RESET_CLASS_PANIC:
    case POOL_RESET_CLASS_TASK_WATCHDOG:
    case POOL_RESET_CLASS_INTERRUPT_WATCHDOG:
    case POOL_RESET_CLASS_OTHER_WATCHDOG:
    case POOL_RESET_CLASS_BROWNOUT:
        return true;
    /* Deep-sleep wake, external/tool resets and unclassifiable resets never
     * resume the temporary target: conservative restore instead. */
    case POOL_RESET_CLASS_DEEP_SLEEP:
    case POOL_RESET_CLASS_EXTERNAL:
    case POOL_RESET_CLASS_UNKNOWN:
    default:
        return false;
    }
}

bool pool_reset_class_requires_conservative_restore(PoolSessionResetClass c)
{
    switch (norm(c)) {
    case POOL_RESET_CLASS_DEEP_SLEEP:
    case POOL_RESET_CLASS_EXTERNAL:
    case POOL_RESET_CLASS_UNKNOWN:
        return true;
    default:
        return false;
    }
}

uint8_t pool_reset_class_to_record_class(PoolSessionResetClass c)
{
    switch (norm(c)) {
    case POOL_RESET_CLASS_POWER_ON:
    case POOL_RESET_CLASS_SOFTWARE:
    case POOL_RESET_CLASS_DEEP_SLEEP:
        return 1u; /* POOL_RECORD_RESET_CLASS_CLEAN */
    case POOL_RESET_CLASS_PANIC:
    case POOL_RESET_CLASS_TASK_WATCHDOG:
    case POOL_RESET_CLASS_INTERRUPT_WATCHDOG:
    case POOL_RESET_CLASS_OTHER_WATCHDOG:
    case POOL_RESET_CLASS_BROWNOUT:
        return 2u; /* POOL_RECORD_RESET_CLASS_CRASH */
    default:
        return 3u; /* POOL_RECORD_RESET_CLASS_UNKNOWN */
    }
}

const char *pool_reset_class_str(PoolSessionResetClass c)
{
    switch (c) {
    case POOL_RESET_CLASS_POWER_ON:           return "RESET_POWER_ON";
    case POOL_RESET_CLASS_SOFTWARE:           return "RESET_SOFTWARE";
    case POOL_RESET_CLASS_PANIC:              return "RESET_PANIC";
    case POOL_RESET_CLASS_TASK_WATCHDOG:      return "RESET_TASK_WATCHDOG";
    case POOL_RESET_CLASS_INTERRUPT_WATCHDOG: return "RESET_INTERRUPT_WATCHDOG";
    case POOL_RESET_CLASS_OTHER_WATCHDOG:     return "RESET_OTHER_WATCHDOG";
    case POOL_RESET_CLASS_BROWNOUT:           return "RESET_BROWNOUT";
    case POOL_RESET_CLASS_DEEP_SLEEP:         return "RESET_DEEP_SLEEP";
    case POOL_RESET_CLASS_EXTERNAL:           return "RESET_EXTERNAL";
    case POOL_RESET_CLASS_UNKNOWN:            return "RESET_UNKNOWN";
    default:                                  return "RESET_CLASS_INVALID";
    }
}
