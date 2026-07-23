/*
 * NeuralAxe timed pool sessions — ESP-IDF reset-reason compile validation
 * (Gate B4).
 *
 * This translation unit exists to PROVE, at compile time against the real
 * ESP-IDF v5.5.3 headers, that the raw values pinned in
 * pool_session_reset.h match esp_reset_reason_t exactly — so the pure
 * classifier can never silently drift from the platform enum.
 *
 * It calls NOTHING at runtime: esp_reset_reason() is never invoked in Gate
 * B4, and the thin wrapper below is compiled but has no callers (the future
 * Gate B5+ boot integrator reads the reason once and classifies it).
 */

#include "esp_system.h"
#include "pool_session_reset.h"

_Static_assert((int)ESP_RST_UNKNOWN    == POOL_RESET_RAW_UNKNOWN,    "raw pin drift: UNKNOWN");
_Static_assert((int)ESP_RST_POWERON    == POOL_RESET_RAW_POWERON,    "raw pin drift: POWERON");
_Static_assert((int)ESP_RST_EXT        == POOL_RESET_RAW_EXT,        "raw pin drift: EXT");
_Static_assert((int)ESP_RST_SW         == POOL_RESET_RAW_SW,         "raw pin drift: SW");
_Static_assert((int)ESP_RST_PANIC      == POOL_RESET_RAW_PANIC,      "raw pin drift: PANIC");
_Static_assert((int)ESP_RST_INT_WDT    == POOL_RESET_RAW_INT_WDT,    "raw pin drift: INT_WDT");
_Static_assert((int)ESP_RST_TASK_WDT   == POOL_RESET_RAW_TASK_WDT,   "raw pin drift: TASK_WDT");
_Static_assert((int)ESP_RST_WDT        == POOL_RESET_RAW_WDT,        "raw pin drift: WDT");
_Static_assert((int)ESP_RST_DEEPSLEEP  == POOL_RESET_RAW_DEEPSLEEP,  "raw pin drift: DEEPSLEEP");
_Static_assert((int)ESP_RST_BROWNOUT   == POOL_RESET_RAW_BROWNOUT,   "raw pin drift: BROWNOUT");
_Static_assert((int)ESP_RST_SDIO       == POOL_RESET_RAW_SDIO,       "raw pin drift: SDIO");
_Static_assert((int)ESP_RST_USB        == POOL_RESET_RAW_USB,        "raw pin drift: USB");
_Static_assert((int)ESP_RST_JTAG       == POOL_RESET_RAW_JTAG,       "raw pin drift: JTAG");
_Static_assert((int)ESP_RST_EFUSE      == POOL_RESET_RAW_EFUSE,      "raw pin drift: EFUSE");
_Static_assert((int)ESP_RST_PWR_GLITCH == POOL_RESET_RAW_PWR_GLITCH, "raw pin drift: PWR_GLITCH");
_Static_assert((int)ESP_RST_CPU_LOCKUP == POOL_RESET_RAW_CPU_LOCKUP, "raw pin drift: CPU_LOCKUP");

/*
 * Compile-only convenience wrapper for the future Gate B5+ integrator.
 * NOT called anywhere in Gate B4; it never reads the reset reason itself.
 */
PoolSessionResetClass pool_session_reset_classify_esp(esp_reset_reason_t reason);
PoolSessionResetClass pool_session_reset_classify_esp(esp_reset_reason_t reason)
{
    return pool_session_reset_classify_raw((int32_t)reason);
}
