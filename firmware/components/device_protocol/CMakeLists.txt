if(IDF_TARGET STREQUAL "esp32c3")
    set(serial_driver esp_driver_usb_serial_jtag)
elseif(IDF_TARGET STREQUAL "esp32")
    set(serial_driver esp_driver_uart)
else()
    message(FATAL_ERROR "ESP Base serial control supports only esp32c3 and esp32")
endif()
idf_component_register(
    SRCS "esp_base_capacity.c" "esp_base_protocol.c" "command_guard.c" "command_decoder.c" "control_state.c" "network_auth.c" "mqtt_command.c" "mqtt_event.c" "mqtt_owner.c" "frp_owner.c" "frp_management_listener.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system esp_http_client mbedtls freertos nvs_flash device_identity ${serial_driver} remote_config wifi_runtime time_runtime ota_operation native_business esp_ota mqtt esp_frp lwip
)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../../sdk-lock.json" capacity_sdk_lock_sha256)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_BASE_CAPACITY_SDK_LOCK_SHA256="${capacity_sdk_lock_sha256}")
