#!/usr/bin/env python3
"""使用官方签名工具的真实字节反例；不连接设备或分配正式构建号。"""
from pathlib import Path
import copy
import json
import os
import tempfile
import unittest
from unittest import mock
import subprocess
import sys
import zipfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

from build_package import (config_values, decode_json, digest, portable_input, public_key, verify_package,
                           verify_signed, write_once, core_encoded, encoded, validate_run_inputs, run_inputs)


class BuildPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.keys = {}
        for target, key in (("esp32", ec.generate_private_key(ec.SECP256R1())),
                            ("esp32c3", rsa.generate_private_key(public_exponent=65537, key_size=3072))):
            private = cls.root / (target + ".pem")
            private.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
            private.chmod(0o600)
            public = cls.root / (target + ".public")
            cls.command(target, "extract-public-key", "--keyfile", str(private), str(public))
            cls.keys[target] = private, public

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @staticmethod
    def command(target, action, *args):
        subprocess.run([sys.executable, "-m", "espsecure", action, "--version",
                        "2" if target == "esp32c3" else "1", *args],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def test_official_signature_binds_exact_bytes_for_both_targets(self):
        for target, (private, public) in self.keys.items():
            with self.subTest(target=target):
                original = self.root / (target + "-manifest.json")
                signed = self.root / (target + "-signed.bin")
                original.write_bytes(b'{"signing_trust":"software-test"}\n')
                self.command(target, "sign-data", "--keyfile", str(private),
                             "--output", str(signed), str(original))
                verify_signed(signed, original, public, target)
                original.write_bytes(b'{"signing_trust":"release"}\n')
                with self.assertRaisesRegex(ValueError, "实际编译字节"):
                    verify_signed(signed, original, public, target)
                original.write_bytes(b'{"signing_trust":"software-test"}\n')
                changed = bytearray(signed.read_bytes())
                changed[-68 if target == "esp32" else -4090] ^= 1
                signed.write_bytes(changed)
                with self.assertRaises(subprocess.CalledProcessError):
                    verify_signed(signed, original, public, target)

    def test_public_inputs_cannot_contain_private_material_or_wrong_target(self):
        for target, (private, public) in self.keys.items():
            self.assertEqual(public_key(public, target), public.read_bytes())
            with self.assertRaisesRegex(ValueError, "不能归档私钥"):
                public_key(private, target)
        with self.assertRaises(ValueError):
            public_key(self.keys["esp32c3"][1], "esp32")

    def test_immutable_record_replay_and_changed_bytes(self):
        path = self.root / "receipt.json"
        write_once(path, b'{"build_number":42}\n')
        first = digest(path.read_bytes())
        write_once(path, path.read_bytes())
        self.assertEqual(first, digest(path.read_bytes()))
        with self.assertRaisesRegex(ValueError, "拒绝覆盖"):
            write_once(path, b'{"build_number":43}\n')
        self.assertEqual(first, digest(path.read_bytes()))

    def test_strict_json_and_portable_input(self):
        for value in (b'{"build_number":1,"build_number":2}', b'{"x":NaN}'):
            with self.assertRaises(ValueError):
                decode_json(value)
        value = {"sdk_root": "/private/sdk", "sdkconfig": {"path": "/private/config", "sha256": "a"},
                 "verification_key": {"path": "/private/key", "sha256": "b"},
                 "tools": {"gcc": {"path": "/private/gcc", "sha256": "c"}}}
        self.assertEqual(portable_input(value), {"sdkconfig": {"sha256": "a"},
                         "verification_key": {"sha256": "b"}, "tools": {"gcc": {"sha256": "c"}}})
        archive = self.root / "escape.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("../firmware.bin", b"untrusted")
        with self.assertRaisesRegex(ValueError, "越界"):
            verify_package(archive)

    def test_generated_sdkconfig_aliases_must_agree(self):
        path = self.root / "sdkconfig"
        path.write_text("CONFIG_SAMPLE=y\nCONFIG_SAMPLE=y\n# CONFIG_OTHER is not set\n")
        self.assertEqual(config_values(path), {"CONFIG_SAMPLE": "y", "CONFIG_OTHER": "n"})
        path.write_text("CONFIG_SAMPLE=y\n# CONFIG_SAMPLE is not set\n")
        with self.assertRaisesRegex(ValueError, "冲突"):
            config_values(path)

    def run_fixture(self, target="esp32c3"):
        manifest = (Path(__file__).parent / ("package-" + target + ".json")).read_bytes()
        inputs = {"target": target, "source_commit_sha": "a" * 40, "sdkconfig": {"sha256": "b" * 64}}
        snapshot = {"schema_version": 1, "kind": "standalone-package-inputs", "commands": [{
            "argv": ["python3", "tools/package_entry.py", "inputs", target],
            "executable": "/冻结工具/python3", "executable_sha256": "c" * 64,
            "stdout": encoded(inputs).decode()}]}
        context = {"job_manifest_sha256": digest(manifest), "profile_snapshot_id": digest(core_encoded(snapshot))}
        return snapshot, manifest, inputs, context

    def test_core_snapshot_accepts_exact_inputs_for_each_target(self):
        for target in ("esp32c3", "esp32"):
            validate_run_inputs(*self.run_fixture(target))

    def test_product_digest_cannot_replace_core_snapshot(self):
        snapshot, manifest, inputs, context = self.run_fixture()
        context["profile_snapshot_id"] = digest(encoded(inputs))
        with self.assertRaisesRegex(ValueError, "完整输入快照"):
            validate_run_inputs(snapshot, manifest, inputs, context)

    def test_changed_source_config_or_target_cannot_reuse_snapshot(self):
        snapshot, manifest, inputs, context = self.run_fixture()
        for field, changed in (("source_commit_sha", "d" * 40),
                               ("sdkconfig", {"sha256": "e" * 64}), ("target", "esp32")):
            current = dict(inputs, **{field: changed})
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_run_inputs(snapshot, manifest, current, context)

    def test_changed_interpreter_or_command_is_not_the_original_snapshot(self):
        original, manifest, inputs, context = self.run_fixture()
        for field, value in (("executable_sha256", "d" * 64),
                             ("executable", "/another/python3"),
                             ("argv", ["python3", "untracked.py"])):
            snapshot = copy.deepcopy(original)
            snapshot["commands"][0][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "完整输入快照"):
                validate_run_inputs(snapshot, manifest, inputs, context)
        # 即使给定新摘要，也不得把任意命令输出当作固件输入 producer。
        context["profile_snapshot_id"] = digest(core_encoded(snapshot))
        with self.assertRaisesRegex(ValueError, "工具输出"):
            validate_run_inputs(snapshot, manifest, inputs, context)

    def test_run_inputs_keeps_venv_entry_and_checks_resolved_executable_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "python-real"
            binary.write_bytes(b"original executable")
            entry = root / "python3"
            entry.symlink_to(binary)
            snapshot, manifest, inputs, context = self.run_fixture()
            snapshot["commands"][0].update(executable=str(entry),
                                            executable_sha256=digest(binary.read_bytes()))
            context["profile_snapshot_id"] = digest(core_encoded(snapshot))
            request = {"schema_version": 1, "kind": "standalone-package-run",
                       "context": context, "manifest": json.loads(manifest)}
            for name, value in (("package-run.json", request),
                                ("client-version-result.json", {"context": context}),
                                ("package-inputs.json", snapshot)):
                (root / name).write_bytes(core_encoded(value))
            environment = {"FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE": str(root / "version.json"),
                           "FAST_DEPLOY_PROFILE_SNAPSHOT_ID": context["profile_snapshot_id"]}
            with mock.patch.dict(os.environ, environment), mock.patch("sys.executable", str(entry)):
                self.assertEqual(run_inputs(inputs)["package-inputs.json"], core_encoded(snapshot))
                # 同一二进制的其他入口不能冒充原 venv。
                with mock.patch("sys.executable", str(binary)), self.assertRaisesRegex(ValueError, "解释器"):
                    run_inputs(inputs)
                binary.write_bytes(b"changed executable")
                with self.assertRaisesRegex(ValueError, "解释器"):
                    run_inputs(inputs)

    def test_manifest_must_bind_real_product_entry_and_architecture(self):
        snapshot, original, inputs, context = self.run_fixture()
        for field, value in (("entry", "tools/another.py"), ("architectures", ["xtensa"]),
                             ("variant", "esp32"), ("artifact", "other.zip")):
            manifest = json.loads(original)
            manifest[field] = value
            raw = encoded(manifest)
            context["job_manifest_sha256"] = digest(raw)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "打包声明"):
                validate_run_inputs(snapshot, raw, inputs, context)


if __name__ == "__main__":
    unittest.main()
