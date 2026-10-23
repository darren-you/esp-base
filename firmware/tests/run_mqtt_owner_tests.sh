#!/usr/bin/env bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
components_dir="${ESP_BASE_TEST_COMPONENTS_DIR:-$test_root/managed_components}"
command -v pkg-config >/dev/null && pkg-config --exists openssl || {
  printf 'MQTT owner tests require OpenSSL and pkg-config.\n' >&2; exit 1;
}
[[ -f "$components_dir/mqtt/runtime/include/emqtt.h" ]] || {
  printf 'Resolve the locked mqtt dependency before running MQTT owner tests.\n' >&2; exit 1;
}
build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
read -r -a openssl_cflags <<< "$(pkg-config --cflags openssl)"
read -r -a openssl_libs <<< "$(pkg-config --libs openssl)"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "${openssl_cflags[@]}" -I "$test_root/tests/fakes/network-auth" \
  -I "$test_root/tests/fakes" -I "$test_root/components/device_protocol/include" \
  -I "$test_root/components/remote_config/include" \
  -I "$components_dir/mqtt/runtime/include" \
  "$test_root/components/device_protocol/mqtt_owner.c" \
  "$test_root/components/device_protocol/mqtt_command.c" \
  "$test_root/components/device_protocol/mqtt_event.c" \
  "$test_root/components/device_protocol/mqtt_ota.c" \
  "$test_root/components/device_protocol/network_auth.c" \
  "$test_root/components/device_protocol/command_guard.c" \
  "$components_dir/mqtt/runtime/emqtt_contract.c" \
  "$test_root/tests/mqtt_owner_test.c" "${openssl_libs[@]}" -o "$build_dir/mqtt_owner_test"
"$build_dir/mqtt_owner_test"
