#!/usr/bin/env python3
"""使用固定 SDK 官方生成器检验只读迁移预检的通过与阻断。"""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
PREFLIGHT = ROOT / "tools/preflight_v2_migration.py"
DEVICE_ID = "00000000-0000-4000-8000-000000000001"


class PreflightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.idf = Path(os.environ["IDF_PATH"])
        cls.generator = cls.idf / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        cls.partitions = cls.idf / "components/partition_table/gen_esp32part.py"
        cls.temp = tempfile.TemporaryDirectory(prefix="esp-base-v2-preflight-test-")
        cls.work = Path(cls.temp.name)
        cls.config = bytearray(112)
        cls.config[:5] = b"EBCF\x01"
        cls.table = cls.work / "partition-table.bin"
        subprocess.run([sys.executable, str(cls.partitions), "--quiet", "--offset", "0x8000",
                        "--flash-size", "4MB", str(ROOT / "firmware/partitions/partition_table.csv"),
                        str(cls.table)], check=True, capture_output=True)
        cls.identity = cls.make_nvs("identity", 0x6000,
                                    f"base_identity,namespace,,\ndevice_uuid,data,string,{DEVICE_ID}\n")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    @classmethod
    def make_nvs(cls, stem: str, size: int, records: str) -> bytes:
        csv = cls.work / f"{stem}.csv"
        image = cls.work / f"{stem}.bin"
        csv.write_text("key,type,encoding,value\n" + records)
        subprocess.run([sys.executable, str(cls.generator), "generate", str(csv), str(image), hex(size)],
                       check=True, capture_output=True)
        return image.read_bytes()

    def store(self, *, version: int = 1, extra: str = "", receipt: bytes | None = None,
              config: bytes | None = None) -> bytes:
        config_bytes = bytearray(self.config if config is None else config)
        config_bytes[4] = version
        if receipt is not None:
            extra += "base_ota,namespace,,\noperation,data,base64," + base64.b64encode(receipt).decode() + "\n"
        return self.make_nvs("store", 0x20000,
                             "base_config,namespace,,\ncommitted,data,base64," +
                             base64.b64encode(config_bytes).decode() + "\n" + extra)

    def full_flash(self, *, store: bytes | None = None) -> bytearray:
        flash = bytearray(b"\xff" * 0x400000)
        table = self.table.read_bytes()
        flash[0x8000:0x8000 + len(table)] = table
        flash[0x9000:0xF000] = self.identity
        flash[0x3E0000:0x400000] = self.store() if store is None else store
        flash[0x20000] = 0xE9
        selector = bytearray(b"\xff" * 32)
        selector[:4] = (1).to_bytes(4, "little")
        selector[24:28] = (2).to_bytes(4, "little")
        selector[28:32] = zlib.crc32(selector[:4], 0xFFFFFFFF).to_bytes(4, "little")
        flash[0xF000:0xF020] = selector
        return flash

    def run_preflight(self, first: bytes, second: bytes | None = None,
                      candidate: Path | None = None) -> subprocess.CompletedProcess[str]:
        a, b = self.work / "first-flash.bin", self.work / "second-flash.bin"
        for path, data in ((a, first), (b, first if second is None else second)):
            path.write_bytes(data)
            path.chmod(0o600)
        command = [sys.executable, str(PREFLIGHT), "--backup-a", str(a), "--backup-b", str(b),
                   "--idf-path", str(self.idf), "--device-id", DEVICE_ID]
        if candidate is not None:
            command += ["--output-base-store", str(candidate)]
        return subprocess.run(command,
                              text=True, capture_output=True, check=False)

    def test_official_v1_image_passes_without_exposing_values(self) -> None:
        result = self.run_preflight(self.full_flash())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("EBCF v1, 112B", result.stdout)
        self.assertIn("ota_0: image-header", result.stdout)
        self.assertNotIn(DEVICE_ID, result.stdout)

    def test_mismatched_full_backup_blocks(self) -> None:
        first = self.full_flash()
        second = bytearray(first)
        second[0x20040] ^= 1
        result = self.run_preflight(first, second)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("不一致", result.stderr)

    def test_unknown_nvs_key_blocks(self) -> None:
        store = self.store(extra="unexpected,data,u8,1\n")
        result = self.run_preflight(self.full_flash(store=store))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("非预期记录", result.stderr)

    def test_corrupted_nvs_data_blocks(self) -> None:
        flash = self.full_flash()
        location = flash.find(b"EBCF", 0x3E0000)
        self.assertGreaterEqual(location, 0)
        flash[location] ^= 1
        result = self.run_preflight(flash)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CRC", result.stderr)

    def test_v2_blob_is_not_misread_as_v1(self) -> None:
        result = self.run_preflight(self.full_flash(store=self.store(version=2)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("不是规范 EBCF v1", result.stderr)

    def test_pending_otadata_blocks(self) -> None:
        flash = self.full_flash()
        flash[0xF000 + 24:0xF000 + 28] = (1).to_bytes(4, "little")
        result = self.run_preflight(flash)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("未决槽", result.stderr)

    def test_candidate_round_trips_wifi_and_ota_receipt(self) -> None:
        config = bytearray(self.config)
        config[5:8] = bytes((1, 8, 8))
        config[8:12] = (42).to_bytes(4, "little")
        config[16:24] = b"testwifi"
        config[48:56] = b"password"
        receipt = bytearray(118)
        receipt[:5] = b"EOTA\x01"
        receipt[5:9] = bytes((2, 0x10, 0x11, 6))
        receipt[10:14] = (0x10000).to_bytes(4, "little")
        receipt[14:50] = DEVICE_ID.encode()
        receipt[50:86] = b"00000000-0000-4000-8000-000000000002"
        receipt[86:118] = bytes(range(32))
        output = self.work / "candidate-with-receipt.bin"
        result = self.run_preflight(self.full_flash(store=self.store(config=config, receipt=receipt)),
                                    candidate=output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.stat().st_size, 0x20000)
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        parser = self.idf / "components/nvs_flash/nvs_partition_tool/nvs_tool.py"
        inspect = subprocess.run([sys.executable, str(parser), "-f", "json", "-d", "minimal", str(output)],
                                 text=True, capture_output=True, check=True)
        records = {(record["namespace"], record["key"]): base64.b64decode(record["data"])
                   for record in json.loads(inspect.stdout)}
        expected_v2 = (b"EBCF\x02\x01\x08\x08" + (42).to_bytes(4, "little") + bytes(12) +
                       b"testwifipassword")
        self.assertEqual(records[("base_config", "committed")], expected_v2)
        self.assertEqual(records[("base_ota", "operation")], receipt)

    def test_candidate_never_overwrites(self) -> None:
        output = self.work / "existing-candidate.bin"
        output.write_bytes(b"keep")
        result = self.run_preflight(self.full_flash(), candidate=output)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
