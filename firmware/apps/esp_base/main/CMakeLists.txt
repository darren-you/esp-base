idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "."
    PRIV_REQUIRES base_net base_ota base_protocol base_store app_update esp_timer heap mbedtls)
