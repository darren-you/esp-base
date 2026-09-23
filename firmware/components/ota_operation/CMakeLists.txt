idf_component_register(
    SRCS "esp_base_ota_policy.c" "esp_base_ota_receipt.c" "esp_base_ota_firmware.c" "esp_base_storage_owner.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ota nvs_flash
)
