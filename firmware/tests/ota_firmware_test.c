#include "esp_base_ota_firmware.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_partition.h"

static bool signed_enabled, rollback_possible, change_during_hash;
static unsigned observe_calls, verify_calls, rollback_calls;
static uint8_t running_subtype, boot_subtype, image_seed[2];
static eota_state_t running_state, target_state;
static eota_result_t image_result[2];

static void reset(void)
{
    signed_enabled = rollback_possible = true;
    change_during_hash = false;
    observe_calls = verify_calls = rollback_calls = 0;
    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    running_state = target_state = EOTA_STATE_VALID;
    image_seed[0] = 0xa0;
    image_seed[1] = 0xb0;
    image_result[0] = image_result[1] = EOTA_UPDATE_OK;
}

bool eota_available(void) { return signed_enabled; }

eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
    assert(policy && slots && !strcmp(policy->project_name, "esp_base") &&
           policy->chip_id == 0x0005 && policy->ota_0_address_bytes == 0x20000 &&
           policy->ota_1_address_bytes == 0x200000 && policy->ota_size_bytes == 0x1e0000);
    const uint8_t other = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ?
                          ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    *slots = (eota_slots_t){
        .running_subtype = running_subtype,
        .boot_subtype = boot_subtype,
        .target_subtype = other,
        .running_address_bytes = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0x20000 : 0x200000,
        .boot_address_bytes = boot_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0x20000 : 0x200000,
        .target_address_bytes = other == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0x20000 : 0x200000,
        .running_size_bytes = 0x1e0000,
        .boot_size_bytes = 0x1e0000,
        .target_size_bytes = 0x1e0000,
        .running_state = running_state,
        .target_state = target_state,
    };
    ++observe_calls;
    if (change_during_hash && observe_calls == 2) slots->target_state = EOTA_STATE_INVALID;
    return EOTA_UPDATE_OK;
}

eota_result_t eota_sha256_verified_image(const eota_policy_t *policy, uint8_t subtype,
                                         uint32_t *image_size_bytes, uint8_t digest[32])
{
    assert(policy && image_size_bytes && digest &&
           (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1));
    ++verify_calls;
    const unsigned index = subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    *image_size_bytes = 0;
    memset(digest, 0, 32);
    if (image_result[index] != EOTA_UPDATE_OK) return image_result[index];
    *image_size_bytes = 123456;
    memset(digest, image_seed[index], 32);
    return EOTA_UPDATE_OK;
}

bool esp_ota_check_rollback_is_possible(void)
{
    ++rollback_calls;
    return rollback_possible;
}

static void expect_uncertain(void)
{
    esp_base_ota_firmware_set_t set;
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_UNCERTAIN);
    const esp_base_ota_firmware_set_t empty = {0};
    assert(memcmp(&set, &empty, sizeof set) == 0);
}

int main(void)
{
    esp_base_ota_firmware_set_t set;
    reset();
    assert(esp_base_ota_observe_firmware_set(NULL) == ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT);
    signed_enabled = false;
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_UNSUPPORTED);
    const esp_base_ota_firmware_set_t empty = {0};
    assert(memcmp(&set, &empty, sizeof set) == 0 && observe_calls == 0);

    reset();
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xa0 &&
           set.bootable_firmware_sha256[0][0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0xb0 &&
           observe_calls == 2 && verify_calls == 2 && rollback_calls == 2);

    reset(); running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xb0 &&
           set.bootable_firmware_sha256[1][0] == 0xa0);

    reset(); image_seed[1] = image_seed[0];
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 1 && set.bootable_firmware_sha256[0][0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0);

    reset(); target_state = EOTA_STATE_UNTRACKED;
    image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    assert(esp_base_ota_observe_firmware_set(&set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 1 && set.running_firmware_sha256[0] == 0xa0 && rollback_calls == 0);

    reset(); running_state = EOTA_STATE_PENDING_VERIFY; expect_uncertain();
    reset(); boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1; expect_uncertain();
    reset(); target_state = EOTA_STATE_NEW; expect_uncertain();
    reset(); target_state = EOTA_STATE_UNDEFINED; expect_uncertain();
    reset(); target_state = EOTA_STATE_PENDING_VERIFY; expect_uncertain();
    reset(); rollback_possible = false; expect_uncertain();
    reset(); image_result[0] = EOTA_UPDATE_IMAGE_INVALID; expect_uncertain();
    reset(); image_result[1] = EOTA_UPDATE_IMAGE_INVALID; expect_uncertain();
    reset(); target_state = EOTA_STATE_INVALID; expect_uncertain();
    reset(); target_state = EOTA_STATE_ABORTED; expect_uncertain();
    reset(); target_state = EOTA_STATE_UNTRACKED; expect_uncertain();
    reset(); target_state = EOTA_STATE_UNTRACKED;
    image_result[1] = EOTA_UPDATE_RESOURCE_FAILURE; expect_uncertain();
    reset(); change_during_hash = true; expect_uncertain();
    reset(); image_seed[0] = 0; expect_uncertain();
    puts("  ota_firmware passed (verified signed set, rollback, fallback and mutation rejection)");
}
