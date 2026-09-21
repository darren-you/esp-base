idf_component_register(
    SRCS "esp_base_protocol.c" "command_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system freertos
)
