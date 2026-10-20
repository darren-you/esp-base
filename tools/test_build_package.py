#!/usr/bin/env python3
"""使用官方签名工具的真实字节反例；不连接设备或分配正式构建号。"""
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
import zipfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

from build_package import (config_values, decode_json, digest, portable_input, public_key, verify_package,
                           verify_signed, write_once)


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


if __name__ == "__main__":
    unittest.main()
