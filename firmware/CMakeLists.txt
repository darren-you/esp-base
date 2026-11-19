cmake_minimum_required(VERSION 3.22)

# 版本 = release.json 的产品版本 + 构建号；本地构建的构建号为 0。
file(READ "${CMAKE_CURRENT_LIST_DIR}/../release.json" esp_base_release)
string(JSON esp_base_version GET "${esp_base_release}" version)
if(NOT DEFINED ESP_BASE_BUILD_NUMBER)
    set(ESP_BASE_BUILD_NUMBER 0)
endif()
set(PROJECT_VER "${esp_base_version}+${ESP_BASE_BUILD_NUMBER}")

# components/ 是系统层和业务运行时；wasm/ 是业务层（WASM 宿主），连同它对 WAMR 的构建设置单独放一个目录。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/apps/esp_base/main" "${CMAKE_CURRENT_LIST_DIR}/wasm")
# 只构建主程序依赖到的组件。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 两个目标各有一份由官方 Component Manager 生成的依赖锁。
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(esp_base)
