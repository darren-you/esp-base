#!/usr/bin/env python3
"""从两份完整 ESP-AT Flash 备份生成可重建旧持久区的 16 KiB 仓外归档。"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import stat
import sys


ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x400000
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
SECTOR_SIZE = 0x1000
PRESERVED_SIZE = 2 * SECTOR_SIZE
OLD_REGIONS = (("nvs", 0x12000, 0xe000), ("at_customize", 0x20000, 0xe0000))
ARCHIVE_SIZE = len(OLD_REGIONS) * PRESERVED_SIZE


class ArchiveError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ArchiveError(message)


def read_backup(path: Path) -> tuple[bytes, os.stat_result]:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode), "备份必须是普通文件")
        require(before.st_uid == os.getuid() and before.st_mode & 0o177 == 0,
                "备份必须归当前用户所有且权限为 0600 或更严")
        require(before.st_size == FLASH_SIZE, "备份必须是完整 4 MiB Flash")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            contents = stream.read(FLASH_SIZE + 1)
        after = os.fstat(descriptor)
        require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), "读取期间备份发生变化")
        require(len(contents) == FLASH_SIZE, "备份长度变化")
        return contents, before
    finally:
        os.close(descriptor)


def check_old_table(flash: bytes) -> None:
    entries: dict[str, tuple[int, int]] = {}
    table = flash[TABLE_OFFSET:TABLE_OFFSET + TABLE_SIZE]
    for offset in range(0, TABLE_SIZE, 32):
        raw = table[offset:offset + 32]
        if raw == b"\xff" * 32:
            break
        require(raw[:2] == b"\xaa\x50", "旧 ESP-AT 分区表含非标准条目")
        label = raw[12:28].split(b"\0", 1)[0]
        try:
            name = label.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ArchiveError("旧 ESP-AT 分区标签不是 ASCII") from exc
        require(name and name not in entries, "旧 ESP-AT 分区标签为空或重复")
        entries[name] = (int.from_bytes(raw[4:8], "little"),
                         int.from_bytes(raw[8:12], "little"))
    for name, address, size in OLD_REGIONS:
        require(entries.get(name) == (address, size), f"旧 {name} 分区与已核对布局不符")


def make_archive(flash: bytes) -> bytes:
    check_old_table(flash)
    parts = []
    for name, address, size in OLD_REGIONS:
        original = flash[address:address + size]
        require(original[PRESERVED_SIZE:] == b"\xff" * (size - PRESERVED_SIZE),
                f"旧 {name} 前两页以外存在非空字节，16 KiB 归档不能无损保存")
        parts.append(original[:PRESERVED_SIZE])
    archive = b"".join(parts)
    require(len(archive) == ARCHIVE_SIZE, "归档长度不符")
    for index, (name, address, size) in enumerate(OLD_REGIONS):
        original = flash[address:address + size]
        rebuilt = archive[index * PRESERVED_SIZE:(index + 1) * PRESERVED_SIZE] + \
                  b"\xff" * (size - PRESERVED_SIZE)
        require(rebuilt == original and hashlib.sha256(rebuilt).digest() ==
                hashlib.sha256(original).digest(), f"旧 {name} 完整分区重建不符")
    return archive


def write_archive(destination: Path, archive: bytes) -> None:
    parent = destination.parent.resolve(strict=True)
    parent_info = parent.stat()
    require(stat.S_ISDIR(parent_info.st_mode) and parent_info.st_uid == os.getuid() and
            stat.S_IMODE(parent_info.st_mode) & 0o077 == 0,
            "归档父目录必须归当前用户所有且对组和其他用户关闭")
    resolved = parent / destination.name
    require(not resolved.is_relative_to(ROOT), "归档只能写在仓库外")
    require(not any((ancestor / ".git").exists() for ancestor in (parent, *parent.parents)),
            "归档只能写在 Git 仓库外")
    mode = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(resolved, mode, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(archive)
            stream.flush()
            os.fsync(descriptor)
        require(os.fstat(descriptor).st_size == ARCHIVE_SIZE, "归档写入长度不符")
    except Exception:
        os.unlink(resolved)
        raise
    finally:
        os.close(descriptor)
    try:
        check, _ = read_archive(resolved)
        require(check == archive and hashlib.sha256(check).digest() ==
                hashlib.sha256(archive).digest(), "归档落盘读回不符")
    except Exception:
        os.unlink(resolved)
        raise


def read_archive(path: Path) -> tuple[bytes, os.stat_result]:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(descriptor)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and
                stat.S_IMODE(info.st_mode) == 0o600 and info.st_size == ARCHIVE_SIZE,
                "归档权限或长度不符")
        return os.read(descriptor, ARCHIVE_SIZE + 1), info
    finally:
        os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup-a", type=Path, required=True)
    parser.add_argument("--backup-b", type=Path, required=True)
    parser.add_argument("--output-at-old-raw", type=Path, required=True)
    args = parser.parse_args()
    try:
        first, first_stat = read_backup(args.backup_a)
        second, second_stat = read_backup(args.backup_b)
        require((first_stat.st_dev, first_stat.st_ino) !=
                (second_stat.st_dev, second_stat.st_ino), "必须提供两个独立备份文件")
        require(first == second and hashlib.sha256(first).digest() ==
                hashlib.sha256(second).digest(), "两份完整 Flash 备份的字节或 SHA-256 不一致")
        archive = make_archive(first)
        write_archive(args.output_at_old_raw, archive)
    except (ArchiveError, OSError) as exc:
        print(f"ESP32 旧 AT 归档阻断：{exc}", file=sys.stderr)
        return 1
    print("ESP32 旧 AT 归档通过：两份完整备份一致，旧 NVS/at_customize 完整分区逐字节及 SHA-256 可重建；已写入仓外 0600 归档。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
