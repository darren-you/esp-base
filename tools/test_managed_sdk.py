"""实际受管 SDK 正例与真实小 Git 隔离负例；不授设备或固件资格。"""
import os
from pathlib import Path
import tempfile
import unittest

import check_sdk as sdk
from sdk_capacity_fixture import CapacityFixture


class ManagedSDKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("IDF_PATH")
        if not value:
            raise RuntimeError("IDF_PATH 必须指向已装配的精确受管 SDK")
        cls.actual = Path(value)
        # This original entry still begins with the complete actual source graph.
        sdk.check(cls.actual)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="base-managed-real-git-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.capacity = CapacityFixture(self.root)
        self.capacity.apply()
        self.idf, self.tlsf, self.lwip = self.capacity.sdk, self.capacity.tlsf, self.capacity.lwip
        self.stamp = self.capacity.stamp
        self.original = self.capacity.original
        self.after = self.capacity.after

    def gate(self):
        result = self.capacity.check()
        if result.returncode != 0:
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            raise ValueError(result.stderr)

    def reject(self):
        with self.assertRaises(ValueError):
            self.gate()

    def test_actual_positive_and_isolated_contract(self):
        self.gate()

    def test_wrong_idf_revision(self):
        self.capacity.recipe["idf"]["revision"] = "0" * 40
        self.capacity.write_recipe(stamp=True)
        self.reject()

    def test_wrong_lwip_revision_or_dirty(self):
        actual = self.capacity.recipe["lwip"]["revision"]
        self.capacity.recipe["lwip"]["revision"] = "0" * 40
        self.capacity.write_recipe(stamp=True)
        self.reject()
        self.capacity.recipe["lwip"]["revision"] = actual
        self.capacity.write_recipe(stamp=True)
        (self.lwip / "tcp.c").write_text("int unverified_lwip;\n")
        self.reject()

    def test_wrong_tlsf_revision(self):
        self.capacity.recipe["tlsf"]["revision"] = "0" * 40
        self.capacity.write_recipe(stamp=True)
        self.reject()

    def test_staged_sdk_change(self):
        self.capacity.git(self.idf, "add", "--", "components/heap/multi_heap.c")
        self.reject()

    def test_extra_root_or_tlsf_change(self):
        extra = self.idf / "unexpected.bin"
        extra.write_bytes(b"unverified")
        self.reject()
        extra.unlink()
        (self.tlsf / "unexpected.c").write_text("int unverified;\n")
        self.reject()

    def test_partial_or_tampered_managed_source(self):
        path = "components/heap/multi_heap.c"
        file = self.idf / path
        file.write_bytes(self.original["idf", path])
        self.reject()
        file.write_bytes(self.after["idf", path] + b"\n")
        self.reject()

    def test_managed_source_symlink(self):
        file = self.tlsf / "tlsf.c"
        other = self.root / "same-bytes.c"
        other.write_bytes(file.read_bytes())
        file.unlink(); file.symlink_to(other)
        self.reject()

    def test_patch_digest(self):
        file = self.capacity.product / self.capacity.recipe["managed_patches"][0]["path"]
        file.write_bytes(file.read_bytes() + b"\n")
        self.reject()

    def test_missing_bad_or_linked_stamp(self):
        raw = self.stamp.read_bytes()
        self.stamp.unlink()
        self.reject()
        self.stamp.write_bytes(raw + b"\n")
        self.stamp.chmod(0o400)
        self.reject()
        other = self.root / "same-stamp.json"
        other.write_bytes(raw)
        self.stamp.unlink(); self.stamp.symlink_to(other)
        self.reject()

    def test_official_baseline_digest(self):
        item = next(item for declaration in self.capacity.recipe["managed_patches"]
                    if declaration["repository"] == "idf" for item in declaration["files"]
                    if item["path"] == "components/heap/multi_heap.c")
        item["before_sha256"] = "0" * 64
        self.capacity.write_recipe(stamp=True)
        self.reject()

    def test_unavailable_or_wrong_other_submodule(self):
        extra = self.capacity.extra
        moved = self.root / "unavailable-framework"
        extra.rename(moved)
        self.reject()
        moved.rename(extra)
        (extra / "extra.c").write_text("int different_other_source;\n")
        self.capacity.commit(extra)
        self.reject()


if __name__ == "__main__":
    unittest.main()
