#!/usr/bin/env bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "${ESP_BASE_TEST_TARGET:-esp32c3}" in
  esp32c3) target_define=CONFIG_IDF_TARGET_ESP32C3 ;;
  esp32) target_define=CONFIG_IDF_TARGET_ESP32 ;;
  *) printf 'esp-base FRP crypto tests\n  error  ESP_BASE_TEST_TARGET must be esp32c3 or esp32.\n' >&2; exit 2 ;;
esac
build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
cjson_dir="$test_root/managed_components/espressif__cjson/cJSON"
[[ -f "$cjson_dir/cJSON.c" ]] || { printf 'esp-base FRP crypto tests\n  error  Resolve the locked cJSON dependency first.\n' >&2; exit 1; }
command -v pkg-config >/dev/null && pkg-config --exists openssl || {
  printf 'esp-base FRP crypto tests\n  error  OpenSSL development files and pkg-config are required.\n' >&2; exit 1;
}
read -r -a openssl_cflags <<< "$(pkg-config --cflags openssl)"
read -r -a openssl_libs <<< "$(pkg-config --libs openssl)"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -Wno-deprecated-declarations \
  -I "$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$build_dir/cjson.o"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D"$target_define"=1 \
  -Wall -Wextra -Werror -fsanitize=address,undefined "${openssl_cflags[@]}" \
  -I "$test_root/tests/fakes/network_auth" -I "$test_root/tests/fakes" \
  -I "$test_root/components/device_protocol/include" -I "$test_root/components/remote_config/include" \
  -I "$test_root/components/ota_operation/include" -I "$test_root/managed_components/esp_ota/include" -I "$cjson_dir" \
  "$test_root/components/device_protocol/network_auth.c" \
  "$test_root/components/device_protocol/frp_status_listener.c" \
  "$test_root/components/device_protocol/command_decoder.c" \
  "$test_root/components/device_protocol/command_guard.c" \
  "$test_root/components/remote_config/config_codec.c" \
  "$test_root/tests/frp_status_crypto_test.c" "$build_dir/cjson.o" \
  "${openssl_libs[@]}" -lm -o "$build_dir/frp_status_crypto_test"
"$build_dir/frp_status_crypto_test"
