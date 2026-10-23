#!/usr/bin/env bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
command -v pkg-config >/dev/null && pkg-config --exists openssl || {
  printf 'MQTT OTA tests require OpenSSL and pkg-config.\n' >&2; exit 1;
}
read -r -a openssl_cflags <<< "$(pkg-config --cflags openssl)"
read -r -a openssl_libs <<< "$(pkg-config --libs openssl)"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "${openssl_cflags[@]}" -I "$test_root/tests/fakes/network-auth" \
  -I "$test_root/tests/fakes" -I "$test_root/components/device_protocol/include" \
  "$test_root/components/device_protocol/network_auth.c" \
  "$test_root/components/device_protocol/command_guard.c" \
  "$test_root/components/device_protocol/mqtt_ota.c" \
  "$test_root/components/device_protocol/mqtt_ota_stream.c" \
  "$test_root/tests/mqtt_ota_test.c" "${openssl_libs[@]}" -o "$build_dir/mqtt_ota_test"
"$build_dir/mqtt_ota_test" "$test_root/tests/testdata/mqtt-ota-v1.txt"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I "$test_root/tests/fakes/mqtt-ota" \
  -I "$test_root/tests/fakes" -I "$test_root/components/device_protocol/include" \
  "$test_root/components/device_protocol/mqtt_ota_stream.c" \
  "$test_root/components/device_protocol/mqtt_ota_transfer.c" \
  "$test_root/tests/mqtt_ota_transfer_test.c" -o "$build_dir/mqtt_ota_transfer_test"
"$build_dir/mqtt_ota_transfer_test"
if [[ "${ESP_BASE_TEST_THREAD_SANITIZER:-0}" == 1 ]]; then
  "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Wall -Wextra -Werror \
    -fsanitize=thread -I "$test_root/tests/fakes/mqtt-ota" \
    -I "$test_root/tests/fakes" -I "$test_root/components/device_protocol/include" \
    "$test_root/components/device_protocol/mqtt_ota_stream.c" \
    "$test_root/components/device_protocol/mqtt_ota_transfer.c" \
    "$test_root/tests/mqtt_ota_transfer_test.c" -o "$build_dir/mqtt_ota_transfer_tsan"
  "$build_dir/mqtt_ota_transfer_tsan"
fi
