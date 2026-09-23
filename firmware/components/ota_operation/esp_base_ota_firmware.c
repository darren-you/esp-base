// SPDX-License-Identifier: Apache-2.0
#include "esp_base_ota_firmware.h"

#include <stdbool.h>
#include <string.h>

#include "esp_base_ota_policy.h"
#include "esp_ota_ops.h"

static bool same_slots(const eota_slots_t *first, const eota_slots_t *second)
{
    return first->running_subtype == second->running_subtype &&
           first->boot_subtype == second->boot_subtype &&
           first->target_subtype == second->target_subtype &&
           first->running_address_bytes == second->running_address_bytes &&
           first->boot_address_bytes == second->boot_address_bytes &&
           first->target_address_bytes == second->target_address_bytes &&
           first->running_size_bytes == second->running_size_bytes &&
           first->boot_size_bytes == second->boot_size_bytes &&
           first->target_size_bytes == second->target_size_bytes &&
           first->running_state == second->running_state &&
           first->target_state == second->target_state;
}

static bool zero_digest(const uint8_t digest[EOTA_SHA256_BYTES])
{
    uint8_t any = 0;
    for (size_t i = 0; i < EOTA_SHA256_BYTES; ++i) any |= digest[i];
    return any == 0;
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_set_t *firmware_set)
{
    if (firmware_set == NULL) return ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT;
    *firmware_set = (esp_base_ota_firmware_set_t){0};
    if (!eota_available()) return ESP_BASE_OTA_FIRMWARE_UNSUPPORTED;

    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t before;
    if (eota_observe_slots(&policy, &before) != EOTA_UPDATE_OK ||
        before.running_subtype != before.boot_subtype ||
        before.running_address_bytes != before.boot_address_bytes ||
        before.running_size_bytes != before.boot_size_bytes ||
        before.running_state != EOTA_STATE_VALID) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;

    /* VALID means the current image passed Base's local confirmation. An
     * inactive VALID entry is admitted only when IDF also proves rollback is
     * possible. A verified image with any other state might still be loaded
     * by the bootloader's fallback scan, so refuse the whole observation. */
    const bool has_rollback = before.target_state == EOTA_STATE_VALID;
    if (!has_rollback && before.target_state != EOTA_STATE_UNTRACKED &&
        before.target_state != EOTA_STATE_INVALID &&
        before.target_state != EOTA_STATE_ABORTED) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    if (has_rollback && !esp_ota_check_rollback_is_possible()) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    uint32_t running_size = 0, target_size = 0;
    uint8_t running_sha256[EOTA_SHA256_BYTES], target_sha256[EOTA_SHA256_BYTES];
    if (eota_sha256_verified_image(&policy, before.running_subtype, &running_size,
                                   running_sha256) != EOTA_UPDATE_OK ||
        running_size == 0 || zero_digest(running_sha256)) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }
    const eota_result_t target_result = eota_sha256_verified_image(
        &policy, before.target_subtype, &target_size, target_sha256);
    if (has_rollback) {
        if (target_result != EOTA_UPDATE_OK || target_size == 0 ||
            zero_digest(target_sha256)) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    } else if (target_result != EOTA_UPDATE_IMAGE_INVALID) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    eota_slots_t after;
    if (eota_observe_slots(&policy, &after) != EOTA_UPDATE_OK ||
        !same_slots(&before, &after) ||
        (has_rollback && !esp_ota_check_rollback_is_possible())) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    memcpy(firmware_set->running_firmware_sha256, running_sha256, EOTA_SHA256_BYTES);
    memcpy(firmware_set->bootable_firmware_sha256[0], running_sha256, EOTA_SHA256_BYTES);
    firmware_set->bootable_count = 1;
    if (has_rollback && memcmp(running_sha256, target_sha256, EOTA_SHA256_BYTES) != 0) {
        memcpy(firmware_set->bootable_firmware_sha256[1], target_sha256, EOTA_SHA256_BYTES);
        firmware_set->bootable_count = 2;
    }
    return ESP_BASE_OTA_FIRMWARE_OK;
}
