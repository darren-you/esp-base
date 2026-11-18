#!/usr/bin/env python3
"""ESP Base 业务应用的打包：检查 WASM 模块、生成清单、封装成应用包。由 esp_base.py 的 app 子命令调用。

应用包的布局见 firmware/components/base_wasm/core/wasm_package.h。这里产出未签名的包（包头、清单、wasm），
签名由 esp_base.py app sign 用官方 espsecure 完成：补齐到 4 KiB 后附一个扇区的 Secure Boot V2 签名块。
"""
import json
import re
import struct
import tomllib
from pathlib import Path

ABI = 1  # 这个工具认识的宿主接口版本
MAGIC = b"EBAP"
FORMAT = 1
HEADER_BYTES = 32
SECTOR_BYTES = 4096
SLOT_BYTES = 32768
MANIFEST_MAX = 1024
LINEAR_MAX = 16384  # 线性内存（栈加数据）的上限；超过说明模块没有按 SDK 的链接参数构建
MEMORY_RANGE = (4096, 65536)
STACK_RANGE = (1024, 8192)
NAME = re.compile(r"[a-z][a-z0-9_]{0,22}\Z")
VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\Z")
CHIP = re.compile(r"[a-z0-9]{1,15}\Z")
PIN = re.compile(r"(0|[1-9]\d?)\Z")
PIN_MODES = ("in", "in_pullup", "in_pulldown", "out0", "out1", "pwm")
PINS_MAX = 8
# 宿主接口：导入模块 base 里的函数，以及它们从哪个接口版本开始提供。
IMPORTS = {name: 1 for name in (
    "log", "uptime_ms", "unix_ms", "timer_set", "timer_cancel", "sleep_ms", "mqtt_connect", "mqtt_disconnect",
    "mqtt_subscribe", "mqtt_publish", "event_topic", "event_payload", "param", "info", "gpio_write", "gpio_read",
    "pwm_config", "pwm_write", "pwm_read")}


class AppError(Exception):
    pass


# ---- WASM 模块检查 ----
def _leb_u(data, at):
    result = shift = 0
    while True:
        byte = data[at]
        at += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, at


def _leb_s(data, at):
    result = shift = 0
    while True:
        byte = data[at]
        at += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return (result - (1 << shift) if byte & 0x40 else result), at


def _name(data, at):
    length, at = _leb_u(data, at)
    return data[at:at + length].decode(), at + length


def _limits(data, at):
    flag = data[at]
    low, at = _leb_u(data, at + 1)
    high = None
    if flag & 1:
        high, at = _leb_u(data, at)
    return (low, high), at


def _const(data, at):
    opcode = data[at]
    if opcode not in (0x41, 0x42):
        raise AppError("全局变量的初值不是整数常量")
    value, at = _leb_s(data, at + 1)
    if data[at] != 0x0B:
        raise AppError("全局变量的初值表达式不合法")
    return value, at + 1


def _scan(body):
    """扫一个函数体：返回它是否含 memory.grow，遇到 WebAssembly 1.0 之外的指令时抛错。"""
    at, grow = 0, False
    while at < len(body):
        opcode = body[at]
        at += 1
        if opcode in (0x02, 0x03, 0x04):
            if body[at] not in (0x40, 0x7F, 0x7E, 0x7D, 0x7C):
                raise AppError("模块用了多值块（WebAssembly 1.0 之外的特性）")
            at += 1
        elif opcode in (0x0C, 0x0D, 0x10, 0x20, 0x21, 0x22, 0x23, 0x24):
            _, at = _leb_u(body, at)
        elif opcode == 0x0E:
            count, at = _leb_u(body, at)
            for _ in range(count + 1):
                _, at = _leb_u(body, at)
        elif opcode == 0x11:
            _, at = _leb_u(body, at)
            _, at = _leb_u(body, at)
        elif 0x28 <= opcode <= 0x3E:
            _, at = _leb_u(body, at)
            _, at = _leb_u(body, at)
        elif opcode == 0x3F:
            at += 1
        elif opcode == 0x40:
            at += 1
            grow = True
        elif opcode in (0x41, 0x42):
            _, at = _leb_s(body, at)
        elif opcode == 0x43:
            at += 4
        elif opcode == 0x44:
            at += 8
        elif opcode in (0x00, 0x01, 0x05, 0x0B, 0x0F, 0x1A, 0x1B) or 0x45 <= opcode <= 0xBF:
            pass
        else:
            raise AppError(f"模块用了 WebAssembly 1.0 之外的指令（0x{opcode:02x}）；应用要编译到 wasm32v1-none")
    return grow


def inspect(wasm):
    """核对模块满足宿主的要求，返回 {imports, linear_bytes, abi}。不满足时抛 AppError。"""
    if wasm[:8] != b"\0asm\x01\0\0\0":
        raise AppError("不是 WebAssembly 1.0 模块")
    imports, exports, globals_, memory, grow, at = [], {}, [], None, False, 8
    while at < len(wasm):
        section = wasm[at]
        size, at = _leb_u(wasm, at + 1)
        p, end = at, at + size
        if section == 2:
            count, p = _leb_u(wasm, p)
            for _ in range(count):
                module, p = _name(wasm, p)
                field, p = _name(wasm, p)
                if wasm[p] != 0:
                    raise AppError(f"模块导入了函数之外的东西：{module}.{field}")
                _, p = _leb_u(wasm, p + 1)
                imports.append((module, field))
        elif section == 5:
            count, p = _leb_u(wasm, p)
            if count != 1:
                raise AppError("模块必须有且只有一块线性内存")
            memory, p = _limits(wasm, p)
        elif section == 6:
            count, p = _leb_u(wasm, p)
            for _ in range(count):
                value, p = _const(wasm, p + 2)
                globals_.append(value)
        elif section == 7:
            count, p = _leb_u(wasm, p)
            for _ in range(count):
                field, p = _name(wasm, p)
                kind = wasm[p]
                index, p = _leb_u(wasm, p + 1)
                exports[field] = (kind, index)
        elif section == 8:
            raise AppError("模块带 start 段：实例化时执行的代码不受指令预算约束，不允许")
        elif section == 10:
            count, p = _leb_u(wasm, p)
            for _ in range(count):
                length, p = _leb_u(wasm, p)
                locals_count, q = _leb_u(wasm, p)
                for _ in range(locals_count):
                    _, q = _leb_u(wasm, q)
                    q += 1
                grow = _scan(wasm[q:p + length]) or grow
                p += length
        at = end
    for field in ("app_init", "app_event"):
        if exports.get(field, (None,))[0] != 0:
            raise AppError(f"模块没有导出函数 {field}（用 esp_base_app::app! 导出入口）")
    unknown = [f"{module}.{field}" for module, field in imports if module != "base" or field not in IMPORTS]
    if unknown:
        raise AppError("模块导入了这个版本的宿主接口没有的函数：" + "、".join(unknown))
    if grow:
        raise AppError("模块含 memory.grow：线性内存无法收缩。应用不能用会增长内存的分配器")
    heap_base = exports.get("__heap_base")
    if not heap_base or heap_base[0] != 3 or exports.get("__data_end", (None,))[0] != 3:
        raise AppError("模块没有导出 __heap_base 和 __data_end：用 SDK 的 .cargo/config.toml 链接参数构建")
    if memory is None:
        raise AppError("模块没有线性内存")
    # 导出的全局变量下标排在导入的全局变量之后；应用没有全局变量导入。
    linear = (globals_[heap_base[1]] + 7) & ~7
    if not 0 < linear <= LINEAR_MAX:
        raise AppError(f"线性内存 {linear} 字节，超过上限 {LINEAR_MAX}：减小栈或静态数据")
    return {"imports": sorted(field for _, field in imports), "linear_bytes": linear,
            "abi": max([IMPORTS[field] for _, field in imports], default=1)}


# ---- 清单与封装 ----
def manifest(project, build_number, required_abi):
    """从应用工程的 Cargo.toml 生成清单：名称和版本取自 [package]，其余取自 [package.metadata.esp-base]。"""
    cargo = tomllib.loads((Path(project) / "Cargo.toml").read_text())
    package = cargo.get("package") or {}
    settings = (package.get("metadata") or {}).get("esp-base") or {}
    unknown = set(settings) - {"memory", "stack", "chips", "pins"}
    if unknown:
        raise AppError("[package.metadata.esp-base] 里有不认识的键：" + "、".join(sorted(unknown)))
    name = str(package.get("name", "")).replace("-", "_")
    version = str(package.get("version", ""))
    if not NAME.match(name):
        raise AppError("应用名（Cargo 包名，连字符换成下划线）必须是 1–23 个小写字母、数字或下划线，字母开头")
    if not VERSION.match(version):
        raise AppError("Cargo 包的 version 必须是 X.Y.Z")
    result = {"name": name, "version": f"{version}+{int(build_number)}", "abi": int(required_abi)}
    for key, (low, high) in (("memory", MEMORY_RANGE), ("stack", STACK_RANGE)):
        value = settings.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or not low <= value <= high:
            raise AppError(f"[package.metadata.esp-base] 的 {key} 必须是 {low}–{high} 之间的整数（字节）")
        result[key] = value
    chips = settings.get("chips")
    if chips is not None:
        if not isinstance(chips, list) or not 1 <= len(chips) <= 4 or not all(isinstance(c, str) and CHIP.match(c) for c in chips):
            raise AppError("[package.metadata.esp-base] 的 chips 必须是 1–4 个芯片型号，例如 [\"esp32c3\"]")
        result["chips"] = chips
    pins = settings.get("pins")
    if pins is not None:
        # 哪些引脚在哪种芯片上能这样用由设备判定（安装时不符合的报 pin_denied）；这里只核对写法。
        if (not isinstance(pins, dict) or not 1 <= len(pins) <= PINS_MAX or
                not all(PIN.match(pin) and mode in PIN_MODES for pin, mode in pins.items())):
            raise AppError(f"[package.metadata.esp-base.pins] 必须是 1–{PINS_MAX} 项「引脚号 = 用途」，用途是 "
                           + "、".join(PIN_MODES) + "，例如 4 = \"out0\"")
        result["pins"] = dict(pins)
    return result


def pack(wasm, fields):
    """未签名的应用包：包头、清单、wasm。签名时再补齐到 4 KiB 并附签名扇区。"""
    text = json.dumps(fields, ensure_ascii=False, separators=(",", ":")).encode()
    if len(text) > MANIFEST_MAX:
        raise AppError("清单超过 1024 字节")
    content = HEADER_BYTES + len(text) + len(wasm)
    if (content + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES + SECTOR_BYTES > SLOT_BYTES:
        raise AppError(f"应用包 {content} 字节，补齐并加签名扇区后超过 {SLOT_BYTES} 字节的应用槽")
    header = MAGIC + struct.pack("<HHII", FORMAT, HEADER_BYTES, len(text), len(wasm)) + bytes(16)
    return header + text + wasm


def describe(package):
    """读回一个应用包（签名与否均可）的包头和清单。"""
    if len(package) < HEADER_BYTES or package[:4] != MAGIC:
        raise AppError("不是应用包")
    version, header_bytes, manifest_bytes, wasm_bytes = struct.unpack("<HHII", package[4:16])
    if version != FORMAT or header_bytes != HEADER_BYTES or any(package[16:HEADER_BYTES]):
        raise AppError("应用包的包头不认识")
    content = HEADER_BYTES + manifest_bytes + wasm_bytes
    if len(package) < content:
        raise AppError("应用包不完整")
    fields = json.loads(package[HEADER_BYTES:HEADER_BYTES + manifest_bytes])
    padded = (content + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
    return {"manifest": fields, "wasm_bytes": wasm_bytes, "content_bytes": content,
            "signed": len(package) == padded + SECTOR_BYTES and package[padded] == 0xE7,
            "wasm": package[HEADER_BYTES + manifest_bytes:content]}
