#!/usr/bin/env python3
"""实际 C 常量回读及镜像偏移／冻结身份拒绝测试；不访问 MCU。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build_identity import (MAX_NUMBER, ROOT, descriptor_bytes,
                            inspect_image, prepare, resolve)


class BuildIdentityTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.env = patch.dict(os.environ, {}, clear=True)
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def formal(self, target="esp32c3", number=MAX_NUMBER):
        value = resolve(target)
        value.update(local_validation=False, build_number=number, run_id="firmware-42",
                     source_commit_sha="a" * 40, input_sha256="b" * 64)
        return value

    def test_actual_compiled_descriptor_has_full_number_and_fixed_layout(self):
        for target in ("esp32c3", "esp32"):
            for value in (resolve(target), self.formal(target)):
                prepare(self.directory, value, target)
                (self.directory / "reader.c").write_text(
                    '#include <stdio.h>\n#include "esp_base_build.h"\n'
                    'int main(void) { return fwrite(&esp_base_build, 1, sizeof esp_base_build, stdout)'
                    ' == sizeof esp_base_build ? 0 : 1; }\n')
                executable = self.directory / "reader"
                subprocess.run(["/usr/bin/cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-I", str(ROOT / "firmware/apps/esp_base/main"),
                                str(self.directory / "esp_base_build.c"), str(self.directory / "reader.c"),
                                "-o", str(executable)], check=True)
                self.assertEqual(subprocess.check_output([str(executable)]), descriptor_bytes(value, target))

    def test_partial_formal_environment_never_falls_back_to_local(self):
        for key in ("FAST_DEPLOY_RUN_ID", "FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE",
                    "FAST_DEPLOY_SOURCE_REF", "FAST_DEPLOY_PACKAGE_CORE_ROOT",
                    "FAST_DEPLOY_RESOLVED_COMMIT_SHA", "FAST_DEPLOY_CORE_COMMIT_SHA"):
            with patch.dict(os.environ, {key: "present"}, clear=True), self.assertRaises(ValueError):
                resolve("esp32c3")

    def test_verify_rejects_changed_identity_and_modified_generated_c(self):
        value = self.formal()
        prepare(self.directory, value, "esp32c3")
        prepare(self.directory, value, "esp32c3", verify=True)
        different = dict(value, build_number=42)
        with self.assertRaises(ValueError):
            prepare(self.directory, different, "esp32c3", verify=True)
        (self.directory / "esp_base_build.c").write_text("forged")
        with self.assertRaises(ValueError):
            prepare(self.directory, value, "esp32c3", verify=True)

    def test_local_lab_and_native_capacity_are_explicit(self):
        self.assertEqual(resolve("esp32", "-capacity-lab")["version"], "1.0.0-capacity-lab")
        value = self.formal()
        for field, invalid in (("build_number", True), ("build_number", 0),
                               ("build_number", MAX_NUMBER + 1), ("run_id", "r" * 128),
                               ("version", "1.0.0-capacity-lab"), ("architecture", "any"),
                               ("source_commit_sha", "x" * 40)):
            with self.subTest(field=field, invalid=invalid), self.assertRaises(ValueError):
                descriptor_bytes(dict(value, **{field: invalid}), "esp32c3")
        self.assertEqual(len(descriptor_bytes(value, "esp32c3")), 320)

    def image(self, value, target):
        image = bytearray(32 + 256)
        image[0:2] = bytes([0xE9, 1])
        image[12:14] = (5 if target == "esp32c3" else 0).to_bytes(2, "little")
        image[28:32] = (256 + 320).to_bytes(4, "little")
        image[32:36] = (0xABCD5432).to_bytes(4, "little")
        image[48:80] = value["version"].encode().ljust(32, b"\0")
        image[80:112] = b"esp_base".ljust(32, b"\0")
        image += descriptor_bytes(value, target)
        return image

    def test_fixed_offset_target_native_version_and_full_bytes_are_checked(self):
        for target in ("esp32c3", "esp32"):
            value = self.formal(target)
            image = self.image(value, target)
            path = self.directory / "image.bin"
            path.write_bytes(image)
            self.assertEqual(inspect_image(path, value, target), value)
            for offset in (0, 12, 28, 32, 48, 80, 288, 296, 400, len(image) - 1):
                modified = bytearray(image)
                modified[offset] ^= 0xff
                path.write_bytes(modified)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    inspect_image(path, value, target)
            path.write_bytes(image[:-1])
            with self.assertRaises(ValueError):
                inspect_image(path, value, target)


if __name__ == "__main__":
    unittest.main()
