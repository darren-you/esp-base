if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT ESP_BASE_MQTT_LAB_INPUTS OR NOT IS_ABSOLUTE "${ESP_BASE_MQTT_LAB_INPUTS}" OR NOT EXISTS "${ESP_BASE_MQTT_LAB_INPUTS}")
        message(FATAL_ERROR "MQTT lab requires an explicit absolute ESP_BASE_MQTT_LAB_INPUTS header; see app README")
    endif()
    configure_file("${ESP_BASE_MQTT_LAB_INPUTS}" "${CMAKE_CURRENT_BINARY_DIR}/mqtt_lab_inputs.h" COPYONLY)
endif()
idf_component_register(SRCS "mqtt_lab_main.c" "mqtt_lab_resources.c" INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES mqtt device_identity remote_config wifi_runtime esp_netif esp_timer esp_system heap nvs_flash lwip)
