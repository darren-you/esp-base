idf_component_register(
    SRCS "esp_base_ota.c" "esp_base_ota_update.c" "esp_base_ota_receipt.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_partition esp_http_client esp_timer mbedtls nvs_flash
)
