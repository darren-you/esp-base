#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf -- "$BUILD_DIR"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/components/device_protocol/include" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/tests/command_guard_test.c" -o "$BUILD_DIR/command_guard_test"
"$BUILD_DIR/command_guard_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/network_auth" -I "$ROOT/components/device_protocol/include" \
  "$ROOT/components/device_protocol/network_auth.c" \
  "$ROOT/components/device_protocol/mqtt_command.c" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/tests/network_auth_test.c" \
  -o "$BUILD_DIR/network_auth_test"
"$BUILD_DIR/network_auth_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol/include" \
  -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/managed_components/mqtt/runtime/include" \
  "$ROOT/components/device_protocol/mqtt_owner.c" \
  "$ROOT/components/device_protocol/mqtt_command.c" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/managed_components/mqtt/runtime/emqtt_contract.c" \
  "$ROOT/tests/mqtt_owner_test.c" -o "$BUILD_DIR/mqtt_owner_test"
"$BUILD_DIR/mqtt_owner_test"
CJSON_DIR="$ROOT/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$CJSON_DIR/cJSON.c" ]]; then
  printf 'esp-base host tests\n  error  Run idf.py -C firmware reconfigure to resolve the locked cJSON dependency.\n' >&2
  exit 1
fi
# Keep strict diagnostics on our sources; the locked third-party cJSON uses
# sprintf internally, which the macOS SDK marks deprecated.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -Wno-deprecated-declarations \
  -I "$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$BUILD_DIR/cJSON.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/components/device_protocol/include" -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/components/ota_runtime/include" -I "$CJSON_DIR" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/components/device_protocol/command_decoder.c" "$ROOT/components/remote_config/config_codec.c" "$BUILD_DIR/cJSON.o" \
  "$ROOT/tests/command_decoder_test.c" -lm -o "$BUILD_DIR/command_decoder_test"
printf 'esp-base host tests\n  command_guard  passed\n'
"$BUILD_DIR/command_decoder_test"
printf '  hardware       not used\n'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/remote_config/include" \
  "$ROOT/components/remote_config/config_codec.c" \
  "$ROOT/components/remote_config/esp_base_remote_config.c" \
  "$ROOT/tests/config_store_test.c" -o "$BUILD_DIR/config_store_test"
"$BUILD_DIR/config_store_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/ota_runtime/include" \
  "$ROOT/components/ota_runtime/esp_base_ota.c" "$ROOT/tests/ota_confirmation_test.c" \
  -o "$BUILD_DIR/ota_confirmation_test"
"$BUILD_DIR/ota_confirmation_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" -I "$ROOT/components/ota_runtime/include" \
  "$ROOT/components/ota_runtime/esp_base_ota_update.c" "$ROOT/tests/ota_update_test.c" \
  -o "$BUILD_DIR/ota_update_test"
"$BUILD_DIR/ota_update_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" -I "$ROOT/components/ota_runtime/include" \
  "$ROOT/components/ota_runtime/esp_base_ota_receipt.c" "$ROOT/tests/ota_receipt_test.c" \
  -o "$BUILD_DIR/ota_receipt_test"
"$BUILD_DIR/ota_receipt_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol" \
  "$ROOT/components/device_protocol/control_state.c" "$ROOT/tests/control_state_test.c" \
  -o "$BUILD_DIR/control_state_test"
"$BUILD_DIR/control_state_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/app_main" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/device_identity/include" -I "$ROOT/components/device_protocol/include" \
  -I "$ROOT/components/ota_runtime/include" -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/components/safety_runtime/include" -I "$ROOT/components/time_runtime/include" \
  "$ROOT/apps/esp_base/main/esp_base_main.c" "$ROOT/components/ota_runtime/esp_base_ota.c" \
  "$ROOT/tests/ota_startup_test.c" -o "$BUILD_DIR/ota_startup_test"
"$BUILD_DIR/ota_startup_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/time_runtime/include" \
  "$ROOT/components/time_runtime/esp_base_time.c" "$ROOT/tests/time_runtime_test.c" \
  -o "$BUILD_DIR/time_runtime_test"
"$BUILD_DIR/time_runtime_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/wifi_runtime" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/wifi_runtime/include" -I "$ROOT/components/remote_config/include" \
  "$ROOT/components/wifi_runtime/esp_base_wifi.c" "$ROOT/components/remote_config/config_codec.c" \
  "$ROOT/tests/wifi_startup_test.c" -o "$BUILD_DIR/wifi_startup_test"
for stage in {0..11}; do "$BUILD_DIR/wifi_startup_test" "$stage"; done
printf '  wifi_startup passed (SDK init faults preserve failed state and release resources)\n'
