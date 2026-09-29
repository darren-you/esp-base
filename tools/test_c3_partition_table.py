#!/usr/bin/env python3
"""用固定 SDK 生成并核对 C3 产品分区与三份业务包槽。"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CSV = ROOT / "firmware/partitions/c3-product-partition-table.csv"
EXPECTED = {
    "nvs": (1, 2, 0x9000, 0x6000, 0),
    "otadata": (1, 0, 0xf000, 0x2000, 0),
    "phy_init": (1, 1, 0x11000, 0x1000, 0),
    "coredump": (1, 3, 0x12000, 0xe000, 0),
    "ota_0": (0, 0x10, 0x20000, 0x130000, 0),
    "ota_1": (0, 0x11, 0x150000, 0x130000, 0),
    "product_pkgs": (1, 0x06, 0x280000, 0x165000, 0),
    "frp_scratch": (1, 0x06, 0x3e5000, 0x10000, 0),
    "base_store": (1, 2, 0x3f5000, 0xb000, 0),
}


class PartitionTests(unittest.TestCase):
    def test_official_v2_4mib_geometry_and_package_capacity(self) -> None:
        idf = Path(os.environ["IDF_PATH"])
        generator = idf / "components/partition_table/gen_esp32part.py"
        self.assertTrue(generator.is_file())
        with tempfile.TemporaryDirectory(prefix="esp-base-c3-partitions-") as directory:
            output = Path(directory) / "partition-table.bin"
            subprocess.run([sys.executable, str(generator), "--quiet", "--offset", "0x8000",
                            "--flash-size", "4MB", "--secure", "v2", str(CSV), str(output)],
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
        self.assertEqual(EXPECTED["product_pkgs"][3], 3 * 0x77000)
        self.assertEqual(EXPECTED["frp_scratch"][2],
                         EXPECTED["product_pkgs"][2] + EXPECTED["product_pkgs"][3])
        self.assertEqual(EXPECTED["base_store"][2],
                         EXPECTED["frp_scratch"][2] + EXPECTED["frp_scratch"][3])
        self.assertEqual(EXPECTED["base_store"][2] + EXPECTED["base_store"][3], 0x400000)


if __name__ == "__main__":
    unittest.main()
