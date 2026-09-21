idf_component_register(SRCS "esp_base_main.c" INCLUDE_DIRS "."
    REQUIRES device_identity device_protocol nvs_flash ota_runtime remote_config safety_runtime)
