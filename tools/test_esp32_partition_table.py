#!/usr/bin/env python3
"""固定 SDK 核对 ESP32 原生业务双应用与旧 AT 原字节区。"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"


ROOT = Path(__file__).resolve().parents[1]
CSV = ROOT / "firmware/partitions/esp32-partition-table.csv"
EXPECTED = {
    "nvs": (1, 2, 0x9000, 0x6000, 0),
    "phy_init": (1, 1, 0xf000, 0x1000, 0),
    "otadata": (1, 0, 0x10000, 0x2000, 0),
    "coredump": (1, 3, 0x12000, 0xe000, 0),
    "ota_0": (0, 0x10, 0x20000, 0x1e0000, 0),
    "ota_1": (0, 0x11, 0x200000, 0x1e0000, 0),
    "at_old_raw": (1, 0x06, 0x3e5000, 0x5000, 2),
    "frp_scratch": (1, 0x06, 0x3ea000, 0x10000, 0),
    "base_store": (1, 2, 0x3fa000, 0x6000, 0),
}


class PartitionTests(unittest.TestCase):
    def test_official_v1_4mib_geometry_and_preserved_at(self) -> None:
        idf = Path(os.environ["IDF_PATH"])
        generator = idf / "components/partition_table/gen_esp32part.py"
        self.assertTrue(generator.is_file())
        with tempfile.TemporaryDirectory(prefix="esp-base-esp32-partitions-") as directory:
            output = Path(directory) / "partition-table.bin"
            subprocess.run([sys.executable, str(generator), "--quiet", "--offset", "0x8000",
                            "--flash-size", "4MB", "--secure", "v1", str(CSV), str(output)],
                           check=True, capture_output=True)
            table = output.read_bytes()
        entries = {}
        for at in range(0, len(table), 32):
            raw = table[at:at + 32]
            if raw == b"\xff" * 32 or raw[:2] != b"\xaa\x50":
                break
            label = raw[12:28].split(b"\0", 1)[0].decode("ascii")
            self.assertNotIn(label, entries)
            entries[label] = (raw[2], raw[3], int.from_bytes(raw[4:8], "little"),
                              int.from_bytes(raw[8:12], "little"),
                              int.from_bytes(raw[28:32], "little"))
        self.assertEqual(entries, EXPECTED)
        self.assertEqual(EXPECTED["ota_0"][2] + EXPECTED["ota_0"][3], EXPECTED["ota_1"][2])
        self.assertNotIn("product_pkgs", entries)
        self.assertEqual(EXPECTED["frp_scratch"][2], EXPECTED["at_old_raw"][2] + EXPECTED["at_old_raw"][3])
        self.assertEqual(EXPECTED["base_store"][2], EXPECTED["frp_scratch"][2] + EXPECTED["frp_scratch"][3])
        self.assertEqual(EXPECTED["base_store"][2] + EXPECTED["base_store"][3], 0x400000)


if __name__ == "__main__":
    unittest.main()
