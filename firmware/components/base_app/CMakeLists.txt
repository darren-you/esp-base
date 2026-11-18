idf_component_register(SRCS "base_app.c" INCLUDE_DIRS "include"
    REQUIRES base_store
    PRIV_REQUIRES base_net base_ota base_profile base_protocol app_update esp_timer heap mbedtls)
