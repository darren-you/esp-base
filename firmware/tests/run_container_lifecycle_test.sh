#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  printf 'usage: %s <locked-container-source> <locked-wamr-source> <wasi-sdk-root>\n' "$0" >&2
  exit 2
fi

container_source="$1"
wamr_source="$2"
wasi_sdk_root="$3"
firmware_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "${ESP_BASE_TEST_TARGET:-esp32c3}" in
  esp32c3) target_define=CONFIG_IDF_TARGET_ESP32C3 ;;
  esp32) target_define=CONFIG_IDF_TARGET_ESP32 ;;
  *) printf 'container lifecycle test\n  error  ESP_BASE_TEST_TARGET must be esp32c3 or esp32.\n' >&2; exit 2 ;;
esac
locked_sha="$(awk -F '"' '/^[[:space:]]*version: / {print $2; exit}' \
  "$firmware_root/integrations/container_binding/idf_component.yml")"
actual_sha="$(git -C "$container_source" rev-parse HEAD)"
locked_wamr_sha="$(awk '/^  wasm-micro-runtime:$/ {found=1; next}
  found && /^    version: / {print $2; exit}' "$firmware_root/dependencies.lock")"
locked_wamr_sha_esp32="$(awk '/^  wasm-micro-runtime:$/ {found=1; next}
  found && /^    version: / {print $2; exit}' "$firmware_root/dependencies.lock.esp32")"
actual_wamr_sha="$(git -C "$wamr_source" rev-parse HEAD)"
if [[ -z "$locked_sha" || "$actual_sha" != "$locked_sha" ||
      -z "$locked_wamr_sha" || "$actual_wamr_sha" != "$locked_wamr_sha" ||
      "$locked_wamr_sha_esp32" != "$locked_wamr_sha" ||
      -n "$(git -C "$container_source" status --porcelain)" ||
      -n "$(git -C "$wamr_source" status --porcelain)" ||
      ! -f "$wamr_source/core/iwasm/include/wasm_export.h" ||
      ! -f "$wasi_sdk_root/bin/clang" ]]; then
  printf 'container lifecycle test\n  error  Pass clean, exact locked Container/WAMR source and wasi-sdk.\n' >&2
  exit 1
fi

eota_include="${ESP_OTA_INCLUDE:-$firmware_root/managed_components/esp_ota/include}"
if [[ ! -f "$eota_include/eota.h" ]]; then
  printf 'container lifecycle test\n  error  Set ESP_OTA_INCLUDE to the locked esp-ota include directory.\n' >&2
  exit 1
fi
if [[ -n "${ESP_OTA_INCLUDE:-}" ]]; then
  locked_eota_sha="$(awk -F '"' '/^[[:space:]]*esp_ota:$/ {found=1; next}
    found && /^[[:space:]]*version: / {print $2; exit}' \
    "$firmware_root/components/device_protocol/idf_component.yml")"
  actual_eota_sha="$(git -C "$eota_include" rev-parse HEAD)"
  if [[ -z "$locked_eota_sha" || "$actual_eota_sha" != "$locked_eota_sha" ||
        -n "$(git -C "$eota_include" status --porcelain)" ]]; then
    printf 'container lifecycle test\n  error  ESP_OTA_INCLUDE must come from the exact clean locked esp-ota checkout.\n' >&2
    exit 1
  fi
fi

frp_include="${ESP_FRP_INCLUDE:-$firmware_root/managed_components/esp_frp/include}"
mqtt_include="${ESP_MQTT_INCLUDE:-$firmware_root/managed_components/mqtt/runtime/include}"
for dependency in frp mqtt; do
  if [[ "$dependency" == frp ]]; then
    dependency_include="$frp_include"; component=esp_frp
    required_header=esp_frp.h; explicit_include="${ESP_FRP_INCLUDE:-}"
  else
    dependency_include="$mqtt_include"; component=mqtt
    required_header=emqtt_contract.h; explicit_include="${ESP_MQTT_INCLUDE:-}"
  fi
  if [[ ! -f "$dependency_include/$required_header" ]]; then
    printf 'container lifecycle test\n  error  Resolve locked %s headers or set its explicit include path.\n' "$component" >&2
    exit 1
  fi
  if [[ -n "$explicit_include" ]]; then
    locked_dependency_sha="$(awk -v component="$component" -F '"' '
      $0 ~ "^[[:space:]]*" component ":$" {found=1; next}
      found && /^[[:space:]]*version: / {print $2; exit}' \
      "$firmware_root/components/device_protocol/idf_component.yml")"
    if [[ -z "$locked_dependency_sha" ||
          "$(git -C "$dependency_include" rev-parse HEAD)" != "$locked_dependency_sha" ||
          -n "$(git -C "$dependency_include" status --porcelain)" ]]; then
      printf 'container lifecycle test\n  error  Explicit %s headers must match its clean locked checkout.\n' "$component" >&2
      exit 1
    fi
  fi
done

build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
read -r -a openssl_cflags <<< "$(pkg-config --cflags openssl)"
read -r -a openssl_libs <<< "$(pkg-config --libs openssl)"
container_include="$container_source/components/esp_container/include"
if ! cmake -S "$container_source" -B "$build_dir/container-build" \
    -DBUILD_TESTING=ON -DPython3_EXECUTABLE="$(command -v "${TEST_PYTHON:-python3}")" \
    -DESP_CONTAINER_WAMR_SOURCE="$wamr_source" \
    -DESP_CONTAINER_WASI_SDK_ROOT="$wasi_sdk_root" \
    >"$build_dir/configure.log" 2>&1; then
  tail -n 80 "$build_dir/configure.log" >&2
  exit 1
fi
if ! cmake --build "$build_dir/container-build" --target slot_runtime_test \
    >"$build_dir/build.log" 2>&1; then
  tail -n 100 "$build_dir/build.log" >&2
  exit 1
fi
wamr_library="$build_dir/container-build/wamr/libiwasm.a"
printf 'container lifecycle test\n  Container %s\n  WAMR      %s\n  WAMR lib  %s\n' \
  "$actual_sha" "$actual_wamr_sha" \
  "$(openssl dgst -sha256 -r "$wamr_library" | awk '{print $1}')"

compile_args=(-std=c11 -D_POSIX_C_SOURCE=200809L \
  -D"$target_define"=1 \
  -DCONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES=32768 \
  -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
  -DCONFIG_ESP_BASE_TIME_SERVER=\"time.example.invalid\" -DCONFIG_ESP_CONSOLE_UART_NUM=0 \
  -I "$firmware_root/tests/fakes/container_product" \
  -I "$firmware_root/integrations/container_binding/include" \
  -I "$firmware_root/tests/fakes" \
  -I "$firmware_root/tests/fakes/protocol-path" \
  -I "$firmware_root/tests/fakes/app_main" \
  -I "$firmware_root/tests/fakes/ota_update" \
  -I "$firmware_root/components/device_protocol" \
  -I "$firmware_root/components/device_protocol/include" \
  -I "$firmware_root/components/device_identity/include" \
  -I "$firmware_root/components/remote_config/include" \
  -I "$firmware_root/components/wifi_runtime/include" \
  -I "$firmware_root/components/time_runtime/include" \
  -I "$firmware_root/components/safety_runtime/include" \
  -I "$frp_include" -I "$mqtt_include" \
  -I "$firmware_root/integrations/container_binding" \
  -I "$firmware_root/integrations/container_binding/include" \
  -I "$firmware_root/components/ota_operation/include" \
  -I "$eota_include" -I "$container_include" "${openssl_cflags[@]}" \
  "$firmware_root/tests/container_product_lifecycle_test.c" \
  "$firmware_root/tests/historical_ota_startup_protocol.c" \
  "$build_dir/historical_ota_app_main.o" \
  "$firmware_root/components/device_protocol/product_ledger.c" \
  "$firmware_root/components/device_protocol/command_guard.c" \
  "$firmware_root/components/ota_operation/esp_base_storage_owner.c" \
  "$firmware_root/integrations/container_binding/esp_base_container_binding.c" \
  "$firmware_root/integrations/container_binding/esp_base_container_no_package.c" \
  "$build_dir/container-build/libesp_container.a" "$wamr_library" \
  "${openssl_libs[@]}" -lm -ldl)
if [[ "$(uname -s)" == Darwin ]]; then
  compile_args+=(-Wl,-dead_strip)
else
  compile_args+=(-Wl,--gc-sections)
fi

# Compile the actual startup TU with its logging/platform headers, while
# retaining the real Container product API rather than app_main's unit fake.
"${CC:-cc}" -std=c11 -D"$target_define"=1 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections \
  -I "$firmware_root/integrations/container_binding/include" \
  -I "$firmware_root/tests/fakes/app_main" -I "$firmware_root/tests/fakes" \
  -I "$firmware_root/components/device_identity/include" \
  -I "$firmware_root/components/device_protocol/include" \
  -I "$firmware_root/components/remote_config/include" \
  -I "$firmware_root/components/safety_runtime/include" \
  -I "$firmware_root/components/time_runtime/include" \
  -I "$firmware_root/components/ota_operation/include" \
  -I "$eota_include" -I "$frp_include" -I "$container_include" \
  -c "$firmware_root/apps/esp_base/main/esp_base_main.c" \
  -o "$build_dir/historical_ota_app_main.o"

"${CC:-cc}" -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${compile_args[@]}" -o "$build_dir/container_product_lifecycle_test"

"${TEST_PYTHON:-python3}" "$container_source/tests/slot_runtime_test.py" \
  "$build_dir/container_product_lifecycle_test" \
  "$build_dir/container-build/runtime-guests"
"${TEST_PYTHON:-python3}" "$firmware_root/../tools/container_product_deadline_test.py" \
  "$build_dir/container_product_lifecycle_test" "$container_source" \
  "$wasi_sdk_root" "$build_dir/container-build/runtime-guests"
"${TEST_PYTHON:-python3}" "$firmware_root/../tools/container_product_cancel_test.py" \
  "$build_dir/container_product_lifecycle_test" "$container_source" "$wasi_sdk_root"

if [[ "$(uname -s)" == Darwin ]]; then
  "${CC:-cc}" -D_DARWIN_C_SOURCE=1 -DESP_BASE_TEST_RESOURCE_STATS=1 \
    "${compile_args[@]}" -o "$build_dir/container_product_resource_test"
  "${TEST_PYTHON:-python3}" "$container_source/tests/slot_runtime_test.py" \
    "$build_dir/container_product_resource_test" \
    "$build_dir/container-build/runtime-guests"
fi
