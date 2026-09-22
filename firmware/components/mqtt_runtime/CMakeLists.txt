idf_component_register(SRCS "mqtt_contract.c" "esp_base_mqtt.c"
    INCLUDE_DIRS "include" REQUIRES freertos esp_timer espressif__mqtt)
