// SPDX-License-Identifier: Apache-2.0
// QEMU-only probe. It never opens a serial device or embeds backup bytes.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

static uint8_t source_blob[136];
static uint8_t candidate_blob[136];
static uint8_t readback_blob[136];

void app_main(void)
{
    esp_err_t error = nvs_flash_init_partition("base_store");
    printf("PROBE_INIT=%d\n", (int)error);
    if (error != ESP_OK) return;
#ifdef PROBE_INIT_ONLY
    printf("PROBE_DONE=init_only\n");
    return;
#endif
    nvs_handle_t handle;
#ifdef PROBE_VERIFY_ONLY
    error = nvs_open_from_partition("base_store", "base_config", NVS_READONLY, &handle);
#else
    error = nvs_open_from_partition("base_store", "base_config", NVS_READWRITE, &handle);
#endif
    printf("PROBE_OPEN=%d\n", (int)error);
    if (error != ESP_OK) return;
    size_t source_size = sizeof source_blob;
    error = nvs_get_blob(handle, "committed", source_blob, &source_size);
    printf("PROBE_GET=%d\n", (int)error);
    if (error != ESP_OK) { nvs_close(handle); return; }
#ifdef PROBE_VERIFY_ONLY
    const bool is_v3 = source_size >= 40 && source_size <= sizeof source_blob &&
                       memcmp(source_blob, "EBCF", 4) == 0 && source_blob[4] == 3 &&
                       source_size == (size_t)(40u + source_blob[6] + source_blob[7]);
    printf("PROBE_V3=%d\n", is_v3);
    printf("PROBE_DONE=verify_only\n");
    nvs_close(handle);
    return;
#endif
    if (source_size != 112 || memcmp(source_blob, "EBCF", 4) || source_blob[4] != 1 ||
        source_blob[5] > 1 || source_blob[6] > 32 || source_blob[7] > 64 ||
        memcmp(source_blob + 12, "\0\0\0\0", 4)) {
        printf("PROBE_DONE=source_invalid\n");
        nvs_close(handle);
        return;
    }
    for (size_t i = 16u + source_blob[6]; i < 48u; ++i) {
        if (source_blob[i]) { printf("PROBE_DONE=source_invalid\n"); nvs_close(handle); return; }
    }
    for (size_t i = 48u + source_blob[7]; i < 112u; ++i) {
        if (source_blob[i]) { printf("PROBE_DONE=source_invalid\n"); nvs_close(handle); return; }
    }
    memset(candidate_blob, 0, sizeof candidate_blob);
    memcpy(candidate_blob, "EBCF", 4);
    candidate_blob[4] = 3;
    memcpy(candidate_blob + 5, source_blob + 5, 7);
    const size_t candidate_size = 40u + source_blob[6] + source_blob[7];
    memcpy(candidate_blob + 40, source_blob + 16, source_blob[6]);
    memcpy(candidate_blob + 40 + source_blob[6], source_blob + 48, source_blob[7]);
    error = nvs_set_blob(handle, "committed", candidate_blob, candidate_size);
    printf("PROBE_SET=%d\n", (int)error);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
        printf("PROBE_COMMIT=%d\n", (int)error);
    }
    nvs_close(handle);
    if (error != ESP_OK) return;
    error = nvs_open_from_partition("base_store", "base_config", NVS_READONLY, &handle);
    printf("PROBE_REOPEN=%d\n", (int)error);
    if (error != ESP_OK) return;
    size_t readback_size = sizeof readback_blob;
    error = nvs_get_blob(handle, "committed", readback_blob, &readback_size);
    printf("PROBE_READBACK=%d\n", (int)error);
    printf("PROBE_MATCH=%d\n", error == ESP_OK && readback_size == candidate_size &&
           memcmp(readback_blob, candidate_blob, candidate_size) == 0);
    nvs_close(handle);
    printf("PROBE_DONE=commit\n");
}
