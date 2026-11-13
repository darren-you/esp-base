idf_component_register(SRCS "ota_logic.c" "ota_worker.c" INCLUDE_DIRS "include"
    REQUIRES base_protocol
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_http_client esp_https_ota esp_partition mbedtls)
