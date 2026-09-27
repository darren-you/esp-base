idf_component_register(SRCS "esp_base_main.c" INCLUDE_DIRS "."
    REQUIRES container_binding device_identity device_protocol esp_frp esp_ota esp_partition esp_timer freertos nvs_flash ota_operation remote_config safety_runtime time_runtime)
