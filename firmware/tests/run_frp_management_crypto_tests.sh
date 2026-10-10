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
components_dir="${ESP_BASE_TEST_COMPONENTS_DIR:-$test_root/managed_components}"
cjson_dir="$components_dir/espressif__cjson/cJSON"
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
  -I "$test_root/tests/fakes/network-auth" -I "$test_root/tests/fakes" \
  -I "$test_root/components/device_protocol/include" -I "$test_root/components/remote_config/include" \
  -I "$test_root/components/ota_operation/include" -I "$components_dir/esp_ota/include" -I "$cjson_dir" \
  "$test_root/components/device_protocol/network_auth.c" \
  "$test_root/components/device_protocol/frp_management_listener.c" \
  "$test_root/components/device_protocol/command_decoder.c" \
  "$test_root/components/device_protocol/command_guard.c" \
  "$test_root/components/remote_config/config_codec.c" \
  "$test_root/tests/frp_management_crypto_test.c" "$build_dir/cjson.o" \
  "${openssl_libs[@]}" -lm -o "$build_dir/frp_management_crypto_test"
"$build_dir/frp_management_crypto_test"

# Link the same real Base handler used by the owner tests to the real listener
# and PSA HMAC wrapper. SDK/storage/network fakes remain explicit.
if [[ "$(uname -s)" == Darwin ]]; then
  protocol_link_gc=(-Wl,-dead_strip)
else
  protocol_link_gc=(-Wl,--gc-sections)
fi
cat > "$build_dir/psa_openssl_port.c" <<'PSA_PORT'
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include "psa/crypto.h"
#include "network_auth_openssl.inc"
PSA_PORT
for source in network_auth frp_management_listener; do
  "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=address,undefined -I "$test_root/tests/fakes/network-auth" \
    -I "$test_root/components/device_protocol/include" -I "$test_root/components/remote_config/include" \
    -c "$test_root/components/device_protocol/$source.c" -o "$build_dir/$source.o"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined "${openssl_cflags[@]}" \
  -I "$test_root/tests/fakes/network-auth" -I "$test_root/tests/fakes" \
  -c "$build_dir/psa_openssl_port.c" -o "$build_dir/psa_openssl_port.o"
"${CC:-cc}" -std=c11 -D"$target_define"=1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Debase_parse_command=ebase_parse_command_real -Debase_parse_frp_status=ebase_parse_frp_status_real \
  -I "$test_root/tests/fakes" -I "$test_root/components/device_protocol/include" \
  -I "$test_root/components/remote_config/include" -I "$test_root/components/ota_operation/include" \
  -I "$components_dir/esp_ota/include" -I "$cjson_dir" \
  -c "$test_root/components/device_protocol/command_decoder.c" -o "$build_dir/command_decoder_real.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Debase_config_encode=ebase_config_encode_real \
  -I "$test_root/tests/fakes" -I "$test_root/components/remote_config/include" \
  -c "$test_root/components/remote_config/config_codec.c" -o "$build_dir/config_codec_real.o"
"${CC:-cc}" -std=c11 -pthread -D_POSIX_C_SOURCE=200809L -D"$target_define"=1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -ffunction-sections -fdata-sections "${protocol_link_gc[@]}" "${openssl_cflags[@]}" \
  -I "$test_root/tests/fakes/protocol-path" -I "$test_root/tests/fakes/ota-update" -I "$test_root/tests/fakes" \
  -I "$test_root/components/device_protocol/include" -I "$test_root/components/device_protocol" \
  -I "$test_root/components/device_identity/include" -I "$test_root/components/remote_config/include" \
  -I "$test_root/components/wifi_runtime/include" -I "$test_root/components/time_runtime/include" \
  -I "$test_root/components/ota_operation/include" -I "$test_root/components/native_business/include" -I "$components_dir/esp_ota/include" \
  -I "$components_dir/esp_frp/include" -I "$components_dir/mqtt/runtime/include" \
  "$test_root/components/device_protocol/command_guard.c" "$test_root/components/device_protocol/control_state.c" \
  "$test_root/components/native_business/esp_base_business.c" "$test_root/components/ota_operation/esp_base_storage_owner.c" \
  "$test_root/components/device_protocol/flash_observation.c" \
  "$build_dir/command_decoder_real.o" "$build_dir/config_codec_real.o" "$build_dir/cjson.o" \
  "$build_dir/network_auth.o" "$build_dir/frp_management_listener.o" "$build_dir/psa_openssl_port.o" \
  "$test_root/tests/frp_management_owner_crypto_test.c" "${openssl_libs[@]}" -lm \
  -o "$build_dir/frp_management_owner_crypto_test"
"$build_dir/frp_management_owner_crypto_test"
