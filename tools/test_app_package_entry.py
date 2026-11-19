#!/usr/bin/env python3
"""应用正式打包入口的测试：新建一个最小应用，用临时生成的键和一份假的 Fast Deploy 身份跑一遍入口，核对 ZIP。

需要 cargo（rustup）、openssl，以及官方 ESP-IDF（签名和验签用 espsecure；没有导出环境时入口自己 source export.sh）。
Fast Deploy 那一侧（分配构建号、回读包内身份、留存制品）不在这里测。
    python3 tools/test_app_package_entry.py
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esp_base_app  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "tools/app_package_entry.py"
CLI = ROOT / "tools/esp_base.py"
IDENTITY = {"version": "0.1.0", "platform": "plugin", "application_id": "demo-product", "architecture": "wasm32",
            "variant": "demo_app", "local_validation": False, "run_id": "20261118-000001", "build_number": 7,
            "source_commit_sha": "0" * 40, "input_sha256": "1" * 64}


def canonical(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n").encode()


class FormalAppPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="esp-base-app-package-")
        cls.addClassCleanup(cls.directory.cleanup)
        root = Path(cls.directory.name)
        cls.key = root / "key.pem"
        subprocess.run(["openssl", "genrsa", "-out", str(cls.key), "3072"], check=True, capture_output=True)
        cls.project = root / "demo"
        subprocess.run([sys.executable, str(CLI), "app", "new", str(cls.project), "--name", "demo_app"], check=True, capture_output=True)
        cargo = shutil.which("cargo") or str(Path.home() / ".cargo/bin/cargo")
        subprocess.run([cargo, "generate-lockfile"], cwd=cls.project, check=True, capture_output=True)
        cls.lock = (cls.project / "Cargo.lock").read_bytes()

    def run_entry(self, identity, *, name="out", project=None, extra=None):
        run = Path(self.directory.name) / name
        shutil.rmtree(run, ignore_errors=True)
        run.mkdir()
        identity_file = run / "identity.json"
        identity_file.write_bytes(canonical(identity))
        artifact = run / "out" / "demo-app.zip"
        artifact.parent.mkdir()
        environment = {**os.environ, "FAST_DEPLOY_IDENTITY_FILE": str(identity_file), "ESP_BASE_APP_PROJECT": str(project or self.project),
                       "ESP_BASE_APPLICATION_ID": "demo-product", "ESP_BASE_SIGNING_KEY_FILE": str(self.key),
                       "ESP_BASE_SIGNING_TRUST": "software-test", **(extra or {})}
        result = subprocess.run([sys.executable, str(ENTRY), str(artifact)], env=environment, capture_output=True, text=True, timeout=600)
        return result, artifact, identity_file

    def test_inputs_describe_the_toolchain_and_are_stable(self):
        first = subprocess.run([sys.executable, str(ENTRY), "inputs", str(self.project)], check=True, capture_output=True).stdout
        second = subprocess.run([sys.executable, str(ENTRY), "inputs", str(self.project)], check=True, capture_output=True).stdout
        self.assertEqual(first, second)
        document = json.loads(first)
        self.assertEqual(first, canonical(document))
        self.assertEqual(document["app"], "demo_app")
        self.assertRegex(document["rust"]["rustc"], r"^rustc \d+\.\d+\.\d+")
        self.assertEqual(document["rust"]["target"], "wasm32v1-none")
        self.assertEqual(document["sources"]["Cargo.lock"], hashlib.sha256(self.lock).hexdigest())
        self.assertIn("Cargo.toml", document["sources"])
        self.assertNotIn("signing", document)   # 工具链输入不含签名信息

    def test_formal_package_contains_the_signed_app_and_the_exact_identity(self):
        result, artifact, identity_file = self.run_entry(IDENTITY)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        report = json.loads(result.stdout)
        with zipfile.ZipFile(artifact) as archive:
            names = archive.namelist()
            files = {name: archive.read(name) for name in names}
        self.assertEqual(names, ["Cargo.lock", "build.json", "demo_app.ebapp", "demo_app.wasm", "manifest.json"])
        # 包内身份与 Fast Deploy 写出的本次身份逐字节相同：Fast Deploy 回读时核对的就是它。
        self.assertEqual(files["build.json"], identity_file.read_bytes())
        self.assertEqual(files["Cargo.lock"], self.lock)
        manifest = json.loads(files["manifest.json"])
        self.assertEqual(manifest["kind"], "esp-base-app")
        self.assertEqual(manifest["app"]["name"], "demo_app")
        self.assertEqual(manifest["app"]["version"], "0.1.0+7")
        self.assertEqual(manifest["package"], "demo_app.ebapp")
        self.assertEqual(manifest["inputs"]["signing"]["trust"], "software-test")
        self.assertEqual(manifest["inputs"]["sources"]["Cargo.lock"], hashlib.sha256(self.lock).hexdigest())
        for name, entry in manifest["files"].items():
            self.assertEqual((entry["bytes"], entry["sha256"]), (len(files[name]), hashlib.sha256(files[name]).hexdigest()), name)
        described = esp_base_app.describe(files["demo_app.ebapp"])
        self.assertTrue(described["signed"])
        self.assertEqual(described["manifest"], manifest["app"])
        self.assertEqual(described["wasm"], files["demo_app.wasm"])
        self.assertEqual(len(files["demo_app.ebapp"]) % 4096, 0)
        self.assertEqual(report["app_sha256"], hashlib.sha256(files["demo_app.ebapp"]).hexdigest())
        self.assertEqual((report["name"], report["version"], report["signing_trust"]), ("demo_app", "0.1.0+7", "software-test"))
        self.assertEqual(report["artifact"]["artifact_sha256"], hashlib.sha256(artifact.read_bytes()).hexdigest())
        # 源码树没有被改动：构建目录在本次 run 的目录里，依赖锁原样。
        self.assertFalse((self.project / "target").exists())
        self.assertEqual((self.project / "Cargo.lock").read_bytes(), self.lock)
        # 包里的签名用这把键的公钥验得过，换一把键验不过。
        signed = artifact.parent / "demo_app.ebapp"
        signed.write_bytes(files["demo_app.ebapp"])
        other = Path(self.directory.name) / "other.pem"
        subprocess.run(["openssl", "genrsa", "-out", str(other), "3072"], check=True, capture_output=True)
        verify = lambda key: subprocess.run(  # noqa: E731
            ["bash", "-c", 'source "$HOME/.espressif/frameworks/esp-idf-v6.1/export.sh" >/dev/null 2>&1; '
             'python -m espsecure verify-signature --version 2 --keyfile "$1" "$2"', "bash", str(key), str(signed)],
            capture_output=True).returncode
        if (Path.home() / ".espressif/frameworks/esp-idf-v6.1/export.sh").is_file():
            self.assertEqual(verify(self.key), 0)
            self.assertNotEqual(verify(other), 0)

    def test_identity_that_is_not_this_app_is_refused_before_building(self):
        for change, message in (({"platform": "firmware"}, "不是应用 demo_app"), ({"variant": "other_app"}, "不是应用 demo_app"),
                                ({"architecture": "riscv32"}, "不是应用 demo_app"), ({"application_id": "another"}, "不是应用 demo_app"),
                                ({"local_validation": True}, "不是应用 demo_app"), ({"build_number": 0}, "不是应用 demo_app")):
            result, artifact, _ = self.run_entry({**IDENTITY, **change}, name="refused")
            self.assertNotEqual(result.returncode, 0, change)
            self.assertIn(message, result.stderr, change)
            self.assertFalse(artifact.exists(), change)

    def test_version_must_match_the_project(self):
        result, artifact, _ = self.run_entry({**IDENTITY, "version": "1.0.0"}, name="version")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("不一致", result.stderr)
        self.assertFalse(artifact.exists())

    def test_a_project_without_a_lockfile_or_signing_settings_is_refused(self):
        bare = Path(self.directory.name) / "bare"
        shutil.copytree(self.project, bare)
        (bare / "Cargo.lock").unlink()
        result, artifact, _ = self.run_entry(IDENTITY, name="no-lock", project=bare)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cargo.lock", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertFalse(artifact.exists())
        self.assertFalse((bare / "Cargo.lock").exists())   # --locked：不替工程生成依赖锁
        result, artifact, _ = self.run_entry(IDENTITY, name="trust", extra={"ESP_BASE_SIGNING_TRUST": "anything"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ESP_BASE_SIGNING_TRUST", result.stderr)
        self.assertFalse(artifact.exists())


if __name__ == "__main__":
    unittest.main()
