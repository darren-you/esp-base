"""用真实签名 guest 验证 Base 定时事件业务结果，不触达设备。"""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa


def main() -> None:
    binary, container, wasi_sdk = map(Path, sys.argv[1:])
    base = Path(__file__).resolve().parents[1] / "firmware"
    sys.path.insert(0, str(container / "tests"))
    sys.path.insert(0, str(container / "tools"))
    from package_wasm_test import signed_package  # type: ignore[import-not-found]
    import counter_guest  # type: ignore[import-not-found]

    spec = json.loads((container / "examples/counter/spec.example.json").read_text())
    spec["required_capabilities"] = ["timer"]
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = temporary / "private.pem"
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        (temporary / "public.der").write_bytes(key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        for name, result in (("timer-negative", -7), ("timer-zero", 0), ("timer-positive", 3)):
            guest = temporary / f"{name}.wasm"
            command = [
                str(wasi_sdk / "bin/clang"), "--target=wasm32-unknown-unknown",
                "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-nostdlib",
                "-ffreestanding", "-fno-builtin", "-fno-exceptions",
                "-fno-stack-protector",
                *(f"-mno-{feature}" for feature in counter_guest.FEATURES_OFF),
                f"-DECONTAINER_TIMER_BUSINESS_RESULT={result}",
                "-I", str(container / "guest-sdk/include"),
                str(base / "tests/container_product_timer_business_guest.c"),
                str(container / "guest-sdk/src/econtainer_guest.c"),
                "-Wl,--no-entry", "-Wl,--allow-undefined",
                "-Wl,--export=econtainer_event_buffer",
                *(f"-Wl,--export={export}" for export in counter_guest.EXPORT_TYPES),
                f"-Wl,--initial-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
                f"-Wl,--max-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
                f"-Wl,-z,stack-size={counter_guest.STACK_BYTES}", "-o", str(guest),
            ]
            subprocess.run(command, check=True)
            variant = copy.deepcopy(spec)
            variant["product_version"] = f"v0-3-{name}"
            package = signed_package(private, guest.read_bytes(), variant)
            (temporary / f"{name}.pkg").write_bytes(package)
            print(f"timer_business_fixture: result={result} "
                  f"wasm_sha256={hashlib.sha256(guest.read_bytes()).hexdigest()} "
                  f"package_sha256={hashlib.sha256(package).hexdigest()}", flush=True)
        subprocess.run([str(binary), str(temporary), "timer-business"],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
