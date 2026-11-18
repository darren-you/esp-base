#!/usr/bin/env bash
# 重新生成宿主测试用的应用（firmware/tests/apps/*.wasm）。改了 SDK、样例或夹具后运行，产物随源码一起提交。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/../../firmware/tests/apps"
cd "$ROOT"
cargo build --release -p hello
cargo build --release -p fixtures --examples
mkdir -p "$OUT"
cp target/wasm32v1-none/release/hello.wasm "$OUT/hello.wasm"
for name in spin trap init_fail bad_pointer unknown_import no_event deep; do
  cp "target/wasm32v1-none/release/examples/$name.wasm" "$OUT/$name.wasm"
done
# 没有按要求链接的版本：不导出 __heap_base、__data_end，线性内存无法收缩（整页 64 KiB），宿主应当拒绝。
RUSTFLAGS="-C link-arg=-zstack-size=4096" cargo build --release -p hello --target-dir target/unshrunk
cp target/unshrunk/wasm32v1-none/release/hello.wasm "$OUT/hello_unshrunk.wasm"
ls -l "$OUT"
