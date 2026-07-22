/*
 * NeuralAxe timed pool sessions — real ESP-IDF NVS backend (Gate B3).
 *
 * Opens ONLY the dedicated "nx_tps" namespace with bounded keys. It never
 * calls nvs_flash_init (the future caller owns NVS initialization), never
 * erases the partition or any other namespace, and never logs payloads,
 * pool identities, account/worker values or NVS error text. Compiled as a
 * production component; NEVER instantiated by any runtime path in Gate B3.
 *
 * [IDF guarantee relied upon]: an interrupted write of a single key-value
 * pair preserves the previous value ("This should not result in loss of
 * data, except for the new key-value pair if it was being written at the
 * moment of powering off" — ESP-IDF NVS documentation). The dual-slot +
 * pointer-write-last algorithm in pool_session_store.c needs exactly that
 * per-pair guarantee and nothing stronger. NVS API calls are internally
 * thread-safe; the store contract is nevertheless single-owner-synchronous
 * (Gate B5 enforces ownership).
 */

#include <string.h>
#include "nvs.h"
#include "pool_session_store.h"

_Static_assert(sizeof(nvs_handle_t) == sizeof(uint32_t),
               "PoolStoreNvsBackend handle storage no longer matches nvs_handle_t");

static int nvs_be_open(void *ctx)
{
    PoolStoreNvsBackend *b = (PoolStoreNvsBackend *)ctx;
    nvs_handle_t h;
    esp_err_t err;

    if (b == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    if (b->open) {
        return POOL_STORE_BACKEND_OK; /* deterministic repeated open */
    }
    err = nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    b->nvs_handle = (uint32_t)h;
    b->open = true;
    return POOL_STORE_BACKEND_OK;
}

static int nvs_be_read_blob(void *ctx, const char *key, uint8_t *buf, size_t cap,
                            size_t *out_len)
{
    PoolStoreNvsBackend *b = (PoolStoreNvsBackend *)ctx;
    size_t size = 0;
    esp_err_t err;

    if (b == NULL || !b->open || key == NULL || out_len == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = 0;
    /* Size query first (nvs_get_blob with a NULL buffer). */
    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return POOL_STORE_BACKEND_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = size;
    if (buf == NULL || size > cap) {
        /* Report the stored length without copying: the store classifies an
         * oversized blob as corrupt, not as an I/O failure. */
        return POOL_STORE_BACKEND_OK;
    }
    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, buf, &size);
    if (err != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    *out_len = size;
    return POOL_STORE_BACKEND_OK;
}

static int nvs_be_write_blob(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    PoolStoreNvsBackend *b = (PoolStoreNvsBackend *)ctx;

    if (b == NULL || !b->open || key == NULL || buf == NULL || len == 0) {
        return POOL_STORE_BACKEND_IO;
    }
    if (nvs_set_blob((nvs_handle_t)b->nvs_handle, key, buf, len) != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    return POOL_STORE_BACKEND_OK;
}

static int nvs_be_commit(void *ctx)
{
    PoolStoreNvsBackend *b = (PoolStoreNvsBackend *)ctx;

    if (b == NULL || !b->open) {
        return POOL_STORE_BACKEND_IO;
    }
    if (nvs_commit((nvs_handle_t)b->nvs_handle) != ESP_OK) {
        return POOL_STORE_BACKEND_IO;
    }
    return POOL_STORE_BACKEND_OK;
}

static int nvs_be_close(void *ctx)
{
    PoolStoreNvsBackend *b = (PoolStoreNvsBackend *)ctx;

    if (b == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    if (b->open) {
        nvs_close((nvs_handle_t)b->nvs_handle);
        b->open = false;
        b->nvs_handle = 0;
    }
    return POOL_STORE_BACKEND_OK; /* idempotent */
}

static const PoolStoreBackendOps s_nvs_ops = {
    .open       = nvs_be_open,
    .read_blob  = nvs_be_read_blob,
    .write_blob = nvs_be_write_blob,
    .commit     = nvs_be_commit,
    .close      = nvs_be_close,
};

const PoolStoreBackendOps *pool_store_nvs_ops(void)
{
    return &s_nvs_ops;
}
