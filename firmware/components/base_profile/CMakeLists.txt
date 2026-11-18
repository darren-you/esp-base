idf_component_register(SRCS "base_profile.c" INCLUDE_DIRS "include"
    REQUIRES base_protocol
    PRIV_REQUIRES esp_hw_support esp_netif esp_wifi esp_timer heap lwip spi_flash)
