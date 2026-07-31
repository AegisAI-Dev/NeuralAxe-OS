#ifndef WEATHER_TRANSPORT_H_
#define WEATHER_TRANSPORT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_provider.h"

/*
 * NeuralAxe Weather-Aware Tuning — strict HTTPS transport contract
 * (Gate W3).
 *
 * Tests use FAKE transports only; the single real ESP-IDF adapter
 * (weather_open_meteo_http.c) compiles into the component graph but is
 * NEVER instantiated or called by any production runtime path in W3 and
 * remains linker-discardable. No test contacts any network.
 *
 * REAL-ADAPTER CONTRACT (compile-audited; enforced constants below):
 *  - HTTPS only (HTTP_TRANSPORT_OVER_SSL); a plain-http URL is
 *    unrepresentable because the adapter builds its own URL from the
 *    compile-defined allowlisted host/path — no arbitrary URL exists;
 *  - esp_crt_bundle_attach certificate validation + default hostname
 *    verification; skip_cert_common_name_check is NEVER set; no insecure
 *    TLS option; no plaintext fallback;
 *  - GET only, no request body, Accept: application/json;
 *  - automatic redirects DISABLED (disable_auto_redirect); every 3xx maps
 *    to WEATHER_PROVIDER_ERR_REDIRECT_REJECTED;
 *  - bounded connect/read timeout (WEATHER_HTTP_TIMEOUT_MS);
 *  - the response is read into a caller-owned fixed buffer of
 *    WEATHER_RESPONSE_CAP bytes; Content-Length above the cap and chunked
 *    responses exceeding the cap are both rejected as
 *    WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE before/during read;
 *  - incomplete bodies are rejected (ERR_TRUNCATED);
 *  - cleanup runs on every path;
 *  - no full-URL logging (coordinates are treated as sensitive) and no
 *    response-body logging; TLS errors surface only as sanitized codes.
 */

/* Bounded response capacity (Stage 7). The audited minimal Open-Meteo
 * payload (one location, one daily variable, optional current block) is
 * ~600-900 bytes; 4096 gives >4x headroom inside the recommended
 * 4096-8192 range while keeping the static buffer small. */
#define WEATHER_RESPONSE_CAP 4096u

/* Bounded connect+read timeout for the real adapter (milliseconds). */
#define WEATHER_HTTP_TIMEOUT_MS 8000u

/* Total serialized request bound: scheme+host+path+query. */
#define WEATHER_REQUEST_URL_MAX 320u

/*
 * Transport-level response facts. `complete` means the body was fully
 * received; `oversized` means the source exceeded WEATHER_RESPONSE_CAP
 * (whatever bytes were kept must be discarded).
 */
typedef struct {
    int http_status;
    bool content_type_present;
    bool content_type_json;   /* "application/json" (prefix match)       */
    bool oversized;
    bool complete;
    size_t body_len;          /* <= WEATHER_RESPONSE_CAP                 */
    uint8_t body[WEATHER_RESPONSE_CAP];
} WeatherHttpResponse;

/* Injected transport: performs (or fakes) exactly one GET of the
 * allowlisted request and fills *out. Returns a transport-layer result
 * (OK means "an HTTP response was received"; HTTP semantics are judged by
 * weather_transport_validate). */
typedef struct {
    WeatherProviderResult (*fetch)(void *ctx, const WeatherRequest *req,
                                   WeatherHttpResponse *out);
} WeatherTransportOps;

/*
 * HTTP-layer validation shared by fake and real paths:
 *  - oversized            -> ERR_RESPONSE_TOO_LARGE
 *  - !complete            -> ERR_TRUNCATED
 *  - 3xx                  -> ERR_REDIRECT_REJECTED (never followed)
 *  - any other non-2xx    -> ERR_HTTP_STATUS
 *  - Content-Type present but not application/json -> ERR_CONTENT_TYPE
 *  - Content-Type MISSING -> ERR_CONTENT_TYPE (explicit conservative
 *    policy: the provider documents JSON; a missing type fails closed —
 *    weather unavailability degrades toward the SAFE hot stance)
 *  - empty body on 2xx    -> ERR_TRUNCATED
 */
WeatherProviderResult weather_transport_validate(const WeatherHttpResponse *r);

/* Compose the canonical https URL for the real adapter into a bounded
 * buffer ("https://" host path "?" query). Fails when it would exceed
 * WEATHER_REQUEST_URL_MAX. Pure. */
bool weather_request_url(const WeatherRequest *req, char *buf, size_t cap);

/* Real ESP-IDF adapter ops (compiled; NEVER wired in W3). */
const WeatherTransportOps *weather_open_meteo_http_ops(void);

#endif /* WEATHER_TRANSPORT_H_ */
