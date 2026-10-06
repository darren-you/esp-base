cmake_minimum_required(VERSION 3.16)
project(base_frps_ota_interop LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(BUILD_TESTING OFF CACHE BOOL "Do not run unrelated tests" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "No upstream programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "No upstream tests" FORCE)
add_library(cjson STATIC "${BASE_COMPONENTS}/espressif__cjson/cJSON/cJSON.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/headers/cjson")
configure_file("${BASE_COMPONENTS}/espressif__cjson/cJSON/cJSON.h"
  "${CMAKE_CURRENT_BINARY_DIR}/headers/cjson/cJSON.h" COPYONLY)
target_include_directories(cjson PUBLIC "${BASE_COMPONENTS}/espressif__cjson/cJSON"
  "${CMAKE_CURRENT_BINARY_DIR}/headers")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cjson_config")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cjson_config/cJSONConfig.cmake" "set(cJSON_FOUND TRUE)\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cjson_config/cJSONConfigVersion.cmake"
  "set(PACKAGE_VERSION 1.7.19)\nif(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\nset(PACKAGE_VERSION_EXACT TRUE)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nendif()\n")
set(cJSON_DIR "${CMAKE_CURRENT_BINARY_DIR}/cjson_config" CACHE PATH "Exact managed cJSON" FORCE)
set(EFRP_MBEDTLS_SOURCE_DIR "${MBEDTLS_SOURCE}" CACHE PATH "Explicit existing official source" FORCE)
set(EFRP_TEST_UPSTREAM_CRYPTO OFF CACHE BOOL "Only the new composed scenario" FORCE)
set(EFRP_TEST_UPSTREAM_YAMUX OFF CACHE BOOL "No unrelated tests" FORCE)
set(GEN_FILES OFF CACHE BOOL "Use release generated sources" FORCE)
add_subdirectory("${BASE_COMPONENTS}/esp_frp" "efrp")
set(base_includes
  "${MBEDTLS_SOURCE}/tf-psa-crypto/include" "${MBEDTLS_SOURCE}/include"
  "${FIXTURE_INPUT}/fakes" "${BASE_ROOT}/firmware/tests/fakes/protocol-path"
  "${BASE_ROOT}/firmware/tests/fakes/ota-update" "${BASE_ROOT}/firmware/tests/fakes"
  "${BASE_ROOT}/firmware/components/device_protocol/include"
  "${BASE_ROOT}/firmware/components/device_protocol"
  "${BASE_ROOT}/firmware/components/device_identity/include"
  "${BASE_ROOT}/firmware/components/remote_config/include"
  "${BASE_ROOT}/firmware/components/ota_operation/include"
  "${BASE_ROOT}/firmware/components/native_business/include"
  "${BASE_ROOT}/firmware/components/wifi_runtime/include"
  "${BASE_ROOT}/firmware/components/time_runtime/include"
  "${BASE_COMPONENTS}/esp_ota/include" "${BASE_COMPONENTS}/esp_frp/include"
  "${BASE_COMPONENTS}/mqtt/runtime/include"
  "${BASE_COMPONENTS}/esp_frp/src" "${BASE_COMPONENTS}/esp_frp/tests"
  "${FIXTURE_INPUT}")
add_library(base_decoder STATIC
  "${BASE_ROOT}/firmware/components/device_protocol/command_decoder.c"
  "${BASE_ROOT}/firmware/components/remote_config/config_codec.c")
target_include_directories(base_decoder PRIVATE ${base_includes})
target_link_libraries(base_decoder PRIVATE cjson)
target_compile_definitions(base_decoder PRIVATE ebase_parse_command=ebase_parse_command_real
  ebase_parse_frp_status=ebase_parse_frp_status_real ebase_config_encode=ebase_config_encode_real)
add_executable(frps_ota_peer
  frps_ota_peer.c
  "${BASE_COMPONENTS}/esp_frp/src/connect.c"
  "${BASE_COMPONENTS}/esp_frp/tests/dns_fixture.c"
  "${BASE_COMPONENTS}/esp_frp/tests/client_port_posix.c"
  "${BASE_COMPONENTS}/esp_frp/tests/flash_store_fixture.c"
  "${BASE_COMPONENTS}/esp_ota/src/update.c"
  "${BASE_COMPONENTS}/esp_ota/src/http_deadline.c"
  "${BASE_ROOT}/firmware/components/device_protocol/network_auth.c"
  "${BASE_ROOT}/firmware/components/device_protocol/frp_management_listener.c"
  "${BASE_ROOT}/firmware/components/device_protocol/command_guard.c"
  "${BASE_ROOT}/firmware/components/device_protocol/control_state.c"
  "${BASE_ROOT}/firmware/components/native_business/esp_base_business.c"
  "${BASE_ROOT}/firmware/components/ota_operation/esp_base_storage_owner.c"
  "${BASE_ROOT}/firmware/components/ota_operation/esp_base_ota_policy.c"
  "${BASE_ROOT}/firmware/components/ota_operation/esp_base_ota_firmware.c"
  "${BASE_ROOT}/firmware/components/ota_operation/esp_base_ota_receipt.c")
target_include_directories(frps_ota_peer PRIVATE ${base_includes})
target_link_libraries(frps_ota_peer PRIVATE esp_frp base_decoder cjson MbedTLS::mbedtls
  MbedTLS::mbedx509 TF-PSA-Crypto::tfpsacrypto pthread)
target_compile_definitions(frps_ota_peer PRIVATE _POSIX_C_SOURCE=200809L EOTA_TEST_CHIP_ID=${CHIP_ID})
target_compile_definitions(base_decoder PRIVATE _POSIX_C_SOURCE=200809L EOTA_TEST_CHIP_ID=${CHIP_ID})
target_compile_options(frps_ota_peer PRIVATE -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable)
if(APPLE)
  target_link_options(frps_ota_peer PRIVATE -Wl,-dead_strip)
else()
  target_link_options(frps_ota_peer PRIVATE -Wl,--gc-sections)
endif()
