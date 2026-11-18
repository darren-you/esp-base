#!/usr/bin/env bash
# 重新生成 host 测试用的应用和样例应用包（firmware/tests/apps/）。改了 SDK、样例或夹具后运行，产物随源码一起提交。
# 需要 rustup，以及已导出的官方 ESP-IDF 环境（给样例应用包签名用 espsecure）。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/../../firmware/tests/apps"
cd "$ROOT"
cargo build --release -p hello
cargo build --release -p fixtures --examples
mkdir -p "$OUT"
cp target/wasm32v1-none/release/hello.wasm "$OUT/hello.wasm"
for name in spin trap init_fail bad_pointer unknown_import no_event deep trial_fault io pins; do
  cp "target/wasm32v1-none/release/examples/$name.wasm" "$OUT/$name.wasm"
done
# 没有按要求链接的版本：不导出 __heap_base、__data_end，线性内存无法收缩（整页 64 KiB），宿主应当拒绝。
RUSTFLAGS="-C link-arg=-zstack-size=4096" cargo build --release -p hello --target-dir target/unshrunk
cp target/unshrunk/wasm32v1-none/release/hello.wasm "$OUT/hello_unshrunk.wasm"
# 签过名的样例应用包：打包工具与设备、Server 对格式的理解靠它对齐。签名用一把用完即弃的键，只为让布局完整
# （设备只认自己固件的那把键）；需要先导出官方 ESP-IDF 环境（espsecure）。
python3 "$ROOT/../../tools/esp_base.py" app build "$ROOT/examples/hello" >/dev/null
UNSIGNED="$ROOT/target/esp-base/hello.ebapp"
# 签名每次都不同（键是临时的，RSA-PSS 带随机盐）：包的内容没变就保留现有的样例，不制造无意义的改动。
if [[ -f "$OUT/hello.ebapp" ]] && head -c "$(wc -c < "$UNSIGNED")" "$OUT/hello.ebapp" | cmp -s - "$UNSIGNED"; then
  echo "hello.ebapp 的内容没有变化，保留现有的样例"
else
  KEY_DIR="$(mktemp -d)"
  trap 'rm -rf -- "$KEY_DIR"' EXIT
  python3 -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY_DIR/key.pem" >/dev/null
  python3 "$ROOT/../../tools/esp_base.py" app sign "$UNSIGNED" --key-file "$KEY_DIR/key.pem" >/dev/null
  cp "$ROOT/target/esp-base/hello-signed.ebapp" "$OUT/hello.ebapp"
fi
ls -l "$OUT"
