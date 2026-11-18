# WASM 宿主。core/ 不依赖 ESP-IDF（host 测试直接编译它），base_wasm.c 是设备上的平台实现和业务入口。
# WAMR 由组件管理器按精确提交取得（idf_component.yml），它在 ESP-IDF 6 上需要的编译选项在
# firmware/CMakeLists.txt 里统一加，不改它的源码。
idf_component_register(SRCS "base_wasm.c" "core/wasm_host.c" "core/wasm_package.c" "core/wasm_deploy.c" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "core"
    REQUIRES base_runtime
    PRIV_REQUIRES wasm-micro-runtime esp_timer heap mbedtls
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../../tests/apps/hello.wasm")
# WAMR 的 ESP-IDF 端口用 pthread_self() 取当前线程，而 ESP-IDF 的 pthread_self() 在不是 pthread 的任务里断言失败。
# 应用任务由 base_runtime 创建（普通 FreeRTOS 任务，才计入业务的任务和栈水位），所以链接时把 os_self_thread
# 换成按任务句柄实现的版本（base_wasm.c 的 __wrap_os_self_thread）。WAMR 只拿它作线程标识；不改它的源码。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=os_self_thread" "-u __wrap_os_self_thread")
