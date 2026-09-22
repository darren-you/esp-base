#include "esp_base_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

#define DEVICE_ID_NAMESPACE "base_identity"
#define DEVICE_ID_KEY "device_uuid"

static bool is_lower_hex(char character)
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

static bool is_uuid_v4(const char *value)
{
    if (value == NULL || strlen(value) != ESP_BASE_DEVICE_ID_LENGTH - 1) {
        return false;
    }
    for (size_t index = 0; index < ESP_BASE_DEVICE_ID_LENGTH - 1; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!is_lower_hex(value[index])) {
            return false;
        }
    }
    return value[14] == '4' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

esp_err_t esp_base_identity_generate_uuid(char *output, size_t output_size)
{
    if (output == NULL || output_size < ESP_BASE_DEVICE_ID_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bytes[16] = {0};
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3fU) | 0x80U);

    const int written = snprintf(output,
                                 output_size,
                                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                                 bytes[0], bytes[1], bytes[2], bytes[3],
                                 bytes[4], bytes[5],
                                 bytes[6], bytes[7],
                                 bytes[8], bytes[9],
                                 bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return written == ESP_BASE_DEVICE_ID_LENGTH - 1 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t load_or_create_device_id(char *output, size_t output_size)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(DEVICE_ID_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    size_t stored_size = output_size;
    result = nvs_get_str(handle, DEVICE_ID_KEY, output, &stored_size);
    if (result == ESP_OK) {
        nvs_close(handle);
        return is_uuid_v4(output) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return result;
    }

    result = esp_base_identity_generate_uuid(output, output_size);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, DEVICE_ID_KEY, output);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32C3:
        return "ESP32-C3";
    default:
        return "unsupported";
    }
}

esp_err_t esp_base_identity_read(esp_base_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    identity->model = chip_model_name(chip.model);
    identity->revision = chip.revision;

    esp_err_t result = esp_flash_get_size(NULL, &identity->flash_size_bytes);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_read_mac(identity->base_mac, ESP_MAC_BASE);
    if (result != ESP_OK) {
        return result;
    }

    return load_or_create_device_id(identity->device_id, sizeof(identity->device_id));
}
