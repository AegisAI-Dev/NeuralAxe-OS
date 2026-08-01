#ifndef NX_HTTP_POOL_SESSION_API_H_
#define NX_HTTP_POOL_SESSION_API_H_

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_http_server.h"
#ifdef CONFIG_NX_TIMED_SESSIONS
#include "pool_session_runtime_admission.h"
#endif

/*
 * NeuralAxe timed pool sessions — HTTP adapter for the Gate B8 control
 * plane (Phase 2M.1B). Board 601 / BM1370 only.
 *
 * THE HANDLERS ONLY VALIDATE AND SUBMIT. They never apply pool
 * configuration, never start/stop/reconnect Stratum, never grant ASIC
 * mining, never call esp_restart() and never write the Gate B3 session
 * store. Each route performs the repository's existing private-network
 * admission check, reads a strictly bounded body, parses it fail-closed and
 * hands a bounded command to the single Gate B6 runtime owner task. The
 * authoritative result becomes visible through the status route.
 *
 * All three functions are compiled unconditionally but do real work only
 * under CONFIG_NX_TIMED_SESSIONS_API. With the flag off no route is
 * registered, no command queue exists and no API singleton exists.
 */

/* Extra URI handlers the API registers (0 with the flag disabled). Used by
 * the server to size max_uri_handlers with a bounded justification. */
#ifdef CONFIG_NX_TIMED_SESSIONS_API
#define NX_POOL_SESSION_API_URI_HANDLERS 4
#else
#define NX_POOL_SESSION_API_URI_HANDLERS 0
#endif

/*
 * Register the timed-session routes. MUST be called BEFORE the broad
 * "/api" and root wildcard handlers are registered, because the ESP-IDF
 * server matches handlers in registration order.
 *
 * With the flag disabled this is a static inline no-op, so the disabled
 * build contains no out-of-line symbol at all — posture A is byte-for-byte
 * stock behaviour with ZERO Gate B8 symbols in the image.
 */
#ifdef CONFIG_NX_TIMED_SESSIONS_API
esp_err_t nx_pool_session_api_register_routes(httpd_handle_t server, void *rest_context);
#else
static inline esp_err_t nx_pool_session_api_register_routes(httpd_handle_t server,
                                                            void *rest_context)
{
    (void)server;
    (void)rest_context;
    return ESP_OK;
}
#endif

#ifdef CONFIG_NX_TIMED_SESSIONS
/*
 * THE standardized sanitized HTTP 409 body shared by the timed-session
 * routes and the existing pool PATCH / restart / firmware-OTA / web-OTA
 * ownership fences.
 *
 * Under CONFIG_NX_TIMED_SESSIONS_API it emits the committed Gate B5
 * conflict surface as JSON: a stable machine code, the retryable flag, the
 * sanitized owner CLASS and the two safe booleans — never a lease token,
 * lease generation, record generation, session id, hostname, port, account,
 * worker, wallet, password, raw record or raw NVS byte.
 *
 * WITHOUT the API flag it emits the committed Gate B7 bare-token 409 body
 * byte-for-byte, so build postures B and C are unchanged.
 *
 * `conflict` may be NULL (a fail-closed generic conflict is emitted).
 */
esp_err_t nx_pool_session_api_send_conflict(httpd_req_t *req,
                                            NxAdmissionVerdict verdict,
                                            const PoolOperationHttpConflict *conflict);
#endif /* CONFIG_NX_TIMED_SESSIONS */

#endif /* NX_HTTP_POOL_SESSION_API_H_ */
