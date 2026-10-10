#!/usr/bin/env python3
"""独立进程核对第一方 SDK 消费入口；临时 SDK 工具只探测缓存与环境。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parent
SDK_PROBE = '''import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent))
import sdk_cache_dependency

def report():
    child = subprocess.run([sys.executable, "-c",
        "import json, os, sys; sys.path.insert(0, sys.argv[1]); "
        "import sdk_cache_dependency; print(json.dumps({"
        "'environment': os.environ.get('PYTHONDONTWRITEBYTECODE'), "
        "'dont_write_bytecode': sys.dont_write_bytecode}))",
        str(Path(__file__).parent)], check=True, capture_output=True, text=True)
    Path(os.environ["SDK_CACHE_REPORT"]).write_text(json.dumps({
        "environment": os.environ.get("PYTHONDONTWRITEBYTECODE"),
        "dont_write_bytecode": sys.dont_write_bytecode,
        "child": json.loads(child.stdout),
    }))

if __name__ == "__main__":
    report()
    Path(sys.argv[-1]).write_bytes(b"temporary-partition-probe")
elif __name__ == "nvs_parser":
    report()
'''


class SdkPythonCacheTests(unittest.TestCase):
    def run_entry(self, entry: str, action: str, relative: str) -> None:
        with tempfile.TemporaryDirectory(prefix="esp-base-python-cache-") as directory:
            work = Path(directory)
            components = work / "components"
            probe = components / relative
            probe.parent.mkdir(parents=True)
            probe.write_text(SDK_PROBE)
            (probe.parent / "sdk_cache_dependency.py").write_text("VALUE = 1\n")
            report = work / "report.json"
            environment = os.environ.copy()
            for key in ("PYTHONDONTWRITEBYTECODE", "PYTHONPYCACHEPREFIX", "PYTHONPATH"):
                environment.pop(key, None)
            environment["SDK_CACHE_REPORT"] = str(report)
            code = (
                "import pathlib, runpy, subprocess, sys; "
                "sys.path.insert(0, sys.argv[1]); "
                "entry = runpy.run_path(sys.argv[2]); "
                "components = pathlib.Path(sys.argv[3]); " + action
            )
            result = subprocess.run([sys.executable, "-c", code, str(TOOLS),
                                     str(TOOLS / entry), str(components), str(probe)],
                                    env=environment, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            observed = json.loads(report.read_text())
            self.assertEqual(observed["environment"], "1", json.dumps({
                "observed": observed,
                "sdk_bytecode": [str(path.relative_to(components))
                                 for path in components.rglob("*.pyc")],
            }))
            self.assertTrue(observed["dont_write_bytecode"])
            self.assertEqual(observed["child"], {
                "environment": "1", "dont_write_bytecode": True,
            })
            self.assertEqual(list(components.rglob("*.pyc")), [])
            self.assertEqual(list(components.rglob("__pycache__")), [])

    def test_native_prepare_import_and_sdk_subprocess(self) -> None:
        self.run_entry("prepare_native_layout.py",
                       "entry['load_partition_module'](components); "
                       "assert entry['table_from_csv'](components / 'input.csv', components) "
                       "== b'temporary-partition-probe'",
                       "partition_table/gen_esp32part.py")

    def test_preflight_direct_sdk_import(self) -> None:
        self.run_entry("preflight_v3_migration.py",
                       "entry['load_nvs_parser'](components)",
                       "nvs_flash/nvs_partition_tool/nvs_parser.py")

    def test_existing_source_test_entrypoints_inherit_environment(self) -> None:
        for entry in ("test_c3_partition_table.py", "test_esp32_partition_table.py",
                      "test_preflight_v3_migration.py", "test_prepare_native_layout.py"):
            with self.subTest(entry=entry):
                self.run_entry(entry,
                               "subprocess.run([sys.executable, sys.argv[4], "
                               "str(components / 'output.bin')], check=True)",
                               "partition_table/gen_esp32part.py")


if __name__ == "__main__":
    unittest.main()
