#ifndef POOL_SESSION_API_PARSER_H_
#define POOL_SESSION_API_PARSER_H_

#include "pool_session_api_types.h"

/*
 * NeuralAxe timed pool sessions — PURE request parsing and validation
 * (Phase 2M.1B, Gate B8).
 *
 * No ESP-IDF, no esp_http_server, no NVS, no FreeRTOS, no runtime mutation,
 * no heap of its own, no globals. Deterministic: the same bytes always give
 * the same verdict and the same bounded output. cJSON is used only as the
 * repository's committed JSON reader; nothing here touches a socket, a
 * store, a lease or a pool.
 *
 * FAIL-CLOSED CONTRACT:
 *  - unknown fields are REJECTED (no silent ignore);
 *  - duplicate keys are REJECTED;
 *  - a null or wrong-typed required field is REJECTED;
 *  - numeric strings are NEVER accepted for numeric fields;
 *  - a password/pass/token/secret-like field is REJECTED before any command
 *    can be built, and no such value is ever copied into the output;
 *  - any client-supplied SOURCE identity, session id, lease token, lease
 *    generation, restore_required or internal state field is REJECTED;
 *  - a custom-certificate TLS mode is REJECTED;
 *  - *out is fully ZEROED on every failure path, so a rejected request can
 *    never leak a partially parsed value into a command.
 */

/*
 * Parse and validate a session-create body.
 *
 * `body` need not be NUL-terminated within `len`; the parser copies into a
 * bounded stack buffer of POOL_API_MAX_BODY_BYTES + 1 and rejects anything
 * larger with API_ERR_BODY_TOO_LARGE (it never reads past `len`).
 *
 * Returns API_VALID_OK only when every rule passes. `out` is fully written
 * on success and fully zeroed on every failure.
 */
PoolApiValidation pool_api_parse_create(const char *body, size_t len,
                                        PoolApiCreateRequest *out);

/*
 * Parse and validate an ACTION body (Restore Now / acknowledgement). The
 * route convention is an empty body or `{}`; an optional bounded
 * `client_request_id` is accepted for diagnostics. Anything else fails
 * closed. `out_request_id` receives 0 when absent.
 */
PoolApiValidation pool_api_parse_action(const char *body, size_t len,
                                        uint32_t *out_request_id);

/*
 * True when a JSON object key is a password-like name that must never be
 * accepted on any B8 route (case-insensitive substring match over the
 * committed deny list: password, passwd, pass, pwd, secret, token, key,
 * credential, auth). Exposed for tests and for the action parser.
 */
bool pool_api_key_is_password_like(const char *key);

/* True when a key names a SOURCE identity, a session id, a lease token or
 * any other internal-state field a client may never supply. */
bool pool_api_key_is_source_like(const char *key);
bool pool_api_key_is_session_id_like(const char *key);
bool pool_api_key_is_internal_state_like(const char *key);

/*
 * Bounded protocol / TLS / chain token decoders. Total and fail-closed:
 * an unknown token is never accepted and never normalized.
 */
bool pool_api_protocol_from_token(const char *token, PoolSessionProtocol *out);
bool pool_api_tls_mode_from_token(const char *token, uint8_t *out_mode,
                                  bool *out_is_custom);
bool pool_api_chain_from_token(const char *token, PoolChainType *out);

/* The canonical tokens the API emits (never a free-form string). */
const char *pool_api_protocol_token(PoolSessionProtocol p);
const char *pool_api_tls_mode_token(uint8_t mode);
const char *pool_api_chain_token(PoolChainType c);

#endif /* POOL_SESSION_API_PARSER_H_ */
