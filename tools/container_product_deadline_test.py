"""Sign an ABI 2 deadline guest and run Base's real product boot host test."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa


def main() -> None:
    binary, container, wasi_sdk, guests = map(Path, sys.argv[1:])
    base = Path(__file__).resolve().parents[1] / "firmware"
    sys.path.insert(0, str(container / "tests"))
    sys.path.insert(0, str(container / "tools"))
    from package_wasm_test import signed_package  # type: ignore[import-not-found]
    import counter_guest  # type: ignore[import-not-found]

    spec = json.loads((container / "examples/counter/spec.example.json").read_text())
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        guest = temporary / "deadline.wasm"
        command = [
            str(wasi_sdk / "bin/clang"), "--target=wasm32-unknown-unknown",
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-nostdlib",
            "-ffreestanding", "-fno-builtin", "-fno-exceptions",
            "-fno-stack-protector",
            *(f"-mno-{feature}" for feature in counter_guest.FEATURES_OFF),
            "-I", str(container / "guest-sdk/include"),
            str(base / "tests/container_product_deadline_guest.c"),
            str(container / "guest-sdk/src/econtainer_guest.c"),
            "-Wl,--no-entry", "-Wl,--allow-undefined",
            "-Wl,--export=econtainer_event_buffer",
            *(f"-Wl,--export={export}" for export in counter_guest.EXPORT_TYPES),
            f"-Wl,--initial-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,--max-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,-z,stack-size={counter_guest.STACK_BYTES}",
            "-o", str(guest),
        ]
        subprocess.run(command, check=True)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = temporary / "private.pem"
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        (temporary / "public.der").write_bytes(key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        normal = (guests / "counter.wasm").read_bytes()
        (temporary / "normal.pkg").write_bytes(signed_package(private, normal, spec))
        # A fresh PSS salt changes the complete signed package identity while
        # keeping the exact product/version and Wasm bytes unchanged.
        identity = signed_package(private, normal, spec)
        assert identity != (temporary / "normal.pkg").read_bytes()
        (temporary / "identity.pkg").write_bytes(identity)
        newer = copy.deepcopy(spec)
        newer["product_version"] = "v0-1-3"
        (temporary / "newer.pkg").write_bytes(signed_package(private, normal, newer))
        (temporary / "normal-version.txt").write_text(spec["product_version"], encoding="ascii")
        (temporary / "newer-version.txt").write_text(newer["product_version"], encoding="ascii")
        subprocess.run([str(binary), str(temporary), "identity"], check=True)
        expired = copy.deepcopy(spec)
        expired["product_version"] = "v0-1-1"
        expired["required_capabilities"] = ["log", "timer"]
        expired["limits"]["instruction_budget"] = 100000000
        (temporary / "deadline.pkg").write_bytes(signed_package(
            private, guest.read_bytes(), expired))
        subprocess.run([str(binary), str(temporary), "deadline"], check=True)
        event_failed = copy.deepcopy(spec)
        event_failed["product_version"] = "v0-1-2"
        (temporary / "event-loop.pkg").write_bytes(signed_package(
            private, (guests / "event-loop.wasm").read_bytes(), event_failed))
        subprocess.run([str(binary), str(temporary), "event-failure"], check=True)


if __name__ == "__main__":
    main()
