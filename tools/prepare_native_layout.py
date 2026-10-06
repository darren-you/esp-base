#!/usr/bin/env python3
"""仅在仓外生成一次性原生布局候选及旧终态归档；不连接或写入设备。"""
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import zlib

import archive_esp32_at as at_archive
import device_control
import preflight_v3_migration as legacy

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x400000
TABLE_OFFSET = 0x8000
APP_OFFSETS = (0x20000, 0x200000)
APP_SIZE = 0x1e0000
RETIRED_KEYS = {("base_ota", "operation"), ("base_pkg", "slots"),
                ("base_product", "operations")}
CONFIG_KEY = ("base_config", "committed")
LAYOUTS = {
    "c3_product": {"target": "esp32c3", "apps": (0x20000, 0x150000), "app_size": 0x130000,
                   "otadata": 0xf000, "store": (0x3f5000, 0xb000), "packages": (0x280000, 0x165000),
                   "scratch": 0x3e5000, "phy": 0x11000},
    "esp32_product": {"target": "esp32", "apps": (0x20000, 0x140000), "app_size": 0x120000,
                      "otadata": 0x10000, "store": (0x3fa000, 0x6000), "packages": (0x260000, 0x186000),
                      "scratch": 0x3ea000, "phy": 0xf000},
    "c3_v1": {"target": "esp32c3", "apps": APP_OFFSETS, "app_size": APP_SIZE,
              "otadata": 0xf000, "store": (0x3e0000, 0x20000), "phy": 0x11000},
    "esp32_at": {"target": "esp32"},
}


class MigrationError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise MigrationError(message)


def u32(raw, offset=0):
    return int.from_bytes(raw[offset:offset + 4], "little")


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def read_file(path, *, private=False, maximum=FLASH_SIZE):
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode) and before.st_size <= maximum,
                "输入必须是有限长度普通文件")
        if private:
            require(before.st_uid == os.getuid() and before.st_mode & 0o177 == 0,
                    "敏感输入必须归当前用户所有且权限为0600或更严")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            raw = source.read(maximum + 1)
        after = os.fstat(descriptor)
        require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns) and len(raw) == before.st_size,
                "读取期间输入发生变化")
        return raw
    finally:
        os.close(descriptor)


def load_partition_module(components):
    path = components / "partition_table/gen_esp32part.py"
    spec = importlib.util.spec_from_file_location("native_partition_table", path)
    require(spec is not None and spec.loader is not None, "固定SDK分区解析器不可用")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_table(raw, components):
    module = load_partition_module(components)
    try:
        table = module.PartitionTable.from_binary(raw[:0xc00])
        table.verify()
        table.verify_size_fits(FLASH_SIZE)
    except Exception as error:
        raise MigrationError("分区表解析、MD5或几何无效") from error
    entries = {p.name: (p.type, p.subtype, p.offset, p.size, int(p.encrypted) | (int(p.readonly) << 1)) for p in table}
    require(len(entries) == len(table), "分区标签重复")
    require(all(not p.encrypted for p in table), "不支持加密分区迁入")
    return entries


def table_from_csv(path, components):
    with tempfile.TemporaryDirectory(prefix="native_layout_table_") as work:
        output = Path(work, "table.bin")
        result = subprocess.run([sys.executable, str(components / "partition_table/gen_esp32part.py"),
            "--quiet", "--offset", "0x8000", "--flash-size", "4MB", str(path), str(output)],
            capture_output=True, check=False)
        require(result.returncode == 0, "固定SDK无法生成目标分区表")
        return output.read_bytes()


def check_old_layout(flash, layout, components):
    table = parse_table(flash[TABLE_OFFSET:TABLE_OFFSET + 0x1000], components)
    if layout is LAYOUTS["esp32_at"]:
        at_archive.check_old_table(flash)
        return table
    expected = {"nvs": (1, 2, 0x9000, 0x6000), "phy_init": (1, 1, layout["phy"], 0x1000),
                "otadata": (1, 0, layout["otadata"], 0x2000),
                "ota_0": (0, 0x10, layout["apps"][0], layout["app_size"]),
                "ota_1": (0, 0x11, layout["apps"][1], layout["app_size"]),
                "base_store": (1, 2, *layout["store"]), "coredump": (1, 3, 0x12000, 0xe000)}
    if "packages" in layout:
        expected.update(coredump=(1, 3, 0x12000, 0xe000),
                        product_pkgs=(1, 6, *layout["packages"]),
                        frp_scratch=(1, 6, layout["scratch"], 0x10000))
        if layout["target"] == "esp32":
            expected["at_old_raw"] = (1, 6, 0x3e6000, 0x4000)
    require(set(table) == set(expected) and all(table[name][:4] == fields for name, fields in expected.items()),
            "源Flash不是指定的精确旧布局")
    require(all(fields[4] == (2 if name == "at_old_raw" else 0) for name, fields in table.items()),
            "旧布局分区flags不符")
    return table


def ota_selection(flash, layout):
    states = {}
    for index in range(2):
        at = layout["otadata"] + index * 0x1000
        raw = flash[at:at + 32]
        if raw == b"\xff" * 32:
            continue
        sequence, state = u32(raw), u32(raw, 24)
        require(sequence not in {0, 0xffffffff} and state in {2, 3, 4} and
                u32(raw, 28) == zlib.crc32(raw[:4], 0xffffffff),
                "旧otadata损坏、NEW/PENDING或未定义，停止迁入")
        slot = (sequence - 1) % 2
        if slot not in states or sequence > states[slot][0]:
            states[slot] = (sequence, state)
    valid = [(sequence, slot) for slot, (sequence, state) in states.items() if state == 2]
    require(bool(valid), "旧otadata没有VALID启动槽")
    return max(valid)[1], {slot: state for slot, (_, state) in states.items()}


def signed_image(raw, target, *, app=True, signed=True):
    require(len(raw) >= 24 and raw[0] == 0xe9 and 1 <= raw[1] <= 16 and
            int.from_bytes(raw[12:14], "little") == (5 if target == "esp32c3" else 0),
            "固件镜像头或芯片不符")
    at = 24
    checksum = 0xef
    for _ in range(raw[1]):
        require(at + 8 <= len(raw), "镜像段头不完整")
        size = u32(raw, at + 4)
        start = at + 8
        at = start + size
        require(at <= len(raw), "镜像段越界")
        for byte in raw[start:at]:
            checksum ^= byte
    body_end = ((at + 1 + 15) // 16) * 16
    require(body_end <= len(raw) and raw[body_end - 1] == checksum, "镜像checksum无效")
    require(raw[23] in {0, 1}, "镜像hash标志无效")
    if raw[23]:
        require(body_end + 32 <= len(raw) and hashlib.sha256(raw[:body_end]).digest() == raw[body_end:body_end + 32],
                "镜像完整摘要不符")
        body_end += 32
    if app:
        require(len(raw) >= 288 and u32(raw, 32) == 0xabcd5432 and
                raw[80:112].split(b"\0", 1)[0] == b"esp_base", "镜像不是esp_base产品")
        end = (((body_end + 4095) // 4096) * 4096 + 4096 if target == "esp32c3" else body_end + 68) if signed else body_end
    else:
        end = body_end
    require(end <= len(raw), "完整签名镜像超出源槽")
    return raw[:end]


def verify_signature(raw, key, target, work, stem):
    require(key is not None, "Base旧/新固件必须提供实际验签公钥")
    image = work / (stem + ".bin")
    image.write_bytes(raw)
    result = subprocess.run([sys.executable, "-m", "espsecure", "verify-signature", "--version",
                             "2" if target == "esp32c3" else "1", "--keyfile", str(key), str(image)],
                            capture_output=True, check=False)
    require(result.returncode == 0, "官方espsecure验签失败或工具不可用")


def validate_v3_config(blob):
    require(40 <= len(blob) <= 7618 and blob[:5] == b"EBCF\x03" and blob[5] <= 7 and
            blob[32:40] == bytes(8), "活动配置不是规范EBCF V3")
    flags = blob[5]
    lengths = [blob[6], blob[7], blob[12], blob[13], int.from_bytes(blob[14:16], "little"),
               int.from_bytes(blob[16:18], "little")]
    frp_lengths = [blob[20], int.from_bytes(blob[22:24], "little"),
                   int.from_bytes(blob[24:26], "little"), blob[21]]
    at = 40
    def take(size, encoding):
        nonlocal at
        value = blob[at:at + size]
        require(len(value) == size, "配置字段不完整")
        at += size
        return value.decode(encoding)
    try:
        ssid, wifi_password, host, user, password, ca = [take(n, encoding) for n, encoding in
            zip(lengths, ["utf-8", "ascii", "ascii", "utf-8", "utf-8", "ascii"])]
        mqtt_key = take(32, "latin1").encode("latin1").hex() if flags & 2 else ""
        frp_host, token, frp_ca, proxy = [take(n, "ascii") for n in frp_lengths]
        frp_key = take(32, "latin1").encode("latin1").hex() if flags & 4 else ""
        mqtt_port = int.from_bytes(blob[18:20], "little")
        frp_ports = [int.from_bytes(blob[n:n + 2], "little") for n in (26, 28, 30)]
        require(at == len(blob), "配置长度或尾部无效")
        require(bool(flags & 1) or not (ssid or wifi_password), "未配置Wi-Fi含活动字段")
        require(bool(flags & 2) or not (host or user or password or ca or mqtt_port), "未配置MQTT含活动字段")
        require(bool(flags & 4) or not (frp_host or token or frp_ca or proxy or any(frp_ports)), "未配置FRP含活动字段")
        device_control.validate_configuration({"schema_version": 3,
            "wifi": {"ssid": ssid, "password": wifi_password} if flags & 1 else None,
            "mqtt": {"hostname": host, "port": mqtt_port, "username": user, "password": password,
                     "ca_pem": ca, "management_key_hex": mqtt_key} if flags & 2 else None,
            "frp": {"server_hostname": frp_host, "server_port": frp_ports[0], "token": token,
                    "ca_pem": frp_ca, "proxy_name": proxy, "remote_port": frp_ports[1],
                    "local_port": frp_ports[2], "management_key_hex": frp_key} if flags & 4 else None,
            "business": None})
    except (UnicodeError, ValueError) as error:
        raise MigrationError("活动配置V3字段无效") from error
    return u32(blob, 8)


def validate_ledger(blob, ecs_sequence):
    require(len(blob) == 910 and blob[:5] == b"EPRD\x01" and blob[5] <= 8 and
            u32(blob, 906) == zlib.crc32(blob[:906]), "旧产品账本格式或CRC无效")
    count, high = blob[5], u32(blob, 6)
    require((count == 0) == (high == 0) and high >= count, "旧账本计数无效")
    ids = set()
    for index in range(8):
        row = blob[10 + 112 * index:122 + 112 * index]
        if index >= count:
            require(not any(row), "旧账本空行非零")
            continue
        operation = row[4:40].decode("ascii", errors="replace")
        require(row[40] == 0 and legacy.valid_uuid_text(operation) and operation not in ids and
                u32(row) == high - count + 1 + index and 0 < u32(row, 108) <= ecs_sequence and
                any(row[41:73]) and any(row[76:108]) and row[73] in {1, 2, 3} and
                row[74] in {2, 3} and ((row[74] == 3) == (row[75] != 0)),
                "旧账本有PREPARED、损坏或重复记录")
        ids.add(operation)
    return high


def validate_ecs2(blob, flash, layout, valid_hashes):
    require(len(blob) == 288 and blob[:4] == b"ECS2" and u32(blob, 4) == 2 and
            u32(blob, 8) > 0 and u32(blob, 284) == zlib.crc32(blob[:284]), "旧ECS2格式或CRC无效")
    bindings = []
    slot_size = layout["packages"][1] // 3
    for index in range(2):
        row = blob[12 + index * 80:92 + index * 80]
        require(row[0] in {0, 1} and row[1] in {0, 1} and row[3] == 0, "旧ECS2绑定标志无效")
        if row[0] == 0:
            require(not any(row), "旧ECS2空绑定非零")
            continue
        firmware = row[4:36]
        require(firmware in valid_hashes and all(item["firmware"] != firmware for item in bindings),
                "旧ECS2引用非VALID或重复固件")
        package, size, abi, schema = row[36:68], u32(row, 68), u32(row, 72), u32(row, 76)
        if row[1]:
            require(row[2] < 3 and 0 < size <= slot_size and abi > 0 and schema > 0 and any(package),
                    "旧ECS2包几何无效")
            at = layout["packages"][0] + row[2] * slot_size
            require(hashlib.sha256(flash[at:at + size]).digest() == package, "旧ECS2包原字节摘要不符")
        else:
            require(not any(row[2:4] + row[36:]), "旧ECS2无包字段非零")
        bindings.append({"firmware": firmware, "package": package, "size": size,
                         "abi": abi, "schema": schema, "slot": row[2]})
    require({b["firmware"] for b in bindings} == valid_hashes, "旧ECS2没有覆盖精确VALID固件集合")
    if len(bindings) == 2 and bindings[0]["size"] and bindings[1]["size"] and bindings[0]["slot"] == bindings[1]["slot"]:
        require(all(bindings[0][k] == bindings[1][k] for k in ("package", "size", "abi", "schema")), "旧ECS2同包槽绑定不一致")
    operation = blob[172:284]
    phase = operation[0]
    require(phase in {0, 5}, "旧ECS2仍在写入/试运行/中断，须先完成旧链")
    if phase == 0:
        require(not any(operation), "旧ECS2 IDLE含未决操作")
    else:
        target = next((b for b in bindings if b["firmware"] == operation[20:52]), None)
        require(target is not None and any(operation[4:20]) and operation[2] in {0, 1, 2} and
                operation[3] in {0, 1} and (any(operation[96:112]) or
                    operation[2] == 2 and operation[3] == 0), "旧ECS2确认操作无效")
        require(operation[1] == target["slot"] and operation[52:84] == target["package"] and
                (u32(operation, 84), u32(operation, 88), u32(operation, 92)) ==
                (target["size"], target["abi"], target["schema"]) and
                ((operation[2] == 2) == (target["size"] == 0)), "旧ECS2确认绑定不符")
    return {"sequence": u32(blob, 8), "phase": phase, "bindings": bindings}


def validate_v3_receipt(blob, device_id, flash, layout, selected, states, images, ecs):
    require(len(blob) == 308 and blob[:5] == b"EOTA\x03" and blob[5] in {2, 3} and
            (blob[6], blob[7]) in {(0x10, 0x11), (0x11, 0x10)} and blob[9] in {0, 1} and
            blob[186] in {0, 1, 2} and blob[263] in {0, 1}, "旧V3收据未决、损坏或版本未知")
    require(blob[14:50].decode("ascii", errors="replace") == device_id and
            legacy.valid_uuid_text(blob[50:86].decode("ascii", errors="replace")) and
            288 <= u32(blob, 10) <= layout["app_size"] and any(blob[86:118]) and any(blob[118:150]) and
            (not any(blob[150:182]) or blob[150:182] != blob[118:150]) and
            (blob[8] == 0 if blob[5] == 3 else blob[8] in {1, 2, 3, 4, 5, 6, 7, 8, 10}),
            "旧V3原ID、身份、状态或固件字段无效")
    require(bool(blob[9]) == (ecs is not None) and
            (u32(blob, 182) > 0 if ecs else u32(blob, 182) == 0), "旧V3与ECS2启用/序号不符")
    target_size, target_abi, target_schema = (u32(blob, n) for n in (187, 191, 195))
    target_empty = not any(blob[187:263])
    source_size, source_abi, source_schema = (u32(blob, n) for n in (264, 268, 272))
    source_empty = not any(blob[264:308])
    require((not blob[263] and source_empty) or (blob[263] and 0 < source_size < 0x80000000 and source_abi > 0 and
            source_schema > 0 and any(blob[276:308])), "旧V3来源包字段无效")
    if blob[186] == 0:
        require(target_empty and not blob[263], "旧V3无包字段非零")
    else:
        require(ecs is not None and 0 < target_size < 0x80000000 and target_abi > 0 and target_schema > 0 and
                any(blob[199:231]) and any(blob[231:263]), "旧V3目标包字段无效")
        if blob[186] == 1:
            require(blob[263] and (target_size, target_abi, target_schema, blob[199:231]) ==
                    (source_size, source_abi, source_schema, blob[276:308]), "旧V3 REUSE不符")
        elif blob[263]:
            require(target_schema == source_schema, "旧V3 WRITE schema不符")
    source, target = blob[6] - 0x10, blob[7] - 0x10
    if blob[5] == 3:
        require(selected == target and target in images and digest(images[target]) == blob[86:118].hex() and
                len(images[target]) == u32(blob, 10) and source in images and
                digest(images[source]) == blob[118:150].hex(), "旧V3成功与双槽签名身份不符")
    else:
        require(selected == source and source in images and digest(images[source]) == blob[118:150].hex(),
                "旧V3失败与已确认来源身份不符")
        if target in images:
            require(hashlib.sha256(images[target]).digest() == blob[150:182], "旧V3失败备用身份不符")
        else:
            offset = layout["apps"][target]
            require(states.get(target) in {None, 3, 4} and flash[offset:offset + 0x1000] == b"\xff" * 0x1000,
                    "旧V3失败候选未完成物理清理")
    if ecs:
        require(ecs["sequence"] >= u32(blob, 182), "旧ECS2落后于原V3")
        firmware = blob[86:118] if blob[5] == 3 else blob[118:150]
        binding = next((b for b in ecs["bindings"] if b["firmware"] == firmware), None)
        require(binding is not None, "旧V3终态缺少ECS2固件绑定")
        expected = (target_size, target_abi, target_schema, blob[199:231]) if blob[5] == 3 else \
                   (source_size, source_abi, source_schema, blob[276:308])
        require((binding["size"], binding["abi"], binding["schema"], binding["package"]) == expected,
                "旧V3终态包与ECS2不符")
    return {"operation_id": blob[50:86].decode(), "state": "succeeded" if blob[5] == 3 else "failed"}


def inspect_source(flash, source_layout, device_id, source_key, components, work, efuse_mac=None):
    layout = LAYOUTS[source_layout]
    check_old_layout(flash, layout, components)
    parser = legacy.load_nvs_parser(components)
    if source_layout == "esp32_at":
        require(device_id is None and efuse_mac is not None, "旧AT无Base UUID；必须绑定本轮独立eFuse MAC")
        archive = at_archive.make_archive(flash)
        partition = parser.NVS_Partition("old_at_nvs", bytearray(flash[0x12000:0x20000]))
        for page in partition.pages:
            legacy.validate_page(page, "old_at_nvs")
        entries = [entry for page in partition.pages for entry in page.entries if entry.state == "Written"]
        namespaces = {entry.data["value"]: entry.key for entry in entries if entry.metadata["namespace"] == 0}
        phy = [entry for entry in entries if namespaces.get(entry.metadata["namespace"]) == "phy" and entry.key == "cal_mac"]
        require(len(phy) == 1 and phy[0].metadata["type"] == "blob" and phy[0].data["size"] == 6,
                "旧AT缺少可绑定物理设备的phy/cal_mac")
        mac = b"".join(bytes(child.raw) for child in phy[0].children)[:6]
        require(mac.hex() == efuse_mac.replace(":", "").lower(), "旧AT备份与本轮eFuse MAC不符")
        for entry in entries:
            if namespaces.get(entry.metadata["namespace"]) == "nvs.net80211" and entry.key in {"sta.ssid", "sta.pswd", "ap.ssid", "ap.passwd"}:
                require(all(byte == 0xff for child in entry.children for byte in child.raw),
                        "旧AT含非空Wi-Fi字段，不能声明空配置迁入")
        return {}, {"source_layout": source_layout, "source_flash_sha256": digest(flash), "old_at_sha256": digest(archive), "source_efuse_mac": mac.hex()}, archive
    require(device_id is not None and legacy.valid_uuid_text(device_id), "Base迁入必须绑定实际UUID")
    identity = legacy.nvs_records(flash, "nvs", 0x9000, 0x6000, parser,
        {("base_identity", "device_uuid"), *legacy.SDK_NVS_TYPES}, frozenset({"misc"}))
    require(identity.get(("base_identity", "device_uuid")) == ("string", device_id.encode() + b"\0"), "备份UUID不符")
    require(all(identity[k][0] == kind for k, kind in legacy.SDK_NVS_TYPES.items() if k in identity), "SDK身份记录类型不符")
    phy_keys = {key for key in identity if key[0] == "phy"}
    require(not phy_keys or phy_keys == {key for key in legacy.SDK_NVS_TYPES if key[0] == "phy"}, "PHY记录不完整")
    if phy_keys:
        require(len(identity[("phy", "cal_mac")][1]) == 6 and bool(identity[("phy", "cal_data")][1]), "PHY记录长度无效")
    store_offset, store_size = layout["store"]
    allowed = {CONFIG_KEY, *RETIRED_KEYS}
    store = legacy.nvs_records(flash, "base_store", store_offset, store_size, parser, allowed)
    require(all(kind == "blob" for kind, _ in store.values()), "Base活动记录必须是blob")
    config = store.get(CONFIG_KEY)
    revision = None
    if config:
        blob = config[1]
        if source_layout == "c3_v1" and blob[:5] == b"EBCF\x01":
            revision = legacy.validate_v1_config(blob)
            store[CONFIG_KEY] = ("blob", legacy.convert_v1_wifi_only(blob))
        elif source_layout == "c3_v1" and blob[:5] == b"EBCF\x02":
            revision = legacy.validate_v2_config(blob)
            store[CONFIG_KEY] = ("blob", legacy.convert_v2_preserving_mqtt(blob))
        else:
            revision = validate_v3_config(blob)
    selected, states = ota_selection(flash, layout)
    images = {}
    for slot in range(2):
        if states.get(slot) == 2:
            offset = layout["apps"][slot]
            unsigned_source = source_layout == "c3_v1" and source_key is None
            slot_bytes = flash[offset:offset + layout["app_size"]]
            image = signed_image(slot_bytes, layout["target"], signed=not unsigned_source)
            if unsigned_source:
                require(slot_bytes[len(image):] == b"\xff" * (len(slot_bytes) - len(image)),
                        "旧未签名槽尾含非擦除数据，不能绕过其实际签名身份")
            if not unsigned_source:
                verify_signature(image, source_key, layout["target"], work, "old_ota_" + str(slot))
            images[slot] = image
        else:
            require(flash[layout["apps"][slot]:layout["apps"][slot] + 0x1000] == b"\xff" * 0x1000,
                    "非VALID旧槽仍含可启动镜像，停止迁入")
    ecs = None
    if ("base_pkg", "slots") in store:
        require("packages" in layout, "旧无包布局含ECS2")
        ecs = validate_ecs2(store[("base_pkg", "slots")][1], flash, layout,
                            {hashlib.sha256(image).digest() for image in images.values()})
    if ("base_product", "operations") in store:
        require(ecs is not None, "旧产品账本缺ECS2")
        validate_ledger(store[("base_product", "operations")][1], ecs["sequence"])
    terminal = None
    if ("base_ota", "operation") in store:
        terminal = validate_v3_receipt(store[("base_ota", "operation")][1], device_id,
                                       flash, layout, selected, states, images, ecs)
    report = {"source_layout": source_layout, "source_flash_sha256": digest(flash), "device_id": device_id,
              "config_revision": revision, "selected_slot": "ota_" + str(selected), "old_operation": terminal, "source_verification_key_sha256": digest(read_file(source_key, maximum=4096)) if source_key else None,
              "old_slots": {"ota_" + str(slot): {"size_bytes": len(image), "sha256": digest(image)}
                            for slot, image in images.items()},
              "retired_records": {"/".join(key): base64.b64encode(value).decode()
                                  for key, (_, value) in store.items() if key in RETIRED_KEYS}}
    preserved = {key: value for key, value in store.items() if key not in RETIRED_KEYS}
    archive = flash[0x3e6000:0x3ea000] if layout["target"] == "esp32" else None
    return preserved, report, archive


def generate_store(records, size, components, parser, work):
    if not records:
        return b"\xff" * size
    input_csv, output = work / "store.csv", work / "store.bin"
    with input_csv.open("w", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(("key", "type", "encoding", "value"))
        namespaces = set()
        for (namespace, key), (kind, raw) in sorted(records.items()):
            require(kind == "blob", "候选store只支持已校验的blob")
            if namespace not in namespaces:
                writer.writerow((namespace, "namespace", "", ""))
                namespaces.add(namespace)
            writer.writerow((key, "data", "base64", base64.b64encode(raw).decode()))
    result = subprocess.run([sys.executable, str(components / "nvs_flash/nvs_partition_generator/nvs_partition_gen.py"),
                             "generate", str(input_csv), str(output), hex(size)], capture_output=True, check=False)
    require(result.returncode == 0, "官方NVS候选生成失败")
    raw = output.read_bytes()
    actual = legacy.nvs_records(raw, "base_store", 0, size, parser, set(records))
    require(actual == records, "候选NVS精确键/类型/字节读回不符")
    return raw


def build_candidate(flash, target, app, bootloader, table, store, old_at, components):
    expected = table_from_csv(ROOT / "firmware/partitions" /
        ("c3-partition-table.csv" if target == "esp32c3" else "esp32-partition-table.csv"), components)
    require(parse_table(table, components) == parse_table(expected, components), "新表不是当前原生目标布局")
    require(len(table) <= 0x1000, "新分区表含越界内容")
    # Full recovery remains the original image; overwritten regions are explicitly archived first.
    candidate = bytearray(flash)
    boot_offset = 0 if target == "esp32c3" else 0x1000
    require(24 <= len(bootloader) <= TABLE_OFFSET - boot_offset and bootloader[0] == 0xe9 and
            int.from_bytes(bootloader[12:14], "little") == (5 if target == "esp32c3" else 0), "新bootloader几何/芯片不符")
    candidate[boot_offset:TABLE_OFFSET] = b"\xff" * (TABLE_OFFSET - boot_offset)
    candidate[boot_offset:boot_offset + len(bootloader)] = bootloader
    candidate[TABLE_OFFSET:0x9000] = table + b"\xff" * (0x1000 - len(table))
    for offset in APP_OFFSETS:
        candidate[offset:offset + APP_SIZE] = app + b"\xff" * (APP_SIZE - len(app))
    ota_offset = 0xf000 if target == "esp32c3" else 0x10000
    ota = bytearray(b"\xff" * 0x2000)
    for index in range(2):
        at = index * 0x1000
        sequence = (index + 1).to_bytes(4, "little")
        ota[at:at + 4] = sequence
        ota[at + 24:at + 28] = (2).to_bytes(4, "little")
        ota[at + 28:at + 32] = zlib.crc32(sequence, 0xffffffff).to_bytes(4, "little")
    candidate[ota_offset:ota_offset + 0x2000] = ota
    store_offset = 0x3f5000 if target == "esp32c3" else 0x3fa000
    candidate[store_offset:FLASH_SIZE] = store
    if target == "esp32":
        require(old_at is not None and len(old_at) == 0x4000, "旧AT归档不完整")
        candidate[0x3e6000:0x3ea000] = old_at
    return bytes(candidate)


def private_output_directory(path):
    parent = path.parent.resolve(strict=True)
    info = parent.stat()
    require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) & 0o077 == 0,
            "输出父目录必须为当前用户独占0700目录")
    require(not parent.is_relative_to(ROOT) and not any((p / ".git").exists() for p in (parent, *parent.parents)),
            "迁入材料只能保存在Git仓库外")
    require(not path.exists() and not path.is_symlink(), "输出目录已存在，禁止覆盖")
    os.mkdir(path, 0o700)


def write_verified(path, raw):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as target:
            target.write(raw)
            target.flush()
            os.fsync(descriptor)
    finally:
        os.close(descriptor)
    require(read_file(path, private=True) == raw, "输出落盘读回不符")


def prepare(args):
    components = legacy.check_sdk(args.idf_path)
    flash = legacy.compare_backups(args.backup_a, args.backup_b)
    layout = LAYOUTS[args.source_layout]
    target = layout["target"]
    with tempfile.TemporaryDirectory(prefix="native_layout_prepare_") as scratch:
        work = Path(scratch)
        source_key = None
        if args.source_verification_key is not None:
            source_key = work / "source_verification_key.bin"
            write_verified(source_key, read_file(args.source_verification_key, maximum=4096))
        verification_key = work / "verification_key.bin"
        write_verified(verification_key, read_file(args.verification_key, maximum=4096))
        records, report, old_at = inspect_source(flash, args.source_layout, args.device_id,
            source_key, components, work, args.source_efuse_mac)
        app = read_file(args.app, maximum=APP_SIZE)
        require(signed_image(app, target) == app, "新app不是精确完整签名镜像")
        verify_signature(app, verification_key, target, work, "new_app")
        bootloader = read_file(args.bootloader, maximum=TABLE_OFFSET)
        require(signed_image(bootloader, target, app=False, signed=False) == bootloader, "新bootloader镜像完整性或长度不符")
        table = read_file(args.partition_table, maximum=0x1000)
        if target == "esp32":
            verify_signature(table, verification_key, target, work, "new_table")
            key_bytes = read_file(verification_key, maximum=512)
            require(len(key_bytes) == 64 and key_bytes in app and key_bytes in bootloader,
                    "ESP32公钥未同时嵌入新app与bootloader")
        store_size = 0xb000 if target == "esp32c3" else 0x6000
        store = generate_store(records, store_size, components, legacy.load_nvs_parser(components), work)
        source = bytearray(flash)
        if args.source_layout == "esp32_at":
            # Old AT has no Base identity/config; keep its complete original bytes in the archive.
            source[0x9000:0xf000] = b"\xff" * 0x6000
            source[0xf000:0x10000] = b"\xff" * 0x1000
            source[0x12000:0x20000] = b"\xff" * 0xe000
            source[0x3ea000:0x3fa000] = b"\xff" * 0x10000
        elif args.source_layout == "c3_v1":
            # The legacy store occupies the new scratch/tail. Its complete bytes are archived.
            source[0x3e5000:0x3f5000] = b"\xff" * 0x10000
        candidate = build_candidate(bytes(source), target, app, bootloader, table, store, old_at, components)
        report.update(schema_version=1, target=target + "/esp_base", idf_commit=legacy.IDF_COMMIT,
                      candidate_flash_sha256=digest(candidate), candidate_app_sha256=digest(app),
                      candidate_app_size_bytes=len(app), verification_key_sha256=digest(read_file(verification_key, maximum=4096)),
                      bootloader_sha256=digest(bootloader), partition_table_sha256=digest(table),
                      candidate_selected_slot="ota_1", candidate_state="software_only",
                      hardware_write_authorized=False, device_verified=False,
                      old_operation_runtime_result="unknown_after_wired_identity_change",
                      preserved_records={"/".join(k): digest(v) for k, (_, v) in records.items()})
        private_output_directory(args.output_directory)
        # Archive+readback precedes creation of the candidate that retires old runtime keys.
        write_verified(args.output_directory / "source_flash.bin", flash)
        encoded = (json.dumps(report, sort_keys=True, ensure_ascii=False, indent=2) + "\n").encode()
        write_verified(args.output_directory / "migration_receipt.json", encoded)
        if old_at is not None:
            write_verified(args.output_directory / "at_old_raw.bin", old_at)
        write_verified(args.output_directory / "candidate_flash.bin", candidate)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-layout", required=True, choices=sorted(LAYOUTS))
    parser.add_argument("--backup-a", type=Path, required=True)
    parser.add_argument("--backup-b", type=Path, required=True)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--device-id")
    parser.add_argument("--source-efuse-mac")
    parser.add_argument("--source-verification-key", type=Path)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path, required=True)
    parser.add_argument("--partition-table", type=Path, required=True)
    parser.add_argument("--verification-key", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args)
    except (MigrationError, legacy.PreflightError, at_archive.ArchiveError, OSError) as error:
        print("原生布局候选阻断\n  原因  " + str(error), file=sys.stderr)
        return 1
    print("原生布局候选已生成\n  证据  原Flash、旧终态与候选均已仓外私存并精确读回\n"
          "  范围  仅软件候选；未执行设备写入、eFuse或实体迁入验收")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
