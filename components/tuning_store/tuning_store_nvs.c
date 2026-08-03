/*
 * NeuralAxe Weather-Aware Tuning — real ESP-IDF NVS backend (Gate W2).
 *
 * Opens ONLY the dedicated "nx_wtp" namespace with bounded keys. It never
 * calls nvs_flash_init (the future caller owns NVS initialization), never
 * erases the partition or any other namespace, and never logs payloads or
 * NVS error text. Compiled as a production component; NEVER instantiated
 * by any runtime path in Gate W2 (Gate W4 owns wiring under the B5 lease).
 *
 * [IDF guarantee relied upon]: an interrupted write of a single key-value
 * pair preserves the previous value (ESP-IDF NVS documentation). The
 * dual-slot + pointer-write-last algorithm in tuning_store.c needs exactly
 * that per-pair guarantee and nothing stronger.
 */

#include <string.h>
#include "nvs.h"
#include "tuning_store.h"

_Static_assert(sizeof(nvs_handle_t) == sizeof(uint32_t),
               "TuningStoreNvsBackend handle storage no longer matches nvs_handle_t");

static int nvs_be_open(void *ctx)
{
    TuningStoreNvsBackend *b = (TuningStoreNvsBackend *)ctx;
    nvs_handle_t h;
    esp_err_t err;

    if (b == NULL) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (b->open) {
        return TUNING_STORE_BACKEND_OK; /* deterministic repeated open */
    }
    err = nvs_open(TUNING_STORE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return TUNING_STORE_BACKEND_IO;
    }
    b->nvs_handle = (uint32_t)h;
    b->open = true;
    return TUNING_STORE_BACKEND_OK;
}

static int nvs_be_read_blob(void *ctx, const char *key, uint8_t *buf, size_t cap,
                            size_t *out_len)
{
    TuningStoreNvsBackend *b = (TuningStoreNvsBackend *)ctx;
    size_t size = 0;
    esp_err_t err;

    if (b == NULL || !b->open || key == NULL || out_len == NULL) {
        return TUNING_STORE_BACKEND_IO;
    }
    *out_len = 0;
    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return TUNING_STORE_BACKEND_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return TUNING_STORE_BACKEND_IO;
    }
    *out_len = size;
    if (buf == NULL || size > cap) {
        /* Report the stored length without copying: the store classifies
         * an oversized blob as corrupt, not as an I/O failure. */
        return TUNING_STORE_BACKEND_OK;
    }
    err = nvs_get_blob((nvs_handle_t)b->nvs_handle, key, buf, &size);
    if (err != ESP_OK) {
        return TUNING_STORE_BACKEND_IO;
    }
    *out_len = size;
    return TUNING_STORE_BACKEND_OK;
}

static int nvs_be_write_blob(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    TuningStoreNvsBackend *b = (TuningStoreNvsBackend *)ctx;

    if (b == NULL || !b->open || key == NULL || buf == NULL || len == 0) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (nvs_set_blob((nvs_handle_t)b->nvs_handle, key, buf, len) != ESP_OK) {
        return TUNING_STORE_BACKEND_IO;
    }
    return TUNING_STORE_BACKEND_OK;
}

static int nvs_be_commit(void *ctx)
{
    TuningStoreNvsBackend *b = (TuningStoreNvsBackend *)ctx;

    if (b == NULL || !b->open) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (nvs_commit((nvs_handle_t)b->nvs_handle) != ESP_OK) {
        return TUNING_STORE_BACKEND_IO;
    }
    return TUNING_STORE_BACKEND_OK;
}

static int nvs_be_close(void *ctx)
{
    TuningStoreNvsBackend *b = (TuningStoreNvsBackend *)ctx;

    if (b == NULL) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (b->open) {
        nvs_close((nvs_handle_t)b->nvs_handle);
        b->open = false;
        b->nvs_handle = 0;
    }
    return TUNING_STORE_BACKEND_OK; /* idempotent */
}

static const TuningStoreBackendOps s_nvs_ops = {
    .open       = nvs_be_open,
    .read_blob  = nvs_be_read_blob,
    .write_blob = nvs_be_write_blob,
    .commit     = nvs_be_commit,
    .close      = nvs_be_close,
};

const TuningStoreBackendOps *tuning_store_nvs_ops(void)
{
    return &s_nvs_ops;
}
