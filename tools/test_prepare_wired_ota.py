#!/usr/bin/env python3
"""双目标合成 Flash、官方 NVS 与真实测试签名；不授予硬件资格。"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock
import zlib

import prepare_wired_ota as wired
import test_prepare_native_layout as fixture

native = wired.native
NEW_OPERATION = "55555555-5555-4555-8555-555555555555"


class WiredOtaTests(unittest.TestCase):
    # 复用既有合成镜像/官方工具夹具，不继承或重复整套历史迁入测试。
    secure = classmethod(fixture.NativeMigrationTests.secure.__func__)
    image = staticmethod(fixture.NativeMigrationTests.image)
    setUp = fixture.NativeMigrationTests.setUp
    nvs = fixture.NativeMigrationTests.nvs
    native_flash = fixture.NativeMigrationTests.native_flash

    @classmethod
    def setUpClass(cls):
        fixture.NativeMigrationTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        fixture.NativeMigrationTests.tearDownClass.__func__(cls)

    @staticmethod
    def config():
        raw = fixture.NativeMigrationTests.config()
        size = 40 + sum((raw[6], raw[7], raw[12], raw[13], native.u32(raw[14:16] + b"\0\0"),
                         native.u32(raw[16:18] + b"\0\0"), 32))
        current = bytearray(raw[:size])
        current[5] = 3
        current[20:40] = bytes(20)
        return bytes(current)

    def records(self, flash, name):
        return native.legacy.nvs_records(flash, "base_store", *native.LAYOUTS[name]["store"],
            native.legacy.load_nvs_parser(self.components), {native.CONFIG_KEY, wired.OPERATION_KEY})

    def replace_receipt(self, flash, name, mutate):
        records = self.records(flash, name)
        raw = bytearray(records[wired.OPERATION_KEY][1])
        mutate(raw)
        records[wired.OPERATION_KEY] = ("blob", bytes(raw))
        return self.replace_records(flash, name, records)

    def replace_records(self, flash, name, records):
        groups = {}
        for (namespace, key), (kind, raw) in records.items():
            self.assertEqual(kind, "blob")
            groups.setdefault(namespace, []).append((key, "base64", base64.b64encode(raw).decode()))
        at, size = native.LAYOUTS[name]["store"]
        changed = bytearray(flash)
        changed[at:at + size] = self.nvs(groups, size)
        return bytes(changed)

    def arguments(self, flash, name, suffix="prepared"):
        target = native.LAYOUTS[name]["target"]
        paths = [self.directory / (suffix + ending) for ending in ("-a.bin", "-b.bin")]
        for path in paths:
            path.write_bytes(flash)
            path.chmod(0o600)
        image = self.artifacts[target][0]
        key = self.keys[target][1]
        return argparse.Namespace(source_layout=name, backup_a=paths[0], backup_b=paths[1],
            idf_path=self.idf, device_id=fixture.DEVICE, previous_operation_id=fixture.OPERATION,
            operation_id=NEW_OPERATION, source_app_sha256=native.digest(self.images[(target, "old_source")]),
            verification_key=key, verification_key_sha256=native.digest(key.read_bytes()),
            image=image, image_sha256=native.digest(image.read_bytes()),
            output_directory=self.directory / suffix)

    def assert_blocked(self, args):
        with self.assertRaises((native.MigrationError, native.legacy.PreflightError, OSError, ValueError)):
            wired.prepare(args)
        self.assertFalse(args.output_directory.exists())

    def apply_stage(self, flash, plan, stage, directory):
        # 模拟的是完整已读回阶段，不能据此宣称实际写入或撕裂恢复通过。
        self.assertEqual(native.digest(flash), stage["before_flash_sha256"])
        data = (directory / stage["file"]).read_bytes()
        self.assertEqual((len(data), native.digest(data)), (stage["size_bytes"], stage["sha256"]))
        at = stage["address"]
        self.assertEqual(native.digest(flash[at:at + len(data)]), stage["before_range_sha256"])
        changed = bytearray(flash)
        changed[at:at + len(data)] = data
        self.assertEqual(native.digest(changed), stage["after_flash_sha256"])
        return bytes(changed)

    def test_three_complete_boundaries_both_targets_and_physical_page_orders(self):
        for name in ("c3_native", "esp32_native"):
            for reverse_pages in (False, True):
                with self.subTest(layout=name, reverse_pages=reverse_pages):
                    flash, _ = self.native_flash(name, 2, failed_inactive=True)
                    layout = native.LAYOUTS[name]
                    ota = layout["otadata"]
                    if reverse_pages:
                        flash = flash[:ota] + flash[ota + 4096:ota + 8192] + flash[ota:ota + 4096] + flash[ota + 8192:]
                    args = self.arguments(flash, name, name + str(reverse_pages))
                    plan = wired.prepare(args)
                    self.assertEqual(json.loads((args.output_directory / "plan.json").read_text()), plan)
                    self.assertEqual(plan["state"], "software_only")
                    self.assertFalse(plan["hardware_write_authorized"])
                    self.assertFalse(plan["device_verified"])
                    self.assertEqual(len(plan["stages"]), 3)
                    self.assertFalse((args.output_directory / "candidate-flash.bin").exists())
                    self.assertEqual((args.output_directory / "source-flash.bin").read_bytes(), flash)
                    prior = self.records(flash, name)
                    self.assertEqual((args.output_directory / "previous-operation.bin").read_bytes(), prior[wired.OPERATION_KEY][1])
                    phase1 = self.apply_stage(flash, plan, plan["stages"][0], args.output_directory)
                    next_records = self.records(phase1, name)
                    self.assertEqual(set(next_records), set(prior))
                    self.assertEqual(next_records[native.CONFIG_KEY], prior[native.CONFIG_KEY])
                    blob = next_records[wired.OPERATION_KEY][1]
                    self.assertEqual(len(blob), 182)
                    self.assertEqual(blob[:10], b"EOTA\x04\x01\x10\x11\x00\x00")
                    self.assertEqual(blob[14:50].decode(), fixture.DEVICE)
                    self.assertEqual(blob[50:86].decode(), NEW_OPERATION)
                    self.assertEqual(blob[86:118].hex(), args.image_sha256)
                    self.assertEqual(blob[118:150].hex(), args.source_app_sha256)
                    self.assertEqual(blob[150:182], bytes(32))
                    self.assertEqual(phase1[:layout["store"][0]], flash[:layout["store"][0]])
                    self.assertEqual(native.ota_selection(phase1, layout)[0], 0)
                    phase2 = self.apply_stage(phase1, plan, plan["stages"][1], args.output_directory)
                    candidate = args.image.read_bytes()
                    at = layout["apps"][1]
                    self.assertEqual(phase2[at:at + len(candidate)], candidate)
                    self.assertEqual(phase2[at + len(candidate):at + native.APP_SIZE], b"\xff" * (native.APP_SIZE - len(candidate)))
                    self.assertEqual(native.ota_selection(phase2, layout), (0, {0: 2, 1: 3}))
                    phase3 = self.apply_stage(phase2, plan, plan["stages"][2], args.output_directory)
                    preserved = plan["source_otadata_sector_address"]
                    self.assertEqual(phase3[preserved:preserved + 4096], flash[preserved:preserved + 4096])
                    changed = plan["stages"][2]["address"]
                    self.assertNotEqual(changed, preserved)
                    row = phase3[changed:changed + 32]
                    self.assertEqual(native.u32(row, 24), 0)  # NEW，绝非 PENDING/SUCCEEDED/VALID。
                    self.assertEqual((native.u32(row) - 1) % 2, 1)
                    self.assertEqual(native.u32(row, 28), zlib.crc32(row[:4], 0xffffffff))
                    self.assertEqual(self.records(phase3, name), next_records)
                    self.assertEqual(phase3[layout["apps"][0]:layout["apps"][0] + native.APP_SIZE],
                                     flash[layout["apps"][0]:layout["apps"][0] + native.APP_SIZE])
                    for start, size in ((0, ota), (ota + 8192, 0x20000 - ota - 8192),
                                        (0x3e0000, layout["store"][0] - 0x3e0000)):
                        self.assertEqual(phase3[start:start + size], flash[start:start + size])
                    for item in args.output_directory.iterdir():
                        self.assertEqual(item.stat().st_mode & 0o777, 0o600)

    def test_reject_source_state_ambiguity_and_dirty_inactive(self):
        for mutation in ("same_slot", "two_valid", "pending", "new", "crc", "unknown", "dirty", "overflow"):
            flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
            wrong = bytearray(flash)
            page = native.LAYOUTS["c3_native"]["otadata"] + 4096
            if mutation == "dirty":
                wrong[0x200000 + 4095] = 0
            elif mutation in ("same_slot", "overflow"):
                wrong[page:page + 4] = (5 if mutation == "same_slot" else 0xfffffffe).to_bytes(4, "little")
                wrong[page + 28:page + 32] = zlib.crc32(wrong[page:page + 4], 0xffffffff).to_bytes(4, "little")
            elif mutation == "crc":
                wrong[page + 28] ^= 1
            else:
                state = {"two_valid": 2, "pending": 1, "new": 0, "unknown": 0xffffff00}[mutation]
                wrong[page + 24:page + 28] = state.to_bytes(4, "little")
            with self.subTest(mutation=mutation):
                self.assert_blocked(self.arguments(bytes(wrong), "c3_native", mutation))

    def test_source_slot_one_and_untracked_inactive_keep_true_physical_source(self):
        for name in ("c3_native", "esp32_native"):
            with self.subTest(layout=name):
                original, _ = self.native_flash(name, 2, failed_inactive=True)
                layout = native.LAYOUTS[name]
                flash = bytearray(original)
                a, b = layout["apps"]
                flash[a:a + native.APP_SIZE], flash[b:b + native.APP_SIZE] = \
                    original[b:b + native.APP_SIZE], original[a:a + native.APP_SIZE]
                # 物理第一页指向 ota_1；另一页没有记录。不能按槽号选择待写物理页。
                ota = layout["otadata"]
                flash[ota:ota + 4] = (4).to_bytes(4, "little")
                flash[ota + 28:ota + 32] = zlib.crc32(flash[ota:ota + 4], 0xffffffff).to_bytes(4, "little")
                flash[ota + 4096:ota + 8192] = b"\xff" * 4096
                flash = self.replace_receipt(bytes(flash), name,
                    lambda raw: raw.__setitem__(slice(6, 8), b"\x11\x10"))
                args = self.arguments(flash, name, name)
                plan = wired.prepare(args)
                self.assertEqual((plan["source_slot"], plan["target_slot"]), ("ota_1", "ota_0"))
                self.assertEqual(plan["source_otadata_sector_address"], ota)
                self.assertEqual(plan["stages"][2]["address"], ota + 4096)
                self.assertEqual(plan["stages"][1]["address"], a)
                self.assertEqual(plan["new_otadata_sequence"], 5)
                at = layout["store"][0]
                store = (args.output_directory / plan["stages"][0]["file"]).read_bytes()
                phase1 = flash[:at] + store
                self.assertEqual(self.records(phase1, name)[wired.OPERATION_KEY][1][6:8], b"\x11\x10")
                self.assertFalse(plan["required_external_preconditions"]["verified_by_this_offline_tool"])

    def test_unresolved_receipt_corruption_and_independent_identity_reject(self):
        mutations = {"prepared": lambda b: b.__setitem__(5, 1), "succeeded": lambda b: b.__setitem__(5, 3),
                     "version": lambda b: b.__setitem__(4, 3), "reserved": lambda b: b.__setitem__(9, 1),
                     "failure": lambda b: b.__setitem__(8, 9), "source": lambda b: b.__setitem__(118, b[118] ^ 1)}
        for name, mutate in mutations.items():
            flash, _ = self.native_flash("esp32_native", 2, failed_inactive=True)
            wrong = self.replace_receipt(flash, "esp32_native", mutate)
            with self.subTest(mutation=name):
                self.assert_blocked(self.arguments(wrong, "esp32_native", name))
        flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
        for field, value in (("device_id", NEW_OPERATION), ("previous_operation_id", NEW_OPERATION),
                             ("operation_id", fixture.OPERATION), ("source_app_sha256", "11" * 32),
                             ("image_sha256", "22" * 32), ("verification_key_sha256", "33" * 32)):
            with self.subTest(field=field):
                args = self.arguments(flash, "c3_native", field)
                setattr(args, field, value)
                self.assert_blocked(args)

    def test_backups_candidate_target_signature_and_same_image_reject(self):
        flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
        for mutation in ("backup", "same_inode", "wrong_target", "same_image", "signature", "trailing"):
            with self.subTest(mutation=mutation):
                args = self.arguments(flash, "c3_native", mutation)
                if mutation == "backup":
                    wrong = bytearray(flash); wrong[0x8000] ^= 1; args.backup_b.write_bytes(wrong)
                elif mutation == "same_inode":
                    args.backup_b = args.backup_a
                else:
                    data = args.image.read_bytes()
                    if mutation == "wrong_target": data = self.images[("esp32", "new_native")]
                    elif mutation == "same_image": data = self.images[("esp32c3", "old_source")]
                    elif mutation == "trailing": data += b"\xff"
                    else:
                        bad = bytearray(data); bad[-4096 + 900] ^= 1; data = bytes(bad)
                    args.image = self.directory / (mutation + "-image.bin")
                    args.image.write_bytes(data)
                    args.image_sha256 = native.digest(data)
                self.assert_blocked(args)

    def test_c3_embedded_key_mismatch_rejects_even_when_official_external_signature_passes(self):
        flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
        args = self.arguments(flash, "c3_native")
        candidate = bytearray(args.image.read_bytes())
        block = len(candidate) - 4096
        candidate[block + 424] ^= 1  # 内嵌 rinv；不改真实签名，重算块 CRC。
        candidate[block + 1196:block + 1200] = zlib.crc32(candidate[block:block + 1196]).to_bytes(4, "little")
        args.image = self.directory / "bad-embedded-key.bin"
        args.image.write_bytes(candidate)
        args.image_sha256 = native.digest(candidate)
        self.secure("verify-signature", "--version", "2", "--keyfile", str(args.verification_key), str(args.image))
        self.assert_blocked(args)

    def test_partial_select_has_valid_sequence_crc_but_no_verified_stage_boundary(self):
        flash, _ = self.native_flash("esp32_native", 2, failed_inactive=True)
        args = self.arguments(flash, "esp32_native")
        plan = wired.prepare(args)
        stage = plan["stages"][2]
        torn = bytearray((args.output_directory / stage["file"]).read_bytes())
        torn[24:28] = (0xffffff00).to_bytes(4, "little")
        self.assertEqual(native.u32(torn, 28), zlib.crc32(torn[:4], 0xffffffff))
        self.assertNotEqual(native.digest(torn), stage["sha256"])
        self.assertEqual(stage["partial_write_state"], "unknown")
        self.assertFalse(stage["automatic_retry_or_resume"])
        self.assertTrue(plan["partial_nvs_or_otadata_write_may_prevent_application_recovery"])
        self.assertTrue(plan["must_not_reset_between_stages"])
        self.assertTrue(plan["must_not_batch_three_stages_as_generic_flash_files"])

    def test_archive_readback_failure_emits_no_stage_and_existing_output_is_not_overwritten(self):
        flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
        args = self.arguments(flash, "c3_native")
        original = native.write_verified
        def fail_archive(path, data):
            if path.name == "previous-operation.bin": raise OSError("synthetic archive readback failure")
            return original(path, data)
        with mock.patch.object(native, "write_verified", side_effect=fail_archive):
            with self.assertRaises(OSError): wired.prepare(args)
        self.assertEqual({p.name for p in args.output_directory.iterdir()}, {"source-flash.bin"})
        before = (args.output_directory / "source-flash.bin").read_bytes()
        with self.assertRaises(native.MigrationError): wired.prepare(args)
        self.assertEqual((args.output_directory / "source-flash.bin").read_bytes(), before)

    def test_cli_fifo_rejects_without_opening_device_or_creating_output(self):
        flash, _ = self.native_flash("c3_native", 2, failed_inactive=True)
        args = self.arguments(flash, "c3_native")
        fifo = self.directory / "image.fifo"
        os.mkfifo(fifo, 0o600)
        args.image = fifo
        command = [sys.executable, str(Path(wired.__file__))]
        for key, value in vars(args).items(): command.extend(("--" + key.replace("_", "-"), str(value)))
        result = subprocess.run(command, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("普通文件".encode(), result.stderr)
        self.assertFalse(args.output_directory.exists())


if __name__ == "__main__":
    unittest.main()
