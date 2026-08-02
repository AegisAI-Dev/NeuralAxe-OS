/*
 * NeuralAxe timed pool sessions — READ-ONLY ESP-IDF NVS backend for the
 * store preflight (Phase 2M.1B, Gate B10.2).
 *
 * This is deliberately NOT the committed Gate B3 backend. That one opens the
 * namespace with NVS_READWRITE, and ESP-IDF's documented behaviour for
 * NVS_READWRITE is to CREATE the namespace when it does not exist. Using it
 * for a preflight would turn "this device has never held a timed-session
 * record" into "this device now has an nx_tps namespace", writing NVS
 * metadata on a device the owner was only trying to inspect.
 *
 * ESP-IDF v5.5.3, nvs.h, verified for this gate:
 *   NVS_READONLY  — "will open a handle for reading only. All write requests
 *                    will be rejected for this handle."
 *   ESP_ERR_NVS_NOT_FOUND — "namespace doesn't exist yet and mode is
 *                    NVS_READONLY"
 *
 * So the read-only open is a pure query, and the IDF layer itself is a
 * second line of defence behind the refusing stubs below.
 *
 * It never calls nvs_flash_init, never opens another namespace, never
 * enumerates namespaces, never reads the partition as a whole, and never
 * logs a payload, key name, namespace name, identity or NVS error string.
 */

#include <string.h>

#include "nvs.h"
#include "esp_err.h"
#include "pool_session_preflight.h"

_Static_assert(sizeof(nvs_handle_t) == sizeof(uint32_t),
               "NxTpsPreflightNvsBackend handle storage no longer matches nvs_handle_t");

void nx_tps_preflight_nvs_init(NxTpsPreflightNvsBackend *b)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
}

bool nx_tps_preflight_nvs_namespace_absent(const NxTpsPreflightNvsBackend *b)
{
    return (b == NULL) ? false : !b->namespace_present;
}

/*
 * Open the ONE namespace read-only.
 *
 * A missing namespace is NOT an error here: it is the answer. It is recorded
 * and reported as success-with-absence so the committed Gate B3 loader is
 * never even entered, and the pure classifier maps it to EMPTY.
 */
static int pf_open(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    nvs_handle_t              h;
    esp_err_t                 err;

    if (b == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    if (b->open || !b->namespace_present) {
        /* Deterministic repeated open; an already-answered absence is not
         * re-queried and certainly not created. */
        return POOL_STORE_BACKEND_OK;
    }

    err = nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* The namespace genuinely does not exist. Nothing was created, and
         * this is the ONLY error that means "absent". */
        b->namespace_present = false;
        b->open              = false;
        return POOL_STORE_BACKEND_OK;
    }
    if (err != ESP_OK) {
        /*
         * ANY other open failure (out of memory, invalid state, a flash fault
         * during namespace lookup) means the namespace could not be READ — not
         * that it is absent. Those two must never share a representation: an
         * unreadable store reported as absent would classify EMPTY and PERMIT a
         * pilot, which is the exact fail-open this component exists to prevent.
         *
         * `open_failed` is therefore a separate flag, and the classifier checks
         * it BEFORE it ever considers absence.
         */
        b->open_failed       = true;
        b->namespace_present = false;
        b->open              = false;
        return POOL_STORE_BACKEND_IO; /* no raw error text escapes */
    }
    b->nvs_handle        = (uint32_t)h;
    b->open              = true;
    b->namespace_present = true;
    return POOL_STORE_BACKEND_OK;
}

/*
 * Bounded read of ONE committed key. Mirrors the committed Gate B3 backend's
 * contract exactly (size query, then copy only when it fits) so the shared
 * loader classifies an oversized or truncated value identically.
 */
static int pf_read_blob(void *ctx, const char *key, uint8_t *buf, size_t cap,
                        size_t *out_len)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    size_t                    size = 0;
    esp_err_t                 err;

    if (b == NULL || key == NULL || out_len == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = 0;
    if (b->read_attempts < UINT32_MAX) {
        b->read_attempts++;
    }
    if (!b->open) {
        /* Absent namespace: every key is absent. Never an invented value. */
        return b->namespace_present ? POOL_STORE_BACKEND_IO
                                    : POOL_STORE_BACKEND_NOT_FOUND;
    }

    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return POOL_STORE_BACKEND_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = size;
    if (buf == NULL || size > cap) {
        /* Report the stored length without copying; the committed loader
         * classifies an oversized blob as corrupt, not as I/O failure. */
        return POOL_STORE_BACKEND_OK;
    }
    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, buf, &size);
    if (err != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = size;
    return POOL_STORE_BACKEND_OK;
}

/*
 * THE REFUSING STUBS.
 *
 * They exist only because pool_session_store_init() requires a complete ops
 * table. They are unconditional refusals that COUNT the attempt, which is
 * what makes "this preflight performed no mutation" a measured runtime fact
 * instead of a claim: the pure classifier fails closed on any non-zero
 * counter, and the tests assert the counters stay at zero on every success
 * and every error path.
 *
 * They call no nvs_set_*, no nvs_erase_* and no nvs_commit — the ESP-IDF
 * write API is not referenced anywhere in this translation unit.
 */
static int pf_write_blob(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;

    (void)key;
    (void)buf;
    (void)len;
    if (b != NULL && b->write_attempts < UINT32_MAX) {
        b->write_attempts++;
    }
    return POOL_STORE_BACKEND_IO; /* never, on any path */
}

static int pf_commit(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;

    if (b != NULL && b->commit_attempts < UINT32_MAX) {
        b->commit_attempts++;
    }
    return POOL_STORE_BACKEND_IO; /* never, on any path */
}

static int pf_close(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;

    if (b == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    if (b->open) {
        nvs_close((nvs_handle_t)b->nvs_handle);
        b->open       = false;
        b->nvs_handle = 0;
    }
    return POOL_STORE_BACKEND_OK; /* idempotent; closed on every path */
}

static const PoolStoreBackendOps s_preflight_ops = {
    .open       = pf_open,
    .read_blob  = pf_read_blob,
    .write_blob = pf_write_blob,
    .commit     = pf_commit,
    .close      = pf_close,
};

const PoolStoreBackendOps *nx_tps_preflight_nvs_ops(void)
{
    return &s_preflight_ops;
}
