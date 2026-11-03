#!/usr/bin/env python3
"""离线准备原生 Base 的三个有线 OTA 阶段；不连接设备或授予写入资格。"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zlib

import build_package
import prepare_native_layout as native

SECTOR = 0x1000
FAILED = 2
PREPARED = 1
NEW = 0
VALID = 2
OPERATION_KEY = native.NATIVE_OPERATION_KEY


def require(condition, message):
    native.require(condition, message)


def digest_argument(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None and
            value != "0" * 64, "摘要必须是非零小写 SHA-256")
    return value


def source_selector(flash, layout):
    """保留物理页身份，不合并同槽记录，拒绝 IDF 状态查询有歧义的来源。"""
    entries = []
    for page in range(2):
        at = layout["otadata"] + page * SECTOR
        row = flash[at:at + 32]
        if row == b"\xff" * 32:
            entries.append(None)
            continue
        sequence, state = native.u32(row), native.u32(row, 24)
        require(sequence not in (0, 0xffffffff) and state in (2, 3, 4) and
                native.u32(row, 28) == zlib.crc32(row[:4], 0xffffffff),
                "来源 otadata 损坏、未知、NEW 或 PENDING")
        entries.append({"page": page, "sequence": sequence,
                        "slot": (sequence - 1) % 2, "state": state})
    present = [entry for entry in entries if entry is not None]
    require(len({entry["slot"] for entry in present}) == len(present),
            "来源两个物理 otadata 记录映射同一槽，拒绝状态歧义")
    active = [entry for entry in present if entry["state"] == VALID]
    require(len(active) == 1, "仅接受一个 VALID A 与已清理的非 VALID 备用槽")
    source = active[0]
    target = 1 - source["slot"]
    other = entries[1 - source["page"]]
    require(other is None or other["slot"] == target and other["state"] in (3, 4),
            "备用槽仍 VALID、未决或来源不符")
    return source, target, entries


def new_selector(flash, layout, source, target, entries):
    sequence = max(entry["sequence"] for entry in entries if entry is not None) + 1
    if (sequence - 1) % 2 != target:
        sequence += 1
    require(sequence < 0xffffffff, "otadata 序号不能安全递增，停止准备")
    at = layout["otadata"] + (1 - source["page"]) * SECTOR
    sector = bytearray(flash[at:at + SECTOR])
    sector[:4] = sequence.to_bytes(4, "little")
    sector[24:28] = NEW.to_bytes(4, "little")
    sector[28:32] = zlib.crc32(sector[:4], 0xffffffff).to_bytes(4, "little")
    return at, bytes(sector), sequence


def prepared_receipt(device_id, operation_id, source, target, source_sha, app):
    # inactive 已物理清理，因此真实旧 B 身份为零；不能继承前次失败的旧 B。
    raw = bytearray(182)
    raw[:10] = b"EOTA\x04" + bytes((PREPARED, 0x10 + source, 0x10 + target, 0, 0))
    raw[10:14] = len(app).to_bytes(4, "little")
    raw[14:50], raw[50:86] = device_id.encode(), operation_id.encode()
    raw[86:118] = bytes.fromhex(native.digest(app))
    raw[118:150] = bytes.fromhex(source_sha)
    return bytes(raw)


def c3_runtime_trust(images, key, work):
    # espsecure verify-signature 使用外部 PEM 验签，不核内嵌 RSA 加速参数。
    # IDF 无 eFuse 信任时却用运行镜像内完整 ets_rsa_pubkey_t 的摘要授权 C。
    output = work / "rsa-key-digest.bin"
    result = subprocess.run([sys.executable, "-m", "espsecure", "digest-sbv2-public-key",
                             "--keyfile", str(key), "--output", str(output)],
                            capture_output=True, check=False)
    require(result.returncode == 0, "官方 RSA 公钥摘要生成失败")
    expected = native.read_file(output, maximum=32)
    require(len(expected) == 32, "官方 RSA 公钥摘要长度错误")
    for image in images:
        require(len(image) >= SECTOR and len(image) % SECTOR == 0, "C3 签名扇区长度不符")
        body_digest = hashlib.sha256(image[:-SECTOR]).digest()
        count = 0
        for number in range(3):
            block = image[-SECTOR + number * 1216:][:1216]
            if block == b"\xff" * 1216:
                continue
            require(block[:2] == b"\xe7\x02" and
                    native.u32(block, 1196) == zlib.crc32(block[:1196]) and
                    block[4:36] == body_digest and
                    hashlib.sha256(block[36:812]).digest() == expected,
                    "C3 内嵌 RSA 签名块信任不符，不能仅凭外部 PEM 验签放行")
            count += 1
        require(count > 0, "C3 缺少同授权信任的 RSA 签名块")
    return expected.hex()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode()


def prepare(args):
    require(args.source_layout in ("c3_native", "esp32_native"), "只接受当前原生布局")
    for value in (args.device_id, args.previous_operation_id, args.operation_id):
        require(native.legacy.valid_uuid_text(value), "UUID 必须是规范小写 v4")
    require(args.operation_id != args.previous_operation_id, "新操作 ID 不能复用原失败 ID")
    for value in (args.source_app_sha256, args.image_sha256, args.verification_key_sha256):
        digest_argument(value)
    components = native.legacy.check_sdk(args.idf_path)
    flash = native.legacy.compare_backups(args.backup_a, args.backup_b)
    layout = native.LAYOUTS[args.source_layout]
    target_name = layout["target"]
    source, target, entries = source_selector(flash, layout)
    target_at = layout["apps"][target]
    require(flash[target_at:target_at + SECTOR] == b"\xff" * SECTOR,
            "备用槽首 4KiB 未完整擦除，拒绝冒充失败清理完成")

    with tempfile.TemporaryDirectory(prefix="wired_ota_prepare_") as scratch:
        work = Path(scratch)
        # 单一公钥稳定快照验证 A/C，不提供换信任入口，也不归档私钥。
        key = work / "verification-key.bin"
        key_bytes = native.read_file(args.verification_key, maximum=4096)
        require(native.digest(key_bytes) == args.verification_key_sha256,
                "验签公钥与独立授权摘要不符")
        native.write_verified(key, key_bytes)
        require(build_package.public_key(key, target_name) == key_bytes, "验签公钥格式不符")
        records, inspected, _ = native.inspect_source(flash, args.source_layout,
            args.device_id, key, components, work)
        require(inspected["selected_slot"] == "ota_" + str(source["slot"]) and
                inspected["old_operation"] == {"operation_id": args.previous_operation_id,
                                               "state": "failed"},
                "来源必须是独立绑定的原 V4 FAILED 终态")
        old_receipt = records[OPERATION_KEY][1]
        require(old_receipt[5] == FAILED and old_receipt[6] == 0x10 + source["slot"] and
                old_receipt[7] == 0x10 + target and
                old_receipt[118:150].hex() == args.source_app_sha256,
                "原失败收据来源槽或独立 A 摘要不符")
        source_at = layout["apps"][source["slot"]]
        source_app = native.signed_image(flash[source_at:source_at + native.APP_SIZE], target_name)
        require(native.digest(source_app) == args.source_app_sha256, "签名 A 与独立来源摘要不符")
        app = native.read_file(args.image, maximum=native.APP_SIZE)
        require(native.signed_image(app, target_name) == app and
                native.digest(app) == args.image_sha256 and args.image_sha256 != args.source_app_sha256,
                "C 必须是不同于 A 的精确完整签名 Base 镜像及独立候选摘要")
        native.verify_signature(app, key, target_name, work, "candidate")
        runtime_key_digest = None
        if target_name == "esp32c3":
            runtime_key_digest = c3_runtime_trust((source_app, app), key, work)
        if target_name == "esp32":
            boot = native.signed_image(flash[0x1000:0x8000], target_name, app=False, signed=False)
            require(all(key_bytes in image for image in (source_app, app, boot)),
                    "ESP32 A/C 与保留 bootloader 必须嵌入同一 ECDSA 公钥")

        intent = prepared_receipt(args.device_id, args.operation_id, source["slot"],
                                  target, args.source_app_sha256, app)
        updated = dict(records)
        updated[OPERATION_KEY] = ("blob", intent)
        store_at, store_size = layout["store"]
        store = native.generate_store(updated, store_size, components,
                                      native.legacy.load_nvs_parser(components), work)
        selector_at, selector, sequence = new_selector(flash, layout, source, target, entries)

        # Archive/readback is a prerequisite even for emitting offline stage files.
        native.private_output_directory(args.output_directory)
        native.write_verified(args.output_directory / "source-flash.bin", flash)
        native.write_verified(args.output_directory / "previous-operation.bin", old_receipt)
        native.write_verified(args.output_directory / "verification-key.bin", key_bytes)
        archive = {"schema_version": 1, "device_id": args.device_id,
                   "previous_operation_id": args.previous_operation_id, "previous_state": "failed",
                   "source_flash_sha256": native.digest(flash),
                   "previous_receipt_sha256": native.digest(old_receipt),
                   "source_app_sha256": args.source_app_sha256,
                   "verification_key_sha256": args.verification_key_sha256}
        native.write_verified(args.output_directory / "archive.json", json_bytes(archive))

        stages = []
        observed = bytearray(flash)
        stage_inputs = (("01-intent-base_store.bin", store_at, store, "prepared_source_selected"),
                        ("02-inactive-slot.bin", target_at,
                         app + b"\xff" * (native.APP_SIZE - len(app)), "prepared_candidate_unselected"),
                        ("03-select-new.bin", selector_at, selector, "prepared_candidate_new"))
        for number, (name, at, data, boundary) in enumerate(stage_inputs, 1):
            before = native.digest(observed)
            previous_range = bytes(observed[at:at + len(data)])
            observed[at:at + len(data)] = data
            native.write_verified(args.output_directory / name, data)
            stages.append({"stage": number, "file": name, "address": at, "size_bytes": len(data),
                           "sha256": native.digest(data), "before_range_sha256": native.digest(previous_range),
                           "before_flash_sha256": before, "after_flash_sha256": native.digest(observed),
                           "requires_previous_stage_complete_readback": number > 1,
                           "requires_complete_readback_before_next_stage_or_reset": True,
                           "complete_readback_boundary": boundary,
                           "partial_write_state": "unknown",
                           "partial_write_recovery": "explicit_original_full_flash_restore",
                           "automatic_retry_or_resume": False})
        plan = {"schema_version": 1, "artifact_type": "esp_base_wired_ota_stages",
                "state": "software_only", "hardware_write_authorized": False, "device_verified": False,
                **archive, "source_layout": args.source_layout, "target": target_name + "/esp_base",
                "idf_commit": native.legacy.IDF_COMMIT, "operation_id": args.operation_id,
                "image_sha256": args.image_sha256, "image_size_bytes": len(app),
                "source_slot": "ota_" + str(source["slot"]), "target_slot": "ota_" + str(target),
                "source_otadata_sector_address": layout["otadata"] + source["page"] * SECTOR,
                "new_otadata_sequence": sequence, "new_otadata_state": "NEW",
                "prepared_receipt_sha256": native.digest(intent), "config_revision": inspected["config_revision"],
                "c3_runtime_key_digest": runtime_key_digest,
                "preserved_records": {"/".join(k): {"type": kind, "sha256": native.digest(value)}
                                      for k, (kind, value) in records.items() if k != OPERATION_KEY},
                "nvs_write_atomicity": "whole_base_store_rebuild_not_application_nvs_commit",
                "otadata_crc_scope": "sequence_only_not_state_or_full_sector",
                "partial_nvs_or_otadata_write_may_prevent_application_recovery": True,
                "old_operation_after_new_intent": "not_found_on_device_original_failed_archived",
                "requires_fresh_same_device_rom_lease_and_security_recheck": True,
                "required_external_preconditions": {
                    "verified_by_this_offline_tool": False,
                    "fresh_capture_and_same_physical_device": True,
                    "exclusive_rom_session_without_intermediate_boot": True,
                    "authorized_production_source_candidate_and_public_key": True,
                    "esp32_runtime_key_symbols_in_source_candidate_and_retained_bootloader":
                        target_name == "esp32",
                    "esp32_key_substring_checks_are_not_runtime_symbol_proof": target_name == "esp32"},
                "must_not_reset_between_stages": True,
                "must_not_batch_three_stages_as_generic_flash_files": True,
                "success_requires": "device_30s_local_confirmation_persistent_receipt_and_independent_queries",
                "stages": stages}
        native.write_verified(args.output_directory / "plan.json", json_bytes(plan))
        return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-layout", choices=("c3_native", "esp32_native"), required=True)
    for name in ("backup-a", "backup-b", "idf-path", "verification-key", "image", "output-directory"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("device-id", "previous-operation-id", "operation-id", "source-app-sha256",
                 "image-sha256", "verification-key-sha256"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        prepare(args)
    except (native.MigrationError, native.legacy.PreflightError, OSError, ValueError) as error:
        parser.exit(1, "有线 OTA 离线准备阻断：" + str(error) + "\n")
    print("已生成三个分开的软件阶段；未连接设备、未授权硬件写入。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
