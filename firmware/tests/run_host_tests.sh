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
  "$ROOT/components/base_protocol/base_protocol.c" "$ROOT/components/base_ota/ota_logic.c" \
  "$ROOT/tests/host_test.c" "$OUT/cJSON.o" -o "$OUT/host_test"
"$OUT/host_test" "$ROOT/../docs/design/device-protocol-samples.json"
