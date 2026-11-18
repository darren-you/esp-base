idf_component_register(SRCS "base_profile.c" INCLUDE_DIRS "include"
    REQUIRES base_protocol
    PRIV_REQUIRES base_runtime esp_hw_support esp_netif esp_wifi esp_timer heap lwip spi_flash)
# 业务运行时留给系统层的接口。
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../base_runtime/system")
