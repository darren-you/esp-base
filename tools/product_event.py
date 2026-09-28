#!/usr/bin/env python3
"""生成 ESP Base 独立 MQTT 业务事件的已签名二进制帧。"""

import argparse
import hashlib
import hmac
import os
import pathlib
import stat
import sys
import uuid


DOMAIN = b"esp-base-product-event-v1\n"
MAX_FRAME_BYTES = 4096
MAX_SEQUENCE = (1 << 64) - 1


def canonical_uuid(value: str) -> str:
    parsed = uuid.UUID(value)
    if parsed.version != 4 or str(parsed) != value:
        raise ValueError("设备或启动身份不是规范小写 UUID v4")
    return value


def private_key(path: pathlib.Path) -> bytes:
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        mode = os.fstat(descriptor).st_mode
        if not stat.S_ISREG(mode) or stat.S_IMODE(mode) != 0o600:
            raise ValueError("管理密钥文件须为普通文件且权限精确为 0600")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            raw = source.read(66)
    finally:
        os.close(descriptor)
    key_hex = raw.removesuffix(b"\n")
    if len(key_hex) != 64 or any(byte not in b"0123456789abcdef" for byte in key_hex):
        raise ValueError("管理密钥文件须包含 64 个小写十六进制字符")
    return bytes.fromhex(key_hex.decode("ascii"))


def event_frame(key: bytes, device_id: str, boot_id: str, package_sha256: str,
                event_sequence: int, event: bytes) -> bytes:
    canonical_uuid(device_id)
    canonical_uuid(boot_id)
    if len(key) != 32 or not any(key):
        raise ValueError("管理密钥须为非零的 32 字节")
    if (len(package_sha256) != 64 or
            any(char not in "0123456789abcdef" for char in package_sha256) or
            set(package_sha256) == {"0"}):
        raise ValueError("包摘要须为非零的 64 位小写十六进制")
    if type(event_sequence) is not int or not 1 <= event_sequence <= MAX_SEQUENCE:
        raise ValueError("事件序号须为 1..2^64-1")
    if not event:
        raise ValueError("业务事件不得为空")
    signed = (DOMAIN + device_id.encode("ascii") + boot_id.encode("ascii") +
              bytes.fromhex(package_sha256) + event_sequence.to_bytes(8, "big") + event)
    frame = hmac.new(key, signed, hashlib.sha256).hexdigest().encode("ascii") + b"\n" + signed
    if len(frame) > MAX_FRAME_BYTES:
        raise ValueError("业务事件帧超过设备 4096 字节上限")
    return frame


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--management-key-file", required=True, type=pathlib.Path)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--boot-id", required=True)
    parser.add_argument("--package-sha256", required=True)
    parser.add_argument("--event-sequence", required=True, type=int)
    parser.add_argument("--event-file", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        key = private_key(args.management_key_file)
        with args.event_file.open("rb") as source:
            event = source.read(MAX_FRAME_BYTES + 1)
        frame = event_frame(key, args.device_id, args.boot_id, args.package_sha256,
                            args.event_sequence, event)
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600)
        try:
            with os.fdopen(descriptor, "wb") as target:
                target.write(frame)
        except BaseException:
            args.output.unlink(missing_ok=True)
            raise
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"topic    esp-base/{args.device_id}/event")
    print(f"frame    {args.output}")
    print(f"bytes    {len(frame)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
