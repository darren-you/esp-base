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
  -I "$ROOT/components/device_protocol/include" -I "$ROOT/components/remote_config/include" -I "$CJSON_DIR" \
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
  -I "$ROOT/components/mqtt_runtime/include" \
  "$ROOT/components/mqtt_runtime/mqtt_contract.c" "$ROOT/tests/mqtt_contract_test.c" \
  -o "$BUILD_DIR/mqtt_contract_test"
"$BUILD_DIR/mqtt_contract_test"
MQTT_DIR="$ROOT/managed_components/espressif__mqtt"
if [[ ! -f "$MQTT_DIR/include/mqtt_client.h" ]]; then
  printf '  error  Run idf.py -C firmware reconfigure to resolve the locked MQTT dependency.\n' >&2
  exit 1
fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DCONFIG_MQTT_REPORT_DELETED_MESSAGES=1 -DCONFIG_EBASE_MQTT_PLAINTEXT_LAB=1 \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/mqtt_runtime/include" -I "$MQTT_DIR/include" \
  "$ROOT/components/mqtt_runtime/mqtt_contract.c" "$ROOT/components/mqtt_runtime/esp_base_mqtt.c" \
  "$ROOT/tests/mqtt_runtime_test.c" -o "$BUILD_DIR/mqtt_runtime_test"
"$BUILD_DIR/mqtt_runtime_test"
