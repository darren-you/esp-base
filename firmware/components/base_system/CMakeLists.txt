idf_component_register(SRCS "base_system.c" "public_ip.c" INCLUDE_DIRS "include"
    REQUIRES base_runtime
    PRIV_REQUIRES base_net base_ota base_profile base_protocol base_store app_update esp_http_client esp_timer heap mbedtls log_esp
                  espcoredump) # 只构建依赖闭包：不列出来的话 sdkconfig 里的 Core Dump 写 Flash 不会生效
# 业务运行时留给系统层的接口（业务组件看不到）。
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../base_runtime/system")
