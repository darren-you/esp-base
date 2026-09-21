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
printf 'esp_base host tests\n  command_guard  passed\n  hardware       not used\n'
