#!/usr/bin/env bash
# host 测试：WASM 宿主核心用真实应用跑通宿主接口和各类故障。需要 cmake；WAMR 取自 Component Manager 已解析的锁定提交。
# 经典与快速解释器各跑一遍：FAST=1 只跑快速解释器，FAST=0 只跑经典解释器。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WAMR="$ROOT/managed_components/wasm-micro-runtime"
if [[ ! -f "$WAMR/build-scripts/runtime_lib.cmake" ]]; then
  echo "先运行一次 idf.py reconfigure（或 tools/esp_base.py build），让 Component Manager 取回锁定的 WAMR。" >&2
  exit 1
fi
OUT="$(mktemp -d)"
trap 'rm -rf -- "$OUT"' EXIT
for fast in ${FAST:-0 1}; do
  cmake -S "$ROOT/tests/wasm_host" -B "$OUT/$fast" -DWAMR_ROOT_DIR="$WAMR" -DWAMR_BUILD_FAST_INTERP="$fast" >"$OUT/cmake-$fast.log" 2>&1 \
    || { cat "$OUT/cmake-$fast.log" >&2; exit 1; }
  cmake --build "$OUT/$fast" -j 8 >"$OUT/build-$fast.log" 2>&1 || { tail -40 "$OUT/build-$fast.log" >&2; exit 1; }
  "$OUT/$fast/wasm_host_test" "$ROOT/tests/apps" "$([[ $fast == 1 ]] && echo fast || echo classic)"
done
