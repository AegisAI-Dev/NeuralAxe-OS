/*
 * NeuralAxe Weather-Aware Tuning — real ESP-IDF HTTPS transport adapter
 * (Gate W3).
 *
 * COMPILE-ONLY IN W3: this translation unit compiles into the component
 * graph but is NEVER instantiated or called by any production runtime
 * path, and no test performs a network operation. It exists so the exact
 * ESP-IDF v5.5.3 contract is compiler-verified now and wired only by a
 * later gate (W4) under the B5 operation-ownership rules.
 *
 * SECURITY CONTRACT (enforced below, audited in tests via the pure layer):
 *  - HTTPS only: transport_type = HTTP_TRANSPORT_OVER_SSL and the URL is
 *    composed from the compile-defined allowlisted host/path — no
 *    arbitrary URL or plain-http scheme is representable;
 *  - esp_crt_bundle_attach + DEFAULT hostname verification;
 *    skip_cert_common_name_check is never set; no insecure option;
 *  - GET only, no request body, Accept: application/json;
 *  - automatic redirects DISABLED; any 3xx surfaces to the shared
 *    weather_transport_validate which rejects it;
 *  - bounded timeout; bounded read into the caller's fixed buffer with a
 *    hard WEATHER_RESPONSE_CAP (Content-Length overflow rejected before
 *    reading; chunked overflow rejected during reading);
 *  - cleanup on every path; no full-URL logging (coordinates are treated
 *    as sensitive), no response-body logging, no raw TLS error exposure —
 *    failures map to sanitized machine codes only. Connect-phase
 *    failures (DNS/TCP/TLS are not separable through this API surface
 *    without deeper introspection) map to ERR_CONNECT_TIMEOUT; finer
 *    TLS/DNS discrimination is a documented W4/W6 measurement task.
 */

#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"

typedef struct {
    bool content_type_present;
    bool content_type_json;
} OmHeaderFacts;

static esp_err_t om_http_event(esp_http_client_event_t *evt)
{
    OmHeaderFacts *facts = (OmHeaderFacts *)evt->user_data;
    if (facts == NULL) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        evt->header_value != NULL) {
        /* Bounded, case-insensitive header-name compare; prefix match on
         * the media type (parameters such as charset are permitted). */
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            facts->content_type_present = true;
            facts->content_type_json =
                (strncasecmp(evt->header_value, "application/json", 16) == 0);
        }
    }
    return ESP_OK;
}

static WeatherProviderResult om_http_fetch(void *ctx, const WeatherRequest *req,
                                           WeatherHttpResponse *out)
{
    char url[WEATHER_REQUEST_URL_MAX];
    OmHeaderFacts facts;
    esp_http_client_handle_t client = NULL;
    WeatherProviderResult res = WEATHER_PROVIDER_ERR_INTERNAL;
    (void)ctx;

    if (req == NULL || out == NULL) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    memset(out, 0, sizeof(*out));
    memset(&facts, 0, sizeof(facts));

    /* Allowlist enforcement: the adapter serves exactly one host+path. */
    if (req->host == NULL || req->path == NULL ||
        strcmp(req->host, WEATHER_OPEN_METEO_HOST) != 0 ||
        strcmp(req->path, WEATHER_OPEN_METEO_PATH) != 0) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    if (!weather_request_url(req, url, sizeof(url))) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }

    {
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .transport_type = HTTP_TRANSPORT_OVER_SSL, /* HTTPS only     */
            .crt_bundle_attach = esp_crt_bundle_attach, /* cert + hostname
                verification with the default (strict) name check        */
            .timeout_ms = (int)WEATHER_HTTP_TIMEOUT_MS,
            .disable_auto_redirect = true, /* 3xx is rejected, never followed */
            .max_redirection_count = 0,
            .event_handler = om_http_event,
            .user_data = &facts,
            .user_agent = "NeuralAxe-OS-Weather", /* generic product UA —
                no device identity of any kind                           */
            .buffer_size = 1024,
            .buffer_size_tx = 512,
        };
        client = esp_http_client_init(&cfg);
    }
    if (client == NULL) {
        return WEATHER_PROVIDER_ERR_INTERNAL;
    }

    do {
        int status;
        int64_t content_len;
        int total = 0;

        if (esp_http_client_set_header(client, "Accept", "application/json") !=
            ESP_OK) {
            res = WEATHER_PROVIDER_ERR_INTERNAL;
            break;
        }
        /* GET with no request body. */
        if (esp_http_client_open(client, 0) != ESP_OK) {
            res = WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT; /* aggregate
                connect-phase failure (DNS/TCP/TLS); sanitized            */
            break;
        }
        content_len = esp_http_client_fetch_headers(client);
        if (content_len < 0) {
            res = WEATHER_PROVIDER_ERR_READ_TIMEOUT;
            break;
        }
        if ((uint64_t)content_len > (uint64_t)WEATHER_RESPONSE_CAP) {
            out->oversized = true;
            res = WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE;
            break;
        }
        status = esp_http_client_get_status_code(client);
        out->http_status = status;
        out->content_type_present = facts.content_type_present;
        out->content_type_json = facts.content_type_json;

        /* Bounded read (identical hard cap for chunked and sized). */
        while (total < (int)WEATHER_RESPONSE_CAP) {
            int r = esp_http_client_read(client, (char *)out->body + total,
                                         (int)WEATHER_RESPONSE_CAP - total);
            if (r < 0) {
                res = WEATHER_PROVIDER_ERR_READ_TIMEOUT;
                goto done;
            }
            if (r == 0) {
                break;
            }
            total += r;
        }
        if (total >= (int)WEATHER_RESPONSE_CAP) {
            /* Buffer full: probe one more byte; any extra data means the
             * source exceeded the cap. */
            char probe;
            int r = esp_http_client_read(client, &probe, 1);
            if (r != 0) {
                out->oversized = true;
                res = WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE;
                break;
            }
        }
        out->body_len = (size_t)total;
        out->complete = esp_http_client_is_complete_data_received(client);
        res = WEATHER_PROVIDER_OK; /* HTTP semantics judged by
                                      weather_transport_validate           */
    } while (0);

done:
    /* Cleanup on every path; errors surface only as machine codes. */
    (void)esp_http_client_close(client);
    (void)esp_http_client_cleanup(client);
    return res;
}

static const WeatherTransportOps s_om_http_ops = {
    .fetch = om_http_fetch,
};

const WeatherTransportOps *weather_open_meteo_http_ops(void)
{
    return &s_om_http_ops;
}
