"""受管 SDK 门的实际正测及隔离负测；夹具不授构建或设备资格。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import check_sdk as sdk


class ManagedSDKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("IDF_PATH")
        if not value:
            raise RuntimeError("IDF_PATH 必须指向已装配的精确受管 SDK")
        cls.actual = Path(value)
        sdk.check(cls.actual)
        cls.original = {}
        cls.after = {}
        for declaration, _ in sdk.patch_inputs():
            repo = sdk.repository_paths(cls.actual)[declaration["repository"]]
            for item in declaration["files"]:
                key = declaration["repository"], item["path"]
                cls.original[key] = subprocess.check_output(["git", "-C", str(repo), "show", "HEAD:" + item["path"]])
                cls.after[key] = (repo / item["path"]).read_bytes()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.idf = self.root / "idf"
        self.tlsf = self.idf / sdk.LOCK["tlsf"]["path"]
        self.lwip = self.idf / sdk.LOCK["lwip"]["path"]
        self.lwip.mkdir(parents=True)
        self.stamp = self.idf / sdk.LOCK["derivation_stamp"]
        self.stamp.write_bytes((sdk.ROOT / "sdk-lock.json").read_bytes())
        (self.root / "sdk-lock.json").write_bytes(self.stamp.read_bytes())
        self.heads = {self.idf: sdk.LOCK["idf"]["revision"], self.tlsf: sdk.LOCK["tlsf"]["revision"],
                      self.lwip: sdk.LOCK["lwip"]["revision"]}
        self.status = {self.lwip: ""}
        self.index = {self.idf: "", self.tlsf: ""}
        for declaration, resource in sdk.patch_inputs():
            repo = self.idf if declaration["repository"] == "idf" else self.tlsf
            names = [item["path"] for item in declaration["files"]]
            if repo == self.idf:
                names += [sdk.LOCK["lwip"]["path"], sdk.LOCK["tlsf"]["path"]]
            self.status[repo] = "\n".join(" M " + name for name in sorted(names))
            if repo == self.idf:
                self.status[repo] += "\n?? " + sdk.LOCK["derivation_stamp"]
            for item in declaration["files"]:
                p = repo / item["path"]
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(self.after[declaration["repository"], item["path"]])
            destination = self.root / declaration["path"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(resource, destination)
        self.submodules = "+" + sdk.LOCK["lwip"]["revision"] + " " + sdk.LOCK["lwip"]["path"] + "\n" + \
            " " + sdk.LOCK["tlsf"]["revision"] + " " + sdk.LOCK["tlsf"]["path"]

    def fake_git(self, path, *args):
        if args == ("rev-parse", "HEAD"):
            return self.heads[path]
        if args == ("rev-parse", "--show-toplevel"):
            return str(path.resolve())
        if args[:2] == ("status", "--porcelain"):
            return self.status[path]
        if args == ("diff", "--cached", "--name-only"):
            return self.index[path]
        if args == ("submodule", "status", "--recursive"):
            return self.submodules
        raise AssertionError(args)

    def fake_show(self, args, **kwargs):
        del kwargs
        assert args[0] == "git" and args[3] == "show"
        repo = "idf" if Path(args[2]) == self.idf else "tlsf"
        assert args[4].startswith("HEAD:")
        return SimpleNamespace(stdout=self.original[repo, args[4].removeprefix("HEAD:")])

    def gate(self):
        with patch.object(sdk, "ROOT", self.root), patch.object(sdk, "git", self.fake_git), \
                patch.object(sdk.subprocess, "run", self.fake_show):
            sdk.check(self.idf)

    def reject(self):
        with self.assertRaises(ValueError):
            self.gate()

    def test_actual_positive_and_isolated_contract(self):
        self.gate()

    def test_wrong_idf_revision(self):
        self.heads[self.idf] = "0" * 40
        self.reject()

    def test_wrong_lwip_revision_or_dirty(self):
        self.heads[self.lwip] = "0" * 40
        self.reject()
        self.heads[self.lwip] = sdk.LOCK["lwip"]["revision"]
        self.status[self.lwip] = " M arbitrary.c"
        self.reject()

    def test_wrong_tlsf_revision(self):
        self.heads[self.tlsf] = "0" * 40
        self.reject()

    def test_staged_sdk_change(self):
        self.index[self.idf] = "components/heap/multi_heap.c"
        self.reject()

    def test_extra_root_or_tlsf_change(self):
        self.status[self.idf] += "\n?? unexpected.bin"
        self.reject()
        self.status[self.idf] = self.status[self.idf].split("\n??")[0]
        self.status[self.tlsf] += "\n M unexpected.c"
        self.reject()

    def test_partial_or_tampered_managed_source(self):
        p = self.idf / "components/heap/multi_heap.c"
        p.write_bytes(self.original["idf", "components/heap/multi_heap.c"])
        self.reject()
        p.write_bytes(self.after["idf", "components/heap/multi_heap.c"] + b"\n")
        self.reject()

    def test_managed_source_symlink(self):
        p = self.tlsf / "tlsf.c"
        backup = self.root / "same-bytes.c"
        backup.write_bytes(p.read_bytes())
        p.unlink(); p.symlink_to(backup)
        self.reject()

    def test_patch_digest(self):
        p = self.root / sdk.LOCK["managed_patches"][0]["path"]
        p.write_bytes(p.read_bytes() + b"\n")
        self.reject()

    def test_missing_bad_or_linked_stamp(self):
        data = self.stamp.read_bytes()
        self.stamp.unlink()
        self.reject()
        self.stamp.write_bytes(data + b"\n")
        self.reject()
        other = self.root / "same-stamp.json"
        other.write_bytes(data)
        self.stamp.unlink(); self.stamp.symlink_to(other)
        self.reject()

    def test_official_baseline_digest(self):
        self.original = dict(self.original)
        self.original["idf", "components/heap/multi_heap.c"] += b"\n"
        self.reject()

    def test_unavailable_or_wrong_other_submodule(self):
        self.submodules += "\n-" + "0" * 40 + " components/other"
        self.reject()
        self.submodules = "+" + "0" * 40 + " components/other"
        self.reject()


if __name__ == "__main__":
    unittest.main()
