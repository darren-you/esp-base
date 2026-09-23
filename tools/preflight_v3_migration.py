#!/usr/bin/env python3
"""只读核对 ESP Base v1/v2 的双份完整 Flash 备份并生成 v3 候选。"""

from __future__ import annotations

import argparse
import base64
import contextlib
import csv
import hashlib
from importlib import metadata, util
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import zlib


FLASH_SIZE = 0x400000
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
OTADATA_OFFSET = 0xF000
OTADATA_SIZE = 0x2000
APP_SIZE = 0x1E0000
APP_OFFSETS = (0x20000, 0x200000)
STORE_OFFSET = 0x3E0000
STORE_SIZE = 0x20000
IDF_COMMIT = "855937cf9dcee13ee9c423fb0319238cdc8d53fd"
NVS_GENERATOR_VERSION = "0.1.9"
UUID_V4 = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z")
ROOT = Path(__file__).resolve().parents[1]


class PreflightError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PreflightError(message)


def private_regular_file(info: os.stat_result) -> None:
    require(stat.S_ISREG(info.st_mode), "备份必须是普通文件，不能是符号链接")
    require(info.st_uid == os.getuid(), "备份必须归当前用户所有")
    require(info.st_mode & 0o177 == 0, "备份权限必须限制为 0600 或更严")
    require(info.st_size == FLASH_SIZE, "备份长度不是完整 4 MiB Flash")


def open_backup(path: Path):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        info = os.fstat(descriptor)
        private_regular_file(info)
        return os.fdopen(descriptor, "rb"), info
    except Exception:
        os.close(descriptor)
        raise


def compare_backups(first: Path, second: Path) -> bytes:
    a_hash, b_hash = hashlib.sha256(), hashlib.sha256()
    contents = bytearray()
    with contextlib.ExitStack() as stack:
        a, a_info = open_backup(first)
        stack.enter_context(a)
        b, b_info = open_backup(second)
        stack.enter_context(b)
        require((a_info.st_dev, a_info.st_ino) != (b_info.st_dev, b_info.st_ino),
                "两份备份必须是独立文件")
        offset = 0
        while offset < FLASH_SIZE:
            left, right = a.read(65536), b.read(65536)
            require(left == right, f"两份完整 Flash 备份在 0x{offset:x} 所在块不一致")
            require(bool(left), "完整 Flash 备份提前结束")
            a_hash.update(left)
            b_hash.update(right)
            contents.extend(left)
            offset += len(left)
        require(not a.read(1) and not b.read(1), "完整 Flash 备份尾部有额外字节")
        for handle, original in ((a, a_info), (b, b_info)):
            current = os.fstat(handle.fileno())
            require((current.st_size, current.st_mtime_ns) == (original.st_size, original.st_mtime_ns),
                    "预检期间完整 Flash 备份发生变化")
    require(a_hash.digest() == b_hash.digest(), "两份备份摘要不一致")
    return bytes(contents)


def check_sdk(idf_path: Path) -> Path:
    result = subprocess.run(
        ["git", "-C", str(idf_path), "rev-parse", "HEAD"],
        text=True, capture_output=True, check=False,
    )
    require(result.returncode == 0 and result.stdout.strip() == IDF_COMMIT,
            "ESP-IDF checkout 不是本仓 sdk-lock.json 固定提交")
    tracked_tools = ["components/partition_table/gen_esp32part.py",
                     "components/nvs_flash/nvs_partition_tool/nvs_parser.py",
                     "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"]
    unchanged = subprocess.run(["git", "-C", str(idf_path), "diff", "--quiet", "HEAD", "--", *tracked_tools],
                               check=False)
    require(unchanged.returncode == 0, "固定 SDK 的分区或 NVS 工具源码已修改")
    components = idf_path / "components"
    require((components / "partition_table/gen_esp32part.py").is_file() and
            (components / "nvs_flash/nvs_partition_tool/nvs_parser.py").is_file(),
            "固定 ESP-IDF 缺少官方分区或 NVS 解析器")
    return components


def check_partition_table(flash: bytes, components: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="esp-base-v2-table-") as temp:
        expected = Path(temp) / "partition-table.bin"
        command = [sys.executable, str(components / "partition_table/gen_esp32part.py"),
                   "--quiet", "--offset", hex(TABLE_OFFSET), "--flash-size", "4MB",
                   str(ROOT / "firmware/partitions/partition_table.csv"), str(expected)]
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        require(result.returncode == 0, "固定 SDK 不能生成仓库声明的分区表")
        table = expected.read_bytes()
    actual = flash[TABLE_OFFSET:TABLE_OFFSET + TABLE_SIZE]
    require(len(table) <= TABLE_SIZE and actual[:len(table)] == table and
            actual[len(table):] == b"\xff" * (TABLE_SIZE - len(table)),
            "Flash 分区表与当前仓库的精确布局不一致")


def load_nvs_parser(components: Path):
    tool_dir = str(components / "nvs_flash/nvs_partition_tool")
    sys.path.insert(0, tool_dir)
    import nvs_parser  # type: ignore[import-not-found]
    return nvs_parser


def validate_page(page, partition_name: str) -> None:
    status = page.header["status"]
    require(status in {"Empty", "Active", "Full"}, f"{partition_name} 含无法安全审计的 NVS 页状态")
    if status == "Empty":
        require(page.raw_header == b"\xff" * 32 and
                page.raw_entry_state_bitmap == b"\xff" * 32 and
                all(entry.state == "Empty" and entry.is_empty for entry in page.entries),
                f"{partition_name} 空白 NVS 页含非空字节")
        return
    require(page.header["version"] in {1, 2}, f"{partition_name} 含未知 NVS 页版本")
    require(page.header["crc"]["original"] == page.header["crc"]["computed"],
            f"{partition_name} NVS 页头 CRC 不符")
    covered = []
    for entry in page.entries:
        covered.append(entry.index)
        covered.extend(child.index for child in entry.children)
        require(entry.state in {"Empty", "Erased", "Written"}, f"{partition_name} NVS 条目状态无效")
        if entry.state == "Empty":
            require(entry.is_empty, f"{partition_name} NVS 空条目含非空字节")
            continue
        metadata = entry.metadata
        require(not entry.is_empty and metadata["span"] >= 1 and
                entry.index + metadata["span"] <= 126,
                f"{partition_name} NVS 已写条目跨度无效")
        require(len(entry.children) == metadata["span"] - 1 and
                all(child.state == entry.state for child in entry.children),
                f"{partition_name} NVS 已写条目跨页或子条目状态无效")
        if entry.state == "Erased":
            continue
        require(metadata["crc"]["original"] == metadata["crc"]["computed"],
                f"{partition_name} NVS 已写条目 CRC 不符")
        if metadata["span"] > 1:
            require(metadata["crc"]["data_original"] == metadata["crc"]["data_computed"],
                    f"{partition_name} NVS 数据 CRC 不符")
        raw_key = bytes(entry.raw[8:24])
        require(b"\0" in raw_key and raw_key.split(b"\0", 1)[1].strip(b"\0") == b"",
                f"{partition_name} NVS 键编码无效")
        try:
            key = raw_key.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise PreflightError(f"{partition_name} NVS 键不是 ASCII") from exc
        require(bool(key), f"{partition_name} NVS 键为空")
    require(sorted(covered) == list(range(126)), f"{partition_name} NVS 页条目覆盖不完整")


def nvs_records(flash: bytes, name: str, offset: int, size: int, parser,
                allowed: set[tuple[str, str]]) -> dict[tuple[str, str], tuple[str, bytes]]:
    partition = parser.NVS_Partition(name, bytearray(flash[offset:offset + size]))
    require(any(page.header["status"] == "Empty" for page in partition.pages),
            f"{name} 没有 NVS 空白页")
    for page in partition.pages:
        validate_page(page, name)
    entries = [entry for page in partition.pages for entry in page.entries if entry.state == "Written"]
    namespaces: dict[int, str] = {}
    for entry in entries:
        metadata = entry.metadata
        if metadata["namespace"] != 0:
            continue
        require(metadata["type"] == "uint8_t" and metadata["span"] == 1 and
                0 < entry.data["value"] < 255 and entry.data["value"] not in namespaces,
                f"{name} namespace 声明无效或重复")
        namespaces[entry.data["value"]] = entry.key
    require(len(namespaces) == len(set(namespaces.values())), f"{name} namespace 名称重复")

    groups: dict[tuple[str, str], list] = {}
    for entry in entries:
        ns_id = entry.metadata["namespace"]
        if ns_id == 0:
            continue
        require(ns_id in namespaces, f"{name} 条目引用未知 namespace")
        groups.setdefault((namespaces[ns_id], entry.key), []).append(entry)
    require(set(groups) <= allowed, f"{name} 含非预期记录；停止迁移并人工确认事实")

    records: dict[tuple[str, str], tuple[str, bytes]] = {}
    for key, group in groups.items():
        types = [entry.metadata["type"] for entry in group]
        if types == ["string"]:
            entry = group[0]
            require(entry.data["size"] > 0, f"{name} 字符串长度无效")
            value = b"".join(bytes(child.raw) for child in entry.children)[:entry.data["size"]]
            require(len(value) == entry.data["size"] and value.endswith(b"\0"),
                    f"{name} 字符串不完整")
            records[key] = ("string", value)
        elif types == ["blob"]:
            entry = group[0]
            value = b"".join(bytes(child.raw) for child in entry.children)[:entry.data["size"]]
            require(len(value) == entry.data["size"], f"{name} 旧式 blob 不完整")
            records[key] = ("blob", value)
        elif types.count("blob_index") == 1 and types.count("blob_data") + 1 == len(types):
            index = next(entry for entry in group if entry.metadata["type"] == "blob_index")
            chunks = [entry for entry in group if entry.metadata["type"] == "blob_data"]
            require(index.metadata["span"] == 1 and index.data["chunk_count"] == len(chunks) and
                    len(chunks) > 0, f"{name} blob 索引与分块不一致")
            ordered = sorted(chunks, key=lambda chunk:
                             (chunk.metadata["chunk_index"] - index.data["chunk_start"]) % 256)
            require([(chunk.metadata["chunk_index"] - index.data["chunk_start"]) % 256
                     for chunk in ordered] == list(range(len(chunks))),
                    f"{name} blob 分块序号不连续")
            value = b"".join(b"".join(bytes(child.raw) for child in chunk.children)[:chunk.data["size"]]
                             for chunk in ordered)
            require(len(value) == index.data["size"], f"{name} blob 总长度不符")
            records[key] = ("blob", value)
        else:
            raise PreflightError(f"{name} 含未知或重复的 NVS 记录类型")
    require(set(namespaces.values()) == {namespace for namespace, _ in records},
            f"{name} 含未使用或未知的 NVS namespace")
    return records


def valid_uuid_text(value: str) -> bool:
    return UUID_V4.fullmatch(value) is not None


def validate_v1_config(blob: bytes) -> int:
    require(len(blob) == 112 and blob[:4] == b"EBCF" and blob[4] == 1,
            "base_config/committed 不是规范 EBCF v1 112B blob")
    configured, ssid_size, password_size = blob[5:8]
    require(configured in {0, 1} and ssid_size <= 32 and password_size <= 64 and
            blob[12:16] == bytes(4) and
            blob[16 + ssid_size:48] == bytes(32 - ssid_size) and
            blob[48 + password_size:112] == bytes(64 - password_size),
            "EBCF v1 长度、标记或保留字节无效")
    ssid, password = blob[16:16 + ssid_size], blob[48:48 + password_size]
    if configured:
        require(1 <= ssid_size <= 32 and 8 <= password_size <= 64,
                "EBCF v1 Wi-Fi 字段长度无效")
        try:
            decoded_ssid = ssid.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise PreflightError("EBCF v1 SSID UTF-8 无效") from exc
        require(all(ord(char) >= 0x20 and ord(char) != 0x7F for char in decoded_ssid),
                "EBCF v1 SSID 含控制字符")
        require(all(0x20 <= char <= 0x7E for char in password) if password_size < 64 else
                all(chr(char) in "0123456789abcdefABCDEF" for char in password),
                "EBCF v1 Wi-Fi 密码格式无效")
    else:
        require(ssid_size == 0 and password_size == 0, "EBCF v1 未配置状态含 Wi-Fi 数据")
    return int.from_bytes(blob[8:12], "little")


def validate_v2_config(blob: bytes) -> int:
    require(24 <= len(blob) <= 4885 and blob[:5] == b"EBCF\x02",
            "base_config/committed 不是规范 EBCF v2 blob")
    flags, ssid_len, wifi_password_len = blob[5:8]
    host_len, user_len = blob[12:14]
    mqtt_password_len = int.from_bytes(blob[14:16], "little")
    ca_len = int.from_bytes(blob[16:18], "little")
    port = int.from_bytes(blob[18:20], "little")
    require(flags <= 3 and ssid_len <= 32 and wifi_password_len <= 64 and
            host_len <= 253 and user_len <= 128 and mqtt_password_len <= 256 and
            ca_len <= 4096 and blob[20:24] == bytes(4) and
            len(blob) == 24 + ssid_len + wifi_password_len + host_len + user_len +
            mqtt_password_len + ca_len + (32 if flags & 2 else 0),
            "EBCF v2 字段长度、标记或保留字节无效")
    at = 24
    def take(length: int) -> bytes:
        nonlocal at
        value = blob[at:at + length]
        at += length
        return value
    ssid = take(ssid_len)
    wifi_password = take(wifi_password_len)
    host = take(host_len)
    user = take(user_len)
    mqtt_password = take(mqtt_password_len)
    ca = take(ca_len)
    key = take(32) if flags & 2 else bytes(32)
    require(at == len(blob), "EBCF v2 长度不完整")
    try:
        config = {
            "schema_version": 3,
            "wifi": {"ssid": ssid.decode("utf-8"), "password": wifi_password.decode("ascii")}
                    if flags & 1 else None,
            "mqtt": {"hostname": host.decode("ascii"), "port": port,
                     "username": user.decode("utf-8"), "password": mqtt_password.decode("utf-8"),
                     "ca_pem": ca.decode("ascii"), "management_key_hex": key.hex()}
                    if flags & 2 else None,
            "frp": None, "business": None,
        }
        if not flags & 1:
            require(not ssid_len and not wifi_password_len, "EBCF v2 未配置 Wi-Fi 却含数据")
        if not flags & 2:
            require(not (host_len or user_len or mqtt_password_len or ca_len or port),
                    "EBCF v2 未配置 MQTT 却含数据")
        spec = util.spec_from_file_location("esp_base_device_control", ROOT / "tools/device-control.py")
        require(spec is not None and spec.loader is not None, "设备配置校验器不可用")
        module = util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.validate_configuration(config)
    except (UnicodeError, ValueError) as exc:
        raise PreflightError("EBCF v2 配置值无效") from exc
    return int.from_bytes(blob[8:12], "little")


def validate_ota_receipt(blob: bytes, device_id: str) -> str:
    require(len(blob) == 118 and blob[:5] == b"EOTA\x01" and blob[9] == 0,
            "base_ota/operation 不是规范 EOTA v1 118B blob")
    status, source, target, failure = blob[5:9]
    require(status in {1, 2} and (source, target) in {(0x10, 0x11), (0x11, 0x10)} and
            (status != 1 or failure == 0) and (status != 2 or failure in {1, 2, 3, 4, 5, 6, 7, 8, 10}) and
            0 < int.from_bytes(blob[10:14], "little") <= 0x1E0000,
            "EOTA v1 状态、槽或长度无效")
    try:
        receipt_device = blob[14:50].decode("ascii")
        operation_id = blob[50:86].decode("ascii")
    except UnicodeDecodeError as exc:
        raise PreflightError("EOTA v1 UUID 编码无效") from exc
    require(receipt_device == device_id and valid_uuid_text(operation_id),
            "EOTA v1 设备身份或 operation ID 无效")
    return "prepared" if status == 1 else "failed"


def audit_otadata_and_slots(flash: bytes) -> list[str]:
    state_names = {0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID",
                   4: "ABORTED", 0xFFFFFFFF: "UNDEFINED"}
    ota = flash[OTADATA_OFFSET:OTADATA_OFFSET + OTADATA_SIZE]
    require(len(ota) == OTADATA_SIZE, "otadata 分区长度不完整")
    entries = []
    for index in range(2):
        raw = ota[index * 0x1000:index * 0x1000 + 32]
        if raw == b"\xff" * 32:
            entries.append(None)
            continue
        sequence, state, crc = (int.from_bytes(raw[:4], "little"),
                                int.from_bytes(raw[24:28], "little"),
                                int.from_bytes(raw[28:32], "little"))
        require(sequence not in {0, 0xFFFFFFFF} and state in state_names and
                zlib.crc32(raw[:4], 0xFFFFFFFF) == crc,
                f"otadata 副本 {index} 的序列、状态或 CRC 无效")
        entries.append((sequence, state))
    require(all(item is None or item[1] not in {0, 1} for item in entries),
            "otadata 含 NEW/PENDING_VERIFY 未决槽；阻断 v3 维护准备")
    selectable = [(index, item[0], item[1]) for index, item in enumerate(entries)
                  if item is not None and item[1] not in {3, 4}]
    require(selectable, "otadata 没有可选择的应用槽")
    selected_index, selected_sequence, selected_state = max(selectable, key=lambda item: (item[1], -item[0]))
    selected_slot = (selected_sequence - 1) % 2
    require(selected_state == 2, "当前 otadata 选择器不是 VALID；阻断 v3 维护准备")
    lines = []
    for slot, offset in enumerate(APP_OFFSETS):
        image = flash[offset:offset + APP_SIZE]
        require(len(image) == APP_SIZE, f"ota_{slot} 分区长度不完整")
        status = "empty" if image == b"\xff" * APP_SIZE else "image-header" if image[0] == 0xE9 else "unrecognized"
        if slot == selected_slot:
            require(status == "image-header", "otadata 选中的应用槽没有 ESP 镜像头")
        matching = [(index, seq, state) for index, item in enumerate(entries) if item is not None
                    for seq, state in [item] if (seq - 1) % 2 == slot]
        newest = max(matching, key=lambda item: item[1]) if matching else None
        state_text = state_names[newest[2]] if newest else "NO_RECORD"
        lines.append(f"ota_{slot}: {status}, otadata={state_text}, 全槽 SHA-256={hashlib.sha256(image).hexdigest()}")
    lines.append(f"otadata: 选中副本 {selected_index} / ota_{selected_slot} / 序列 {selected_sequence} / VALID；仅为离线选择器解析")
    return lines


def audit(flash: bytes, components: Path, device_id: str) -> tuple[list[str], dict[tuple[str, str], tuple[str, bytes]]]:
    check_partition_table(flash, components)
    ota_lines = audit_otadata_and_slots(flash)
    parser = load_nvs_parser(components)
    identity = nvs_records(flash, "nvs", NVS_OFFSET, NVS_SIZE, parser,
                           {("base_identity", "device_uuid")})
    store = nvs_records(flash, "base_store", STORE_OFFSET, STORE_SIZE, parser,
                        {("base_config", "committed"), ("base_ota", "operation")})
    require(set(identity) == {("base_identity", "device_uuid")},
            "默认 nvs 含非预期记录；停止迁移并人工确认事实")
    require(set(store) <= {("base_config", "committed"), ("base_ota", "operation")},
            "base_store 含非预期记录；停止迁移并人工确认事实")
    kind, raw_id = identity[("base_identity", "device_uuid")]
    require(kind == "string" and len(raw_id) == 37 and raw_id[-1:] == b"\0" and
            raw_id[:-1].decode("ascii", errors="replace") == device_id and valid_uuid_text(device_id),
            "默认 nvs 的设备 UUID 与本轮绑定身份不一致")
    lines = ["nvs/base_identity/device_uuid: string, 37B，身份匹配"]
    if ("base_config", "committed") in store:
        kind, blob = store[("base_config", "committed")]
        require(kind == "blob", "base_config/committed 不是 blob")
        if len(blob) >= 5 and blob[4] == 1:
            revision = validate_v1_config(blob)
            lines.append(f"base_store/base_config/committed: EBCF v1, 112B, revision={revision}")
        else:
            revision = validate_v2_config(blob)
            lines.append(f"base_store/base_config/committed: EBCF v2, {len(blob)}B, revision={revision}")
    else:
        lines.append("base_store/base_config/committed: 缺失，旧固件读取为默认空配置")
    if ("base_ota", "operation") in store:
        kind, blob = store[("base_ota", "operation")]
        require(kind == "blob", "base_ota/operation 不是 blob")
        state = validate_ota_receipt(blob, device_id)
        lines.append(f"base_store/base_ota/operation: EOTA v1, 118B, {state}；须独立核对 otadata 与运行槽")
    else:
        lines.append("base_store/base_ota/operation: 缺失")
    return ota_lines + lines, store


def convert_v1_wifi_only(blob: bytes) -> bytes:
    validate_v1_config(blob)
    header = bytearray(40)
    header[:4] = b"EBCF"
    header[4] = 3
    header[5] = blob[5]  # 仅 Wi-Fi bit；MQTT/FRP 保持 absent
    header[6:8] = blob[6:8]
    header[8:12] = blob[8:12]  # 纯格式转换：revision 原样保留
    return bytes(header) + blob[16:16 + blob[6]] + blob[48:48 + blob[7]]


def convert_v2_preserving_mqtt(blob: bytes) -> bytes:
    validate_v2_config(blob)
    header = bytearray(40)
    header[:20] = blob[:20]
    header[4] = 3
    return bytes(header) + blob[24:]


def write_candidate(output: Path, store: dict[tuple[str, str], tuple[str, bytes]], components: Path) -> str:
    root = ROOT.resolve()
    destination = output.resolve()
    require(destination != root and root not in destination.parents,
            "敏感 base_store 候选不能保存到 Git 仓库内")
    require(output.parent.is_dir() and not output.exists(), "候选输出目录不存在或目标文件已存在")
    try:
        installed = metadata.version("esp-idf-nvs-partition-gen")
    except metadata.PackageNotFoundError as exc:
        raise PreflightError("缺少固定 SDK 使用的官方 NVS generator Python 包") from exc
    require(installed == NVS_GENERATOR_VERSION,
            "官方 NVS generator 版本与本次已核对版本不一致")
    old = store.get(("base_config", "committed"))
    if old:
        new_config = (convert_v1_wifi_only(old[1]) if old[1][4] == 1 else
                      convert_v2_preserving_mqtt(old[1]))
    else:
        new_config = convert_v1_wifi_only(b"EBCF\x01" + bytes(107))
    receipt = store.get(("base_ota", "operation"))
    with tempfile.TemporaryDirectory(prefix="esp-base-v3-store-") as temp:
        work = Path(temp)
        source = work / "records.csv"
        generated = work / "base-store.bin"
        descriptor = os.open(source, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(("key", "type", "encoding", "value"))
            writer.writerow(("base_config", "namespace", "", ""))
            writer.writerow(("committed", "data", "base64", base64.b64encode(new_config).decode("ascii")))
            if receipt:
                writer.writerow(("base_ota", "namespace", "", ""))
                writer.writerow(("operation", "data", "base64", base64.b64encode(receipt[1]).decode("ascii")))
        generator = components / "nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        result = subprocess.run([sys.executable, str(generator), "generate", "--version", "2",
                                 str(source), str(generated), hex(STORE_SIZE)],
                                capture_output=True, check=False)
        require(result.returncode == 0 and generated.is_file(), "官方 NVS generator 未能生成候选")
        candidate = generated.read_bytes()
    require(len(candidate) == STORE_SIZE, "官方 NVS generator 输出的 base_store 长度不符")
    parser = load_nvs_parser(components)
    reread = nvs_records(candidate, "candidate_base_store", 0, STORE_SIZE, parser,
                         {("base_config", "committed"), ("base_ota", "operation")})
    expected = {("base_config", "committed"): ("blob", new_config)}
    if receipt:
        expected[("base_ota", "operation")] = receipt
    require(reread == expected, "候选镜像逐键解析读回与迁移输入不一致")
    descriptor, temporary = tempfile.mkstemp(prefix=".esp-base-v3-", dir=output.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(candidate)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, output)
    finally:
        os.unlink(temporary)
    require(output.stat().st_size == STORE_SIZE and output.read_bytes() == candidate,
            "候选输出读回字节不一致")
    return hashlib.sha256(candidate).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup-a", required=True, type=Path, help="第一份完整 4 MiB Flash 备份")
    parser.add_argument("--backup-b", required=True, type=Path, help="第二份完整 4 MiB Flash 备份")
    parser.add_argument("--idf-path", required=True, type=Path, help="sdk-lock.json 固定的 ESP-IDF checkout")
    parser.add_argument("--device-id", required=True, help="本轮已独立核对的设备 UUID v4")
    parser.add_argument("--output-base-store", type=Path,
                        help="可选：在仓外创建经逐键读回验证的 v3 base_store 分区候选；不写设备")
    args = parser.parse_args()
    try:
        require(valid_uuid_text(args.device_id), "device-id 必须为小写 UUID v4")
        components = check_sdk(args.idf_path)
        flash = compare_backups(args.backup_a, args.backup_b)
        records, store = audit(flash, components, args.device_id)
        candidate_sha = write_candidate(args.output_base_store, store, components) if args.output_base_store else None
    except (OSError, PreflightError) as exc:
        print(f"ESP Base v1/v2→v3 只读预检：阻断；{exc}", file=sys.stderr)
        return 1
    except Exception:
        print("ESP Base v1/v2→v3 只读预检：阻断；固定 SDK 解析失败", file=sys.stderr)
        return 1
    print("ESP Base v1/v2→v3 只读预检：通过")
    print(f"完整 Flash：两份独立备份逐字节一致；4 MiB；SHA-256 {hashlib.sha256(flash).hexdigest()}")
    print("分区表：与仓库固定布局逐字节一致")
    for record in records:
        print(f"{'Flash' if record.startswith(('ota_', 'otadata:')) else 'NVS'}：{record}")
    if candidate_sha:
        print(f"v3 base_store 候选：0x20000 字节、0600、逐键读回相等；SHA-256 {candidate_sha}")
    print("只读预检不批准写入；镜像头和选择器解析不能证明镜像可启动。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
