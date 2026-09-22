idf_component_register(
    SRCS "esp_base_remote_config.c" "config_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
