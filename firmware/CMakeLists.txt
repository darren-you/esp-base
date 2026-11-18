cmake_minimum_required(VERSION 3.22)

# 版本 = release.json 的产品版本 + 构建号；本地构建的构建号为 0。
file(READ "${CMAKE_CURRENT_LIST_DIR}/../release.json" esp_base_release)
string(JSON esp_base_version GET "${esp_base_release}" version)
if(NOT DEFINED ESP_BASE_BUILD_NUMBER)
    set(ESP_BASE_BUILD_NUMBER 0)
endif()
set(PROJECT_VER "${esp_base_version}+${ESP_BASE_BUILD_NUMBER}")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/apps/esp_base/main")
# 只构建主程序依赖到的组件。
set(COMPONENTS main)
# WAMR（WASM 宿主的解释器）自己的 Kconfig 没有覆盖的开关；AOT、WASI、线程等在 sdkconfig.defaults 里关闭。
set(WAMR_BUILD_INSTRUCTION_METERING 1) # 每次调用有指令预算：应用死循环时能停下来
set(WAMR_BUILD_BULK_MEMORY 0)          # 应用的目标 wasm32v1-none 不产生这些指令
set(WAMR_BUILD_SIMD 0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 两个目标各有一份由官方 Component Manager 生成的依赖锁。
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(esp_base)

# WAMR 2.4.6 的 ESP-IDF 端口在 ESP-IDF 6 上原样构建所需的编译选项（上游 #5115），不改它的源码：
# - 强制包含 wamr_idf6.h，补上它用到却没有包含的声明；
# - -Wno-format-security 与它自己设的 -Wno-format 配套；
# - 内存保护开启时 ESP-IDF 6 不定义 MALLOC_CAP_EXEC。可执行内存只有 AOT 用，本固件不启用 AOT，
#   申请它时照常失败即可。
idf_component_get_property(wamr_lib wasm-micro-runtime COMPONENT_LIB)
target_compile_options(${wamr_lib} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/components/base_wasm/port/wamr_idf6.h" -Wno-format-security)
if(NOT CONFIG_HEAP_HAS_EXEC_HEAP)
    target_compile_definitions(${wamr_lib} PRIVATE MALLOC_CAP_EXEC=MALLOC_CAP_INVALID)
endif()
