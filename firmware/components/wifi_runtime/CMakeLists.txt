idf_component_register(SRCS "esp_base_wifi.c" INCLUDE_DIRS "include"
    REQUIRES remote_config esp_wifi esp_netif esp_event esp_timer esp_hw_support freertos)
