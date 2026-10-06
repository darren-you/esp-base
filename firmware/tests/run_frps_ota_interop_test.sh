#!/usr/bin/env bash
set -euo pipefail
base_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$base_root/tools/frps_ota_interop.py" "$@"
