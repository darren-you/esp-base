#!/usr/bin/env python3
"""官方SDK与真实测试签名验证一次性离线迁入；只使用合成Flash。"""
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

import prepare_native_layout as migration

DEVICE = "22222222-2222-4222-8222-222222222222"
OPERATION = "44444444-4444-4444-8444-444444444444"


class NativeMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.idf = Path(os.environ["IDF_PATH"])
        cls.components = migration.legacy.check_sdk(cls.idf)
        cls.temp = tempfile.TemporaryDirectory(prefix="native_layout_test_")
        cls.work = Path(cls.temp.name)
        cls.generator = cls.components / "nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        cls.keys = {}
        cls.artifacts = {}
        cls.images = {}
        cls.old_tables = {}
        for target, scheme, version in (("esp32c3", "rsa3072", 2), ("esp32", "ecdsa256", 1)):
            key = cls.work / (target + "_test_key.pem")
            public = cls.work / (target + "_verification_key.bin")
            cls.secure("generate-signing-key", "--version", str(version), "--scheme", scheme, str(key))
            cls.secure("extract-public-key", "--version", str(version), "--keyfile", str(key), str(public))
            cls.keys[target] = (key, public)
            public_bytes = public.read_bytes() if target == "esp32" else b""
            for name in ("old_source", "old_candidate", "new_native"):
                plain = cls.image(target, name.encode(), public_bytes)
                unsigned = cls.work / (target + "_" + name + "_unsigned.bin")
                signed = cls.work / (target + "_" + name + ".bin")
                unsigned.write_bytes(plain)
                cls.secure("sign-data", "--version", str(version), "--keyfile", str(key), "--output", str(signed), str(unsigned))
                cls.images[(target, name)] = signed.read_bytes()
            table_name = "c3-partition-table.csv" if target == "esp32c3" else "esp32-partition-table.csv"
            plain_table = migration.table_from_csv(migration.ROOT / "firmware/partitions" / table_name, cls.components)
            table = cls.work / (target + "_new_table.bin")
            table.write_bytes(plain_table)
            if target == "esp32":
                unsigned_table = cls.work / "esp32_table_unsigned.bin"
                unsigned_table.write_bytes(plain_table)
                cls.secure("sign-data", "--version", "1", "--keyfile", str(key), "--output", str(table), str(unsigned_table))
            app = cls.work / (target + "_new_native.bin")
            bootloader = cls.work / (target + "_bootloader.bin")
            bootloader.write_bytes(cls.image(target, b"bootloader", public_bytes, app=False))
            cls.artifacts[target] = (app, bootloader, table)
        for source_layout, layout in migration.LAYOUTS.items():
            if source_layout == "esp32_at":
                rows = [("nvs", "data", "nvs", "0x12000", "0xe000", ""),
                        ("at_customize", "data", "undefined", "0x20000", "0xe0000", ""),
                        ("factory", "app", "factory", "0x100000", "0x1b0000", "")]
            elif source_layout == "c3_mqtt_factory":
                rows = [("nvs", "data", "nvs", "0x9000", "0x6000", ""),
                        ("phy_init", "data", "phy", "0xf000", "0x1000", ""),
                        ("factory", "app", "factory", "0x10000", "0x100000", "")]
            else:
                rows = [("nvs", "data", "nvs", "0x9000", "0x6000", ""),
                        ("phy_init", "data", "phy", hex(layout["phy"]), "0x1000", ""),
                        ("otadata", "data", "ota", hex(layout["otadata"]), "0x2000", ""),
                        ("coredump", "data", "coredump", "0x12000", "0xe000", ""),
                        ("ota_0", "app", "ota_0", hex(layout["apps"][0]), hex(layout["app_size"]), ""),
                        ("ota_1", "app", "ota_1", hex(layout["apps"][1]), hex(layout["app_size"]), ""),
                        ("base_store", "data", "nvs", hex(layout["store"][0]), hex(layout["store"][1]), "")]
                if "packages" in layout:
                    rows.extend([("product_pkgs", "data", "undefined", hex(layout["packages"][0]), hex(layout["packages"][1]), ""),
                                 ("frp_scratch", "data", "undefined", hex(layout["scratch"]), "0x10000", "")])
                    if layout["target"] == "esp32":
                        rows.append(("at_old_raw", "data", "undefined", "0x3e6000", "0x4000", "readonly"))
            path = cls.work / (source_layout + ".csv")
            with path.open("w", newline="") as out:
                csv.writer(out).writerows(sorted(rows, key=lambda row: int(row[3], 16)))
            cls.old_tables[source_layout] = migration.table_from_csv(path, cls.components)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def secure(cls, *arguments):
        result = subprocess.run([sys.executable, "-m", "espsecure", *arguments], capture_output=True, check=False)
        if result.returncode:
            raise AssertionError(result.stderr.decode() + result.stdout.decode())

    @staticmethod
    def image(target, version, key=b"", app=True, project=b"esp_base", payload=b""):
        header = bytearray(24)
        header[0:2] = bytes((0xe9, 1))
        header[12:14] = (5 if target == "esp32c3" else 0).to_bytes(2, "little")
        header[23] = 1
        data = bytearray(256 if app else 16)
        if app:
            data[:4] = (0xabcd5432).to_bytes(4, "little")
            data[16:16 + len(version)] = version
            data[48:48 + len(project)] = project
        data.extend(key)
        data.extend(payload)
        raw = header + struct.pack("<II", 0x3c000020, len(data)) + data
        checksum = 0xef
        for byte in data:
            checksum ^= byte
        raw += bytes((15 - len(raw) % 16) % 16) + bytes((checksum,))
        return bytes(raw) + hashlib.sha256(raw).digest()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(prefix="native_layout_case_", dir=self.work)
        self.addCleanup(self.case.cleanup)
        self.directory = Path(self.case.name)

    def nvs(self, records, size, version=2):
        source, output = self.directory / "nvs.csv", self.directory / "nvs.bin"
        with source.open("w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(("key", "type", "encoding", "value"))
            for namespace, group in records.items():
                writer.writerow((namespace, "namespace", "", ""))
                for key, encoding, value in group:
                    writer.writerow((key, "data", encoding, value))
        result = subprocess.run([sys.executable, str(self.generator), "generate", str(source), str(output),
                                 hex(size), "--version", str(version)], capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return output.read_bytes()

    @staticmethod
    def config():
        ca = b"-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n"
        fields = [b"testwifi", b"password", b"broker.example.test", "设备".encode(), b"test-secret", ca]
        frp_fields = [b"frp.example.test", b"test-token", ca, b"base-device"]
        header = bytearray(40)
        header[:8] = b"EBCF\x03\x07\x08\x08"
        header[8:12] = (42).to_bytes(4, "little")
        header[12], header[13] = len(fields[2]), len(fields[3])
        header[14:16] = len(fields[4]).to_bytes(2, "little")
        header[16:18] = len(ca).to_bytes(2, "little")
        header[18:20] = (8883).to_bytes(2, "little")
        header[20], header[21] = len(frp_fields[0]), len(frp_fields[3])
        header[22:24] = len(frp_fields[1]).to_bytes(2, "little")
        header[24:26] = len(ca).to_bytes(2, "little")
        for offset, value in ((26, 7000), (28, 10200), (30, 8123)):
            header[offset:offset + 2] = value.to_bytes(2, "little")
        return bytes(header) + b"".join(fields) + bytes((1,)) + bytes(31) + \
               b"".join(frp_fields) + bytes((2,)) + bytes(31)

    def product_flash(self, source_layout, *, receipt_state=3, ecs_phase=0, ledger_pending=False):
        layout = migration.LAYOUTS[source_layout]
        target = layout["target"]
        flash = bytearray(b"\xff" * migration.FLASH_SIZE)
        table = self.old_tables[source_layout]
        flash[0x8000:0x8000 + len(table)] = table
        identity = self.nvs({"base_identity": [("device_uuid", "string", DEVICE)]}, 0x6000)
        flash[0x9000:0xf000] = identity
        flash[layout["phy"]:layout["phy"] + 0x1000] = bytes((0x67,)) * 0x1000
        flash[0x12000:0x20000] = bytes((0x92,)) * 0xe000
        images = [self.images[(target, name)] for name in ("old_source", "old_candidate")]
        for index, image in enumerate(images):
            offset = layout["apps"][index]
            flash[offset:offset + len(image)] = image
        for index in range(2):
            offset = layout["otadata"] + index * 0x1000
            seq = (index + 1 if receipt_state == 3 else 3 - index).to_bytes(4, "little")
            flash[offset:offset + 4] = seq
            flash[offset + 24:offset + 28] = (2).to_bytes(4, "little")
            flash[offset + 28:offset + 32] = zlib.crc32(seq, 0xffffffff).to_bytes(4, "little")
        ecs = bytearray(288)
        ecs[:12] = b"ECS2" + (2).to_bytes(4, "little") + (7).to_bytes(4, "little")
        for index, image in enumerate(images):
            at = 12 + index * 80
            ecs[at] = 1
            ecs[at + 4:at + 36] = hashlib.sha256(image).digest()
        ecs[172] = ecs_phase
        ecs[284:] = zlib.crc32(ecs[:284]).to_bytes(4, "little")
        receipt = bytearray(308)
        receipt[:10] = b"EOTA\x03" + bytes((receipt_state, 0x10, 0x11, 0 if receipt_state == 3 else 6, 1))
        receipt[10:14] = len(images[1]).to_bytes(4, "little")
        receipt[14:50] = DEVICE.encode()
        receipt[50:86] = OPERATION.encode()
        receipt[86:118] = hashlib.sha256(images[1]).digest()
        receipt[118:150] = hashlib.sha256(images[0]).digest()
        if receipt_state == 2:
            receipt[150:182] = hashlib.sha256(images[1]).digest()
        receipt[182:186] = (6).to_bytes(4, "little")
        ledger = bytearray(910)
        ledger[:5] = b"EPRD\x01"
        if ledger_pending:
            ledger[5] = 1
            ledger[6:10] = (1).to_bytes(4, "little")
            row = bytearray(112)
            row[:4] = (1).to_bytes(4, "little")
            row[4:40] = OPERATION.encode()
            row[41:73] = bytes((1,)) * 32
            row[73:76] = bytes((1, 1, 0))
            row[76:108] = bytes((2,)) * 32
            row[108:] = (7).to_bytes(4, "little")
            ledger[10:122] = row
        ledger[906:] = zlib.crc32(ledger[:906]).to_bytes(4, "little")
        blobs = {"base_config": [("committed", "base64", base64.b64encode(self.config()).decode())],
                 "base_ota": [("operation", "base64", base64.b64encode(receipt).decode())],
                 "base_pkg": [("slots", "base64", base64.b64encode(ecs).decode())],
                 "base_product": [("operations", "base64", base64.b64encode(ledger).decode())]}
        store = self.nvs(blobs, layout["store"][1])
        flash[layout["store"][0]:] = store
        flash[layout["scratch"]:layout["scratch"] + 0x10000] = bytes((0x31,)) * 0x10000
        if target == "esp32":
            flash[0x3e6000:0x3ea000] = bytes((0x52,)) * 0x4000
        return flash

    def arguments(self, flash, layout):
        target = migration.LAYOUTS[layout]["target"]
        paths = (self.directory / "backup_a.bin", self.directory / "backup-b.bin")
        for path in paths:
            path.write_bytes(flash)
            path.chmod(0o600)
        app, bootloader, table = self.artifacts[target]
        return argparse.Namespace(backup_a=paths[0], backup_b=paths[1], source_layout=layout,
            idf_path=self.idf, device_id=DEVICE, source_efuse_mac=None,
            source_verification_key=self.keys[target][1], verification_key=self.keys[target][1],
            app=app, bootloader=bootloader, partition_table=table,
            output_directory=self.directory / "prepared")

    def test_both_targets_archive_terminal_and_preserve_real_records(self):
        for layout_name in ("c3_product", "esp32_product"):
            with self.subTest(layout=layout_name):
                flash = self.product_flash(layout_name)
                args = self.arguments(flash, layout_name)
                args.output_directory = self.directory / layout_name
                report = migration.prepare(args)
                candidate = (args.output_directory / "candidate-flash.bin").read_bytes()
                self.assertEqual((args.output_directory / "source-flash.bin").read_bytes(), flash)
                receipt = json.loads((args.output_directory / "migration-receipt.json").read_text())
                self.assertEqual(receipt, report)
                self.assertEqual(report["old_operation"], {"operation_id": OPERATION, "state": "succeeded"})
                self.assertEqual(report["old_operation_runtime_result"], "unknown_after_wired_identity_change")
                self.assertFalse(report["hardware_write_authorized"])
                self.assertFalse(report["device_verified"])
                self.assertEqual(candidate[0x9000:0xf000], flash[0x9000:0xf000])
                layout = migration.LAYOUTS[layout_name]
                self.assertEqual(candidate[layout["phy"]:layout["phy"] + 0x1000], flash[layout["phy"]:layout["phy"] + 0x1000])
                self.assertEqual(candidate[0x12000:0x20000], flash[0x12000:0x20000])
                self.assertEqual(candidate[layout["scratch"]:layout["scratch"] + 0x10000], flash[layout["scratch"]:layout["scratch"] + 0x10000])
                if layout_name == "esp32_product":
                    old_archive = flash[0x3e6000:0x3ea000]
                    self.assertEqual(candidate[0x3e5000:0x3ea000],
                                     old_archive[:0x2000] + b"\xff" * 0x1000 + old_archive[0x2000:])
                    self.assertEqual(candidate[0x3e5000:0x3e7000], old_archive[:0x2000])
                    self.assertEqual(candidate[0x3e8000:0x3ea000], old_archive[0x2000:])
                offset, size = layout["store"]
                records = migration.legacy.nvs_records(candidate, "base_store", offset, size,
                    migration.legacy.load_nvs_parser(self.components), {migration.CONFIG_KEY})
                self.assertEqual(records, {migration.CONFIG_KEY: ("blob", self.config())})
                for app_offset in migration.APP_OFFSETS:
                    image = self.images[(layout["target"], "new_native")]
                    self.assertEqual(candidate[app_offset:app_offset + len(image)], image)
                selected, states = migration.ota_selection(candidate, {"otadata": layout["otadata"]})
                self.assertEqual((selected, states), (1, {0: 2, 1: 2}))
                for file in args.output_directory.iterdir():
                    self.assertEqual(file.stat().st_mode & 0o777, 0o600)

    def test_failed_terminal_keeps_original_id_in_archive(self):
        args = self.arguments(self.product_flash("c3_product", receipt_state=2), "c3_product")
        self.assertEqual(migration.prepare(args)["old_operation"], {"operation_id": OPERATION, "state": "failed"})

    def test_prepared_receipt_ecs2_or_ledger_block_before_output(self):
        for options in ({"receipt_state": 1}, {"ecs_phase": 4}, {"ledger_pending": True}):
            with self.subTest(options=options):
                args = self.arguments(self.product_flash("c3_product", **options), "c3_product")
                with self.assertRaises(migration.MigrationError):
                    migration.prepare(args)
                self.assertFalse(args.output_directory.exists())

    def test_mismatched_backups_identity_and_nvs_crc_block(self):
        for mutation in ("backup", "identity", "nvs"):
            with self.subTest(mutation=mutation):
                flash = self.product_flash("c3_product")
                if mutation == "nvs":
                    at = flash.find(b"EOTA", 0x3f5000)
                    flash[at] ^= 1
                args = self.arguments(flash, "c3_product")
                if mutation == "identity":
                    args.device_id = OPERATION
                if mutation == "backup":
                    other = bytearray(flash)
                    other[0x20000] ^= 1
                    args.backup_b.write_bytes(other)
                with self.assertRaises((migration.MigrationError, migration.legacy.PreflightError)):
                    migration.prepare(args)
                self.assertFalse(args.output_directory.exists())

    def test_fifo_inputs_block_without_waiting_for_writer_or_creating_candidate(self):
        flash = self.product_flash("c3_product")
        for name in ("backup_a", "backup_b", "source_verification_key", "verification_key",
                     "app", "bootloader", "partition_table"):
            with self.subTest(input=name):
                args = self.arguments(flash, "c3_product")
                fifo = self.directory / (name + ".fifo")
                os.mkfifo(fifo, 0o600)
                setattr(args, name, fifo)
                command = [sys.executable, str(migration.ROOT / "tools/prepare_native_layout.py")]
                for field, value in vars(args).items():
                    if value is not None:
                        command.extend(["--" + field.replace("_", "-"), str(value)])
                result = subprocess.run(command, text=True, capture_output=True, check=False, timeout=5)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("普通文件", result.stderr)
                self.assertFalse(args.output_directory.exists())

    def test_signature_target_and_verification_key_snapshot(self):
        for target, offset in (("esp32c3", 4096 + 900), ("esp32", 12)):
            with self.subTest(target=target):
                args = self.arguments(self.product_flash("c3_product"), "c3_product")
                bad = bytearray(self.images[(target, "new_native")])
                bad[offset] ^= 1
                path = self.directory / "bad_app.bin"
                path.write_bytes(bad)
                args.app = path
                with self.assertRaises(migration.MigrationError):
                    migration.prepare(args)
                self.assertFalse(args.output_directory.exists())
        args = self.arguments(self.product_flash("c3_product"), "c3_product")
        key = self.directory / "verification_key.bin"
        original_key = self.keys["esp32c3"][1].read_bytes()
        key.write_bytes(original_key)
        args.verification_key = args.source_verification_key = key
        original_run = migration.subprocess.run
        def change_external_key_at_verification(arguments, **options):
            if "verify-signature" in arguments:
                key.write_bytes(b"changed external verification input")
            return original_run(arguments, **options)
        with mock.patch.object(migration.subprocess, "run", side_effect=change_external_key_at_verification):
            report = migration.prepare(args)
        self.assertNotEqual(key.read_bytes(), original_key)
        self.assertEqual(report["verification_key_sha256"], migration.digest(original_key))
        self.assertEqual(report["source_verification_key_sha256"], migration.digest(original_key))

    def test_unsigned_c3_v1_preserves_uuid_and_converts_config_only(self):
        flash = bytearray(b"\xff" * migration.FLASH_SIZE)
        table = self.old_tables["c3_v1"]
        flash[0x8000:0x8000 + len(table)] = table
        flash[0x9000:0xf000] = self.nvs({"base_identity": [("device_uuid", "string", DEVICE)]}, 0x6000)
        config = bytearray(112)
        config[:8] = b"EBCF\x01\x01\x08\x08"
        config[8:12] = (42).to_bytes(4, "little")
        config[16:24], config[48:56] = b"testwifi", b"password"
        flash[0x3e0000:] = self.nvs({"base_config": [("committed", "base64", base64.b64encode(config).decode())]}, 0x20000)
        image = self.image("esp32c3", b"legacy_v1")
        flash[0x20000:0x20000 + len(image)] = image
        sequence = (1).to_bytes(4, "little")
        flash[0xf000:0xf004] = sequence
        flash[0xf018:0xf01c] = (2).to_bytes(4, "little")
        flash[0xf01c:0xf020] = zlib.crc32(sequence, 0xffffffff).to_bytes(4, "little")
        args = self.arguments(flash, "c3_v1")
        args.source_verification_key = None
        report = migration.prepare(args)
        self.assertEqual(report["config_revision"], 42)
        candidate = (args.output_directory / "candidate-flash.bin").read_bytes()
        records = migration.legacy.nvs_records(candidate, "base_store", 0x3f5000, 0xb000,
            migration.legacy.load_nvs_parser(self.components), {migration.CONFIG_KEY})
        self.assertEqual(records[migration.CONFIG_KEY], ("blob", migration.legacy.convert_v1_wifi_only(config)))
        self.assertEqual(candidate[0x9000:0xf000], flash[0x9000:0xf000])
        self.assertEqual(candidate[0x3e5000:0x3f5000], b"\xff" * 0x10000)
        for suffix in (b"unexpected", self.images[("esp32c3", "old_source")][len(image):]):
            with self.subTest(unsigned_tail=len(suffix)):
                wrong = bytearray(flash)
                wrong[0x20000 + len(image):0x20000 + len(image) + len(suffix)] = suffix
                args = self.arguments(wrong, "c3_v1")
                args.source_verification_key = None
                args.output_directory = self.directory / ("wrong_unsigned_tail_" + str(len(suffix)))
                with self.assertRaises(migration.MigrationError):
                    migration.prepare(args)
                self.assertFalse(args.output_directory.exists())

    def test_c3_mqtt_factory_first_install_archives_source_and_clears_old_app_regions(self):
        flash = bytearray(b"\xff" * migration.FLASH_SIZE)
        table = self.old_tables["c3_mqtt_factory"]
        flash[0x8000:0x8000 + len(table)] = table
        image = self.image("esp32c3", b"lab", project=b"esp_mqtt_broker_client",
                           payload=b"\x37" * 0x12000)
        flash[0x10000:0x10000 + len(image)] = image
        args = self.arguments(flash, "c3_mqtt_factory")
        args.device_id = args.source_verification_key = None
        args.source_efuse_mac = "01:02:03:04:05:06"
        report = migration.prepare(args)
        candidate = (args.output_directory / "candidate-flash.bin").read_bytes()
        self.assertEqual((args.output_directory / "source-flash.bin").read_bytes(), flash)
        self.assertNotIn("device_id", report)
        self.assertEqual(flash[0x11000], 0x37)
        self.assertEqual(report["source_efuse_mac"], "010203040506")
        self.assertEqual(report["source_app_sha256"], migration.digest(image))
        self.assertEqual(candidate[0x9000:0xf000], flash[0x9000:0xf000])
        self.assertEqual(candidate[0x11000:0x20000], b"\xff" * 0xf000)
        self.assertEqual(candidate[0x3e5000:], b"\xff" * 0x1b000)
        self.assertEqual(migration.ota_selection(candidate, {"otadata": 0xf000}),
                         (1, {0: 2, 1: 2}))
        for offset in migration.APP_OFFSETS:
            new_app = self.images[("esp32c3", "new_native")]
            self.assertEqual(candidate[offset:offset + len(new_app)], new_app)
        self.assertFalse(report["hardware_write_authorized"])

    def test_c3_mqtt_factory_rejects_persistent_data_wrong_image_and_unbound_identity(self):
        original = bytearray(b"\xff" * migration.FLASH_SIZE)
        table = self.old_tables["c3_mqtt_factory"]
        original[0x8000:0x8000 + len(table)] = table
        image = self.image("esp32c3", b"lab", project=b"esp_mqtt_broker_client")
        original[0x10000:0x10000 + len(image)] = image
        for mutation in ("nvs", "tail", "image", "chip", "project", "padding", "mac", "uuid", "key", "layout"):
            with self.subTest(mutation=mutation):
                flash = bytearray(original)
                if mutation == "nvs": flash[0x9000] = 0
                elif mutation == "tail": flash[0x3e0000] = 0
                elif mutation == "image": flash[0x10040] ^= 1
                elif mutation == "chip": flash[0x1000c] = 0
                elif mutation == "project":
                    wrong = self.image("esp32c3", b"lab", project=b"another_project")
                    flash[0x10000:0x10000 + len(wrong)] = wrong
                elif mutation == "padding": flash[0x10000 + len(image)] = 0
                elif mutation == "layout": flash[0x8008] ^= 1
                args = self.arguments(flash, "c3_mqtt_factory")
                args.output_directory = self.directory / mutation
                args.device_id = args.source_verification_key = None
                args.source_efuse_mac = "01:02:03:04:05:06"
                if mutation == "mac": args.source_efuse_mac = "not-a-mac"
                elif mutation == "uuid": args.device_id = DEVICE
                elif mutation == "key": args.source_verification_key = self.keys["esp32c3"][1]
                with self.assertRaises(migration.MigrationError):
                    migration.prepare(args)
                self.assertFalse(args.output_directory.exists())

    def test_esp32_at_archive_binds_physical_mac_and_does_not_invent_uuid(self):
        flash = bytearray(b"\xff" * migration.FLASH_SIZE)
        table = self.old_tables["esp32_at"]
        flash[0x8000:0x8000 + len(table)] = table
        old_nvs = self.nvs({"phy": [("cal_mac", "base64", "AQIDBAUG")],
                            "nvs.net80211": [("sta.ssid", "base64", base64.b64encode(b"\xff" * 36).decode()),
                                             ("sta.pswd", "base64", base64.b64encode(b"\xff" * 65).decode())],
                            "misc": [("page" + str(index), "base64",
                                      base64.b64encode(bytes((index + 1,)) * 1000).decode())
                                     for index in range(7)]}, 0xe000, version=1)
        old_nvs = bytearray(old_nvs)
        parser = migration.legacy.load_nvs_parser(self.components)
        parsed = parser.NVS_Partition("old_at_nvs", old_nvs)
        wifi_payload_at = None
        for page_index, page in enumerate(parsed.pages):
            for entry in page.entries:
                if entry.state == "Written" and entry.key == "sta.ssid":
                    wifi_payload_at = page_index * 0x1000 + 64 + entry.children[0].index * 32
                    padding_at = page_index * 0x1000 + 64 + entry.children[-1].index * 32 + entry.data["size"] % 32
                    old_nvs[padding_at] = 0x37
        self.assertIsNotNone(wifi_payload_at)
        self.assertNotEqual(old_nvs[0x2000:0x3000], b"\xff" * 0x1000)
        self.assertEqual(old_nvs[0x3000:], b"\xff" * 0xb000)
        flash[0x12000:0x20000] = old_nvs
        flash[0x20000:0x22000] = bytes((0x37,)) * 0x2000
        args = self.arguments(flash, "esp32_at")
        args.device_id = None
        args.source_verification_key = None
        args.source_efuse_mac = "01:02:03:04:05:06"
        report = migration.prepare(args)
        self.assertNotIn("device_id", report)
        candidate = (args.output_directory / "candidate-flash.bin").read_bytes()
        old_at = (args.output_directory / "at-old-raw.bin").read_bytes()
        self.assertEqual(candidate[0x3e5000:0x3ea000], old_at)
        self.assertEqual(old_at, flash[0x12000:0x15000] + flash[0x20000:0x22000])
        self.assertEqual(candidate[0x9000:0xf000], b"\xff" * 0x6000)
        self.assertEqual(candidate[0x3fa000:], b"\xff" * 0x6000)
        nonempty_wifi = bytearray(flash)
        nonempty_wifi[0x12000 + wifi_payload_at] = 0x38
        parsed = parser.NVS_Partition("old_at_nvs", nonempty_wifi[0x12000:0x20000])
        for page_index, page in enumerate(parsed.pages):
            for entry in page.entries:
                if entry.state == "Written" and entry.key == "sta.ssid":
                    header_at = 0x12000 + page_index * 0x1000 + 64 + entry.index * 32
                    nonempty_wifi[header_at + 28:header_at + 32] = zlib.crc32(
                        b"".join(bytes(child.raw) for child in entry.children)[:entry.data["size"]], 0xffffffff).to_bytes(4, "little")
                    header = nonempty_wifi[header_at:header_at + 32]
                    nonempty_wifi[header_at + 4:header_at + 8] = zlib.crc32(
                        header[:4] + header[8:], 0xffffffff).to_bytes(4, "little")
        nonempty_args = self.arguments(nonempty_wifi, "esp32_at")
        nonempty_args.device_id = nonempty_args.source_verification_key = None
        nonempty_args.source_efuse_mac = "01:02:03:04:05:06"
        nonempty_args.output_directory = self.directory / "nonempty_wifi"
        with self.assertRaisesRegex(migration.MigrationError, "非空Wi-Fi"):
            migration.prepare(nonempty_args)
        self.assertFalse(nonempty_args.output_directory.exists())
        # Restore the exact original backups before checking the independent MAC rejection.
        for path in (args.backup_a, args.backup_b):
            path.write_bytes(flash)
        args.output_directory = self.directory / "wrong_mac"
        args.source_efuse_mac = "01:02:03:04:05:07"
        with self.assertRaises(migration.MigrationError):
            migration.prepare(args)
        self.assertFalse(args.output_directory.exists())
    def test_output_refuses_existing_or_public_parent_and_failed_archive_readback(self):
        args = self.arguments(self.product_flash("c3_product"), "c3_product")
        args.output_directory.mkdir()
        with self.assertRaises(migration.MigrationError):
            migration.prepare(args)
        self.assertEqual(list(args.output_directory.iterdir()), [])
        public = self.directory / "public"
        public.mkdir(mode=0o755)
        public.chmod(0o755)  # 明确测试公开权限，不依赖调用者 umask。
        args.output_directory = public / "prepared"
        with self.assertRaises(migration.MigrationError):
            migration.prepare(args)
        self.assertFalse(args.output_directory.exists())
        args.output_directory = self.directory / "archive_readback_failure"
        original_read = migration.read_file
        def fail_archive_readback(path, **options):
            raw = original_read(path, **options)
            return raw[:-1] + bytes((raw[-1] ^ 1,)) if Path(path).name == "source-flash.bin" else raw
        with mock.patch.object(migration, "read_file", side_effect=fail_archive_readback):
            with self.assertRaises(migration.MigrationError):
                migration.prepare(args)
        self.assertTrue((args.output_directory / "source-flash.bin").exists())
        self.assertFalse((args.output_directory / "migration-receipt.json").exists())
        self.assertFalse((args.output_directory / "candidate-flash.bin").exists())


if __name__ == "__main__":
    unittest.main()
