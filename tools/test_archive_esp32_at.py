#!/usr/bin/env python3
"""ESP32 旧 AT 归档的合成 Flash 正反例，不使用真实备份。"""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/archive_esp32_at.py"
spec = importlib.util.spec_from_file_location("archive_esp32_at", TOOL)
assert spec and spec.loader
archive_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive_tool)


class ArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="esp32-at-archive-test-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.first = self.work / "first.bin"
        self.second = self.work / "second.bin"
        self.output = self.work / "at-old-raw.bin"
        self.flash = self.make_flash()

    @staticmethod
    def make_flash() -> bytearray:
        flash = bytearray(b"\xff" * archive_tool.FLASH_SIZE)
        for index, (name, address, size) in enumerate(archive_tool.OLD_REGIONS):
            entry = bytearray(b"\0" * 32)
            entry[:2] = b"\xaa\x50"
            entry[2] = 1
            entry[4:8] = address.to_bytes(4, "little")
            entry[8:12] = size.to_bytes(4, "little")
            entry[12:12 + len(name)] = name.encode("ascii")
            entry[28:32] = b"\xff" * 4
            at = archive_tool.TABLE_OFFSET + index * 32
            flash[at:at + 32] = entry
            flash[address:address + archive_tool.PRESERVED_SIZE] = bytes([index + 1]) * archive_tool.PRESERVED_SIZE
        return flash

    def run_tool(self, *, other: bytes | None = None, mode: int = 0o600,
                 destination: Path | None = None) -> subprocess.CompletedProcess[str]:
        for path, data in ((self.first, self.flash),
                           (self.second, self.flash if other is None else other)):
            path.write_bytes(data)
            path.chmod(mode)
        return subprocess.run([sys.executable, str(TOOL), "--backup-a", str(self.first),
                               "--backup-b", str(self.second), "--output-at-old-raw",
                               str(destination or self.output)], text=True,
                              capture_output=True, check=False)

    def test_two_backups_reconstruct_complete_regions(self) -> None:
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        archive = self.output.read_bytes()
        self.assertEqual(len(archive), 0x4000)
        for index, (_, address, size) in enumerate(archive_tool.OLD_REGIONS):
            rebuilt = archive[index * 0x2000:(index + 1) * 0x2000] + b"\xff" * (size - 0x2000)
            original = bytes(self.flash[address:address + size])
            self.assertEqual(rebuilt, original)
            self.assertEqual(hashlib.sha256(rebuilt).digest(), hashlib.sha256(original).digest())
        self.assertNotIn("0101010101", result.stdout)

    def test_full_backup_mismatch_blocks_output(self) -> None:
        other = bytearray(self.flash)
        other[0x300000] = 0
        result = self.run_tool(other=other)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_same_file_or_symlink_backup_blocks_output(self) -> None:
        self.first.write_bytes(self.flash)
        self.first.chmod(0o600)
        for second in (self.first, self.second):
            with self.subTest(second=second):
                if second != self.first:
                    second.symlink_to(self.first)
                result = subprocess.run([sys.executable, str(TOOL), "--backup-a", str(self.first),
                                         "--backup-b", str(second), "--output-at-old-raw",
                                         str(self.output)], text=True, capture_output=True, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_nonempty_tail_blocks_output(self) -> None:
        for _, address, _ in archive_tool.OLD_REGIONS:
            with self.subTest(address=address):
                original = self.flash[address + 0x2000]
                self.flash[address + 0x2000] = 0
                result = self.run_tool()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())
                self.flash[address + 0x2000] = original

    def test_wrong_table_blocks_output(self) -> None:
        self.flash[archive_tool.TABLE_OFFSET + 4] ^= 1
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_insecure_or_truncated_backup_blocks_output(self) -> None:
        self.assertNotEqual(self.run_tool(mode=0o644).returncode, 0)
        self.assertFalse(self.output.exists())
        self.flash.pop()
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_existing_output_blocks_without_overwrite(self) -> None:
        self.output.write_bytes(b"keep")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"keep")

    def test_symlink_output_blocks(self) -> None:
        sentinel = self.work / "sentinel"
        sentinel.write_bytes(b"keep")
        self.output.symlink_to(sentinel)
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_repository_output_blocks(self) -> None:
        destination = ROOT / "at-old-raw-never-create.bin"
        self.assertNotEqual(self.run_tool(destination=destination).returncode, 0)
        self.assertFalse(destination.exists())

    def test_public_parent_blocks(self) -> None:
        public = self.work / "public"
        public.mkdir(mode=0o755)
        self.assertNotEqual(self.run_tool(destination=public / "archive.bin").returncode, 0)
        self.assertFalse((public / "archive.bin").exists())
        link = self.work / "parent-link"
        link.symlink_to(public, target_is_directory=True)
        self.assertNotEqual(self.run_tool(destination=link / "archive.bin").returncode, 0)
        self.assertFalse((public / "archive.bin").exists())

    def test_failed_readback_removes_new_output(self) -> None:
        with mock.patch.object(archive_tool, "read_archive", side_effect=archive_tool.ArchiveError("test")):
            with self.assertRaises(archive_tool.ArchiveError):
                archive_tool.write_archive(self.output, bytes(archive_tool.ARCHIVE_SIZE))
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
