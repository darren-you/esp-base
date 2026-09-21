idf_component_register(
    SRCS "esp_base_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_partition
)
