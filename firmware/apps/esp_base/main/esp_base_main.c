#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_base_identity.h"
#include "esp_base_ota.h"
#include "esp_base_protocol.h"
#include "esp_base_remote_config.h"
#include "esp_base_safety.h"

static const char *TAG = "esp_base";

static esp_err_t initialise_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    return result;
}

void app_main(void)
{
    const esp_err_t storage_status = initialise_nvs();
    if (storage_status != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); storage preserved, initialization stopped", esp_err_to_name(storage_status));
        return;
    }

    static esp_base_identity_t identity = {0};
    const esp_err_t identity_status = esp_base_identity_read(&identity);
    if (identity_status != ESP_OK) {
        ESP_LOGE(TAG, "Identity unavailable (%s); initialization stopped", esp_err_to_name(identity_status));
        return;
    }

    esp_base_safety_t safety = {0};
    ESP_ERROR_CHECK(esp_base_safety_start(&safety));

    esp_base_remote_config_t config = {0};
    ESP_ERROR_CHECK(esp_base_remote_config_load(&config));

    esp_base_ota_t ota = {0};
    ESP_ERROR_CHECK(esp_base_ota_inspect(&ota));

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG,
             "ESP_BASE_BOOT project=ESP Base device_id=%s version=%s idf=%s chip=%s revision=%d.%d flash=%" PRIu32
             " slot=%s reset=%s config_generation=%" PRIu32,
             identity.device_id,
             app->version,
             esp_get_idf_version(),
             identity.model,
             identity.revision / 100,
             identity.revision % 100,
             identity.flash_size_bytes,
             ota.running_partition,
             safety.reset_reason,
             config.generation);

    /* Pending images are confirmed only after the P5 self-test/stability gate. */

    const esp_base_protocol_context_t protocol = {
        .device_id = identity.device_id,
        .firmware_version = app->version,
        .chip_model = identity.model,
        .flash_size_bytes = identity.flash_size_bytes,
        .config_generation = config.generation,
        .reset_reason = safety.reset_reason,
        .provisioned = false,
    };
    ESP_ERROR_CHECK(esp_base_protocol_start(&protocol));

    ESP_LOGI(TAG, "ESP_BASE_READY hardware_outputs=untouched provisioning=required");
}
