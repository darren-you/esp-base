idf_component_register(
    SRCS "esp_base_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support nvs_flash spi_flash
)
