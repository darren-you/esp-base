#!/usr/bin/env bash
# host 测试：只编译不依赖 ESP-IDF 的纯逻辑。cJSON 取自 Component Manager 已解析的锁定版本。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CJSON="$ROOT/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$CJSON/cJSON.c" ]]; then
  echo "先运行一次 idf.py reconfigure（或 tools/esp_base.py build），让 Component Manager 取回锁定的 cJSON。" >&2
  exit 1
fi
OUT="$(mktemp -d)"
trap 'rm -rf -- "$OUT"' EXIT
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -Wno-deprecated-declarations -I "$CJSON" -c "$CJSON/cJSON.c" -o "$OUT/cJSON.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$CJSON" -I "$ROOT/components/base_protocol/include" -I "$ROOT/components/base_ota/include" \
  -I "$ROOT/components/base_runtime/system" \
  "$ROOT/components/base_protocol/base_protocol.c" "$ROOT/components/base_ota/ota_logic.c" \
  "$ROOT/components/base_runtime/base_guard.c" \
  "$ROOT/tests/host_test.c" "$OUT/cJSON.o" -o "$OUT/host_test"
"$OUT/host_test" "$ROOT/../docs/design/device-protocol-samples.json"
# 应用包的包头与清单解析（WASM 宿主的纯逻辑）。
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$CJSON" -I "$ROOT/components/base_wasm/core" \
  "$ROOT/components/base_wasm/core/wasm_package.c" "$ROOT/tests/wasm_package_test.c" "$OUT/cJSON.o" -o "$OUT/wasm_package_test"
"$OUT/wasm_package_test" "$ROOT/tests/apps/hello.ebapp"
# 应用部署的状态机。
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$CJSON" -I "$ROOT/components/base_wasm/core" \
  "$ROOT/components/base_wasm/core/wasm_deploy.c" "$ROOT/tests/wasm_deploy_test.c" "$OUT/cJSON.o" -o "$OUT/wasm_deploy_test"
"$OUT/wasm_deploy_test" "$ROOT/../docs/design/device-protocol-samples.json"
# 应用日志的编码。
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$CJSON" -I "$ROOT/components/base_wasm/core" \
  "$ROOT/components/base_wasm/core/wasm_log.c" "$ROOT/tests/wasm_log_test.c" "$OUT/cJSON.o" -o "$OUT/wasm_log_test"
"$OUT/wasm_log_test" "$ROOT/../docs/design/device-protocol-samples.json"
