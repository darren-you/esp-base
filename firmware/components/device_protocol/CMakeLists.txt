idf_component_register(
    SRCS "esp_base_protocol.c" "command_guard.c" "command_decoder.c" "control_state.c" "network_auth.c" "mqtt_command.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system freertos device_identity esp_driver_usb_serial_jtag remote_config wifi_runtime time_runtime ota_runtime mbedtls
)
