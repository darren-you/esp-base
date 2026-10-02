#!/usr/bin/env python3
"""最小串口协议调用示例；宿主 Python，不在 ESP 固件中运行。"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import select
import stat
import sys
import termios
import time
import uuid


class SerialPort:
    """POSIX 串口：不切换 DTR/RTS，关闭时不挂断，写入期限为一秒。"""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            options = termios.tcgetattr(self.fd)
            options[0] = options[1] = options[3] = 0
            options[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB |
                            termios.HUPCL | getattr(termios, "CRTSCTS", 0))
            options[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
            options[4] = options[5] = termios.B115200
            options[6][termios.VMIN] = options[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, options)
        except Exception:
            self.close()
            raise

    def read(self, size=1):
        if not select.select([self.fd], [], [], 0.1)[0]:
            return b""
        try:
            return os.read(self.fd, size)
        except BlockingIOError:
            return b""

    def write(self, payload):
        view = memoryview(payload)
        written = 0
        deadline = time.monotonic() + 1
        while written < len(view):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TimeoutError("串口写入超时；状态为 unknown，不自动重发命令")
            try:
                count = os.write(self.fd, view[written:])
            except BlockingIOError:
                continue
            if count <= 0:
                raise OSError("串口写入中断；状态为 unknown")
            written += count
        return written

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("重复 JSON 字段")
        value[key] = item
    return value


def canonical_id(value):
    if not isinstance(value, str):
        raise ValueError("身份不是字符串")
    parsed = uuid.UUID(value)
    if str(parsed) != value or parsed.version != 4:
        raise ValueError("身份不是规范 UUID v4")
    return value


def read_result(port, request_id, deadline):
    buffer = bytearray()
    discard = False
    while time.monotonic() < deadline:
        byte = port.read(1)
        if not byte:
            continue
        if byte != b"\n":
            if len(buffer) >= 8192 or byte == b"\0":
                buffer.clear()
                discard = True
            elif not discard:
                buffer.extend(byte)
            continue
        line = bytes(buffer)
        buffer.clear()
        if discard:
            discard = False
            continue
        if not line.startswith(b"{"):
            continue
        try:
            value = json.loads(line.decode("utf-8"), object_pairs_hook=unique_object)
            if set(value) != {"protocol_version", "device_id", "boot_id", "request_id", "state", "error_code", "result"}:
                continue
            if type(value["protocol_version"]) is not int or value["protocol_version"] != 1 or value["request_id"] != request_id:
                continue
            canonical_id(value["device_id"])
            canonical_id(value["boot_id"])
            if value["state"] not in {"accepted", "running", "succeeded", "failed", "expired", "unknown"}:
                continue
            failed = value["state"] in {"failed", "expired", "unknown"}
            if failed:
                if not isinstance(value["error_code"], str) or not value["error_code"]:
                    continue
            elif value["error_code"] is not None:
                continue
            if value["result"] is not None and not isinstance(value["result"], dict):
                continue
            yield value
        except (ValueError, TypeError, KeyError):
            continue
    raise TimeoutError("没有收到设备最终结果；状态为 unknown，不自动重发写命令")


def wait_ready(port):
    # 只依据真实启动报告判断就绪，不以打开串口或等待时长代替。
    buffer = bytearray()
    discard = False
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        byte = port.read(1)
        if not byte:
            continue
        if byte != b"\n":
            if len(buffer) >= 8192:
                discard = True
                buffer.clear()
            elif not discard:
                buffer.extend(byte)
            continue
        line = bytes(buffer)
        buffer.clear()
        if discard:
            discard = False
            continue
        marker = b"ESP_BASE_REPORTED "
        if marker not in line:
            continue
        try:
            fields = dict(item.split("=", 1) for item in line.split(marker, 1)[1].decode().strip().split() if "=" in item)
            canonical_id(fields["device_id"])
            canonical_id(fields["boot_id"])
            if int(fields["uptime_ms"]) >= 0:
                return
        except (KeyError, ValueError):
            continue
    raise TimeoutError("未取得当前协议的设备启动状态")


def send(port, request):
    line = json.dumps(request, separators=(",", ":"), ensure_ascii=True).encode()
    if len(line) > 9216:
        raise ValueError("串口请求超过 9216 字节，未发送")
    payload = b"\n" + line + b"\n"
    if port.write(payload) != len(payload):
        raise OSError("串口写入不完整；状态为 unknown")


def status(port):
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id, "command": "status"})
    for value in read_result(port, request_id, time.monotonic() + 5):
        if value["state"] != "succeeded" or not isinstance(value["result"], dict):
            raise ValueError("设备拒绝状态请求")
        uptime = value["result"].get("uptime_ms")
        if type(uptime) is not int or not 0 <= uptime <= 9007199254740991:
            raise ValueError("设备 uptime 无效")
        return value


def product_result(port, current, operation_id, deadline=None):
    canonical_id(operation_id)
    if deadline is None:
        deadline = time.monotonic() + 5
    if time.monotonic() >= deadline:
        raise TimeoutError("产品结果查询期限已到；状态为 unknown，不自动重发写命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "product.result", "parameters": {"operation_id": operation_id}})
    for value in read_result(port, request_id, deadline):
        if value["device_id"] != current["device_id"] or value["boot_id"] != current["boot_id"]:
            raise ValueError("产品结果来自另一设备或启动；状态为 unknown")
        result = value["result"]
        if result is not None:
            if result.get("operation_id") != operation_id:
                raise ValueError("产品结果 operation_id 不匹配；状态为 unknown")
            sequence = result.get("operation_sequence")
            container_sequence = result.get("container_sequence")
            kind = result.get("kind")
            if (type(container_sequence) is not int or
                    not 1 <= container_sequence <= 4294967295 or not isinstance(kind, str)):
                raise ValueError("设备产品结果字段无效；状态为 unknown")
            if kind in {"stop", "start"}:
                if (set(result) != {"operation_id", "operation_sequence", "kind", "container_sequence"} or
                        sequence is not None or
                        (value["state"] in {"running", "succeeded"} and value["error_code"] is not None) or
                        (value["state"] == "unknown" and value["error_code"] not in {
                            "product_state_uncertain", "storage_uncertain"}) or
                        value["state"] not in {"running", "succeeded", "failed", "unknown"}):
                    raise ValueError("设备本次启动产品结果字段无效；状态为 unknown")
                return value
            code = result.get("result_code")
            digest = result.get("package_sha256")
            if (set(result) != {"operation_id", "operation_sequence", "kind", "package_sha256",
                               "container_sequence", "result_code"} or
                    type(sequence) is not int or not 1 <= sequence <= 4294967295 or
                    type(code) is not int or not 0 <= code <= 255 or
                    kind not in {"install", "upgrade", "uninstall"} or
                    not isinstance(digest, str) or len(digest) != 64 or
                    any(char not in "0123456789abcdef" for char in digest)):
                raise ValueError("设备产品结果字段无效；状态为 unknown")
        elif value["state"] != "unknown":
            raise ValueError("设备产品结果缺少操作证据；状态为 unknown")
        return value


OTA_TARGETS = {
    "esp32c3/esp_base": ("esp_secure_boot_v2_rsa3072", 0x130000),
    "esp32/esp_base": ("esp_secure_boot_v1_ecdsa_p256", 0x120000),
}
OTA_PACKAGE_SLOT_BYTES = {"esp32c3/esp_base": 0x77000,
                          "esp32/esp_base": 0x82000}


def ota_result(port, current, operation_id, expected_sha256=None,
               expected_size_bytes=None, expected_package_mode=None,
               expected_package_sha256=None):
    canonical_id(operation_id)
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "ota.result", "parameters": {"operation_id": operation_id}})
    for value in read_result(port, request_id, time.monotonic() + 5):
        if value["device_id"] != current["device_id"]:
            raise ValueError("OTA 结果来自另一设备；状态为 unknown")
        result = value["result"]
        if result is None:
            if value["state"] not in {"unknown", "failed"}:
                raise ValueError("OTA 查询缺少持久结果；状态为 unknown")
            return value
        if not isinstance(result, dict) or set(result) != {
                "operation_id", "sha256", "image_size_bytes", "target", "target_slot",
                "package_mode", "package_sha256"}:
            raise ValueError("OTA 持久结果字段无效；状态为 unknown")
        digest = result["sha256"]
        target = result["target"]
        package_mode = result["package_mode"]
        package_digest = result["package_sha256"]
        if (result["operation_id"] != operation_id or
                not isinstance(digest, str) or len(digest) != 64 or
                set(digest) - set("0123456789abcdef") or digest == "0" * 64 or
                type(result["image_size_bytes"]) is not int or
                not isinstance(target, str) or target not in OTA_TARGETS or
                not 1 <= result["image_size_bytes"] <= OTA_TARGETS[target][1] or
                not isinstance(result["target_slot"], str) or
                result["target_slot"] not in {"ota_0", "ota_1"} or
                not isinstance(package_mode, str) or
                package_mode not in {"no_package", "reuse", "write"} or
                (package_mode == "no_package" and package_digest is not None) or
                (package_mode != "no_package" and
                 (not isinstance(package_digest, str) or
                  len(package_digest) != 64 or
                  set(package_digest) - set("0123456789abcdef") or
                  package_digest == "0" * 64)) or
                (expected_sha256 is not None and digest != expected_sha256) or
                (expected_size_bytes is not None and
                 result["image_size_bytes"] != expected_size_bytes) or
                (expected_package_mode is not None and
                 package_mode != expected_package_mode) or
                (expected_package_sha256 is not None and
                 package_digest != expected_package_sha256)):
            raise ValueError("OTA 持久结果与请求不符；状态为 unknown")
        return value


def ota_start(port, current, operation_id, image_file, image_url, target,
              package_mode="no_package", package_file=None, package_url=None,
              guest_abi_version=None, data_schema_version=None,
              trial_event_file=None):
    canonical_id(operation_id)
    if not isinstance(target, str) or target not in OTA_TARGETS or not isinstance(image_url, str) or not image_url.startswith("https://") or len(image_url) > 512:
        raise ValueError("OTA 目标或 HTTPS URL 无效；未发送命令")
    if not isinstance(package_mode, str) or package_mode not in {"no_package", "reuse", "write"}:
        raise ValueError("OTA 包模式无效；未发送命令")
    package_options = (package_file, guest_abi_version,
                       data_schema_version, trial_event_file)
    if package_mode == "no_package":
        if any(value is not None for value in package_options) or package_url is not None:
            raise ValueError("无包 OTA 不接受产品包参数；未发送命令")
    elif (any(value is None for value in package_options) or
          type(guest_abi_version) is not int or
          not 1 <= guest_abi_version <= 4294967295 or
          type(data_schema_version) is not int or
          not 1 <= data_schema_version <= 4294967295 or
          (package_mode == "reuse" and package_url is not None) or
          (package_mode == "write" and
           (not isinstance(package_url, str) or
            not package_url.startswith("https://") or len(package_url) > 1024))):
        raise ValueError("带包 OTA 参数无效；未发送命令")
    scheme, slot_size_bytes = OTA_TARGETS[target]
    descriptor = os.open(image_file, os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or not 1 <= before.st_size <= slot_size_bytes:
            raise ValueError("签名固件不是目标槽可容纳的普通文件；未发送命令")
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(descriptor, 65536)
            if not block:
                break
            size += len(block)
            if size > before.st_size:
                raise ValueError("签名固件读取期间发生变化；未发送命令")
            digest.update(block)
        after = os.fstat(descriptor)
        if (size != before.st_size or size != after.st_size or
                before.st_mtime_ns != after.st_mtime_ns):
            raise ValueError("签名固件读取期间发生变化；未发送命令")
    finally:
        os.close(descriptor)
    package_parameters = {"package_mode": package_mode}
    if package_mode != "no_package":
        descriptor = os.open(package_file, os.O_RDONLY | os.O_CLOEXEC |
                             getattr(os, "O_NOFOLLOW", 0))
        try:
            before = os.fstat(descriptor)
            if (not stat.S_ISREG(before.st_mode) or
                    not 1 <= before.st_size <= OTA_PACKAGE_SLOT_BYTES[target]):
                raise ValueError("签名包超过目标包槽或不是普通文件；未发送命令")
            package_digest = hashlib.sha256()
            package_size = 0
            while True:
                block = os.read(descriptor, 65536)
                if not block:
                    break
                package_size += len(block)
                if package_size > before.st_size:
                    raise ValueError("签名包读取期间发生变化；未发送命令")
                package_digest.update(block)
            after = os.fstat(descriptor)
            if (package_size != before.st_size or package_size != after.st_size or
                    before.st_mtime_ns != after.st_mtime_ns):
                raise ValueError("签名包读取期间发生变化；未发送命令")
        finally:
            os.close(descriptor)
        package_parameters.update({
            "package_sha256": package_digest.hexdigest(),
            "trial_event_sha256": trial_event_digest(trial_event_file),
            "package_size_bytes": package_size,
            "guest_abi_version": guest_abi_version,
            "data_schema_version": data_schema_version,
        })
        if package_mode == "write":
            package_parameters["package_url"] = package_url
    fresh = status(port)
    if fresh["device_id"] != current["device_id"] or fresh["boot_id"] != current["boot_id"]:
        raise ValueError("OTA 发送前设备已重启；未发送命令")
    if (package_mode == "no_package" and
            fresh["result"].get("capabilities", {}).get("ota") != "ready"):
        raise ValueError("设备 OTA 当前不可用；未发送命令")
    if package_mode == "reuse":
        binding = product_status(port, current)
        if (binding["state"] != "succeeded" or
                binding["result"]["package_sha256"] !=
                package_parameters["package_sha256"]):
            raise ValueError("当前确认包与复用请求不符；未发送命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "ota.start", "device_id": current["device_id"],
                "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000,
                "parameters": {"operation_id": operation_id, "image_url": image_url,
                               "sha256": digest.hexdigest(), "image_size_bytes": size,
                               "target": target, "signature": {"scheme": scheme},
                               **package_parameters}})
    try:
        for receipt in read_result(port, request_id, time.monotonic() + 5):
            if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
                raise ValueError("OTA 写回执来自另一设备或启动；按原操作 ID 查询")
            if receipt["result"] is not None:
                raise ValueError("OTA 写回执字段无效；按原操作 ID 查询")
            if receipt["state"] in {"failed", "expired", "unknown", "running"}:
                return receipt
            if receipt["state"] == "succeeded":
                raise ValueError("OTA 写回执缺少持久结果证明；按原操作 ID 查询")
    except TimeoutError:
        pass
    # New boot may close the current serial port. A separate ota.result call
    # uses the original operation ID; never resend this write.
    return {"device_id": current["device_id"], "boot_id": current["boot_id"],
            "request_id": request_id, "state": "unknown",
            "error_code": "ota_write_receipt_missing", "result": None}


def product_status(port, current):
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "product.status"})
    for value in read_result(port, request_id, time.monotonic() + 5):
        if value["device_id"] != current["device_id"] or value["boot_id"] != current["boot_id"]:
            raise ValueError("产品状态来自另一设备或启动；状态为 unknown")
        result = value["result"]
        if value["state"] == "unknown":
            if result is not None or value["error_code"] not in {
                    "product_ledger_uninitialized", "operation_busy", "storage_uncertain", "resource_failure"}:
                raise ValueError("设备产品状态的 unknown 证据无效")
            return value
        if value["state"] != "succeeded" or not isinstance(result, dict) or set(result) != {
                "operation_sequence_high_watermark", "next_operation_sequence", "pending_operation_id",
                "container_sequence", "package_sha256", "firmware_sha256",
                "runtime_guest_abi_version", "package_guest_abi_version", "package_data_schema_version", "active_product"}:
            raise ValueError("设备产品状态字段无效；状态为 unknown")
        watermark = result["operation_sequence_high_watermark"]
        next_sequence = result["next_operation_sequence"]
        pending = result["pending_operation_id"]
        container_sequence = result["container_sequence"]
        digest = result["package_sha256"]
        if (type(watermark) is not int or not 0 <= watermark <= 4294967295 or
                (watermark < 4294967295 and
                 (type(next_sequence) is not int or next_sequence != watermark + 1)) or
                (watermark == 4294967295 and next_sequence is not None) or
                (pending is not None and (watermark == 0 or canonical_id(pending) != pending)) or
                type(container_sequence) is not int or not 1 <= container_sequence <= 4294967295 or
                (digest is not None and
                 (not isinstance(digest, str) or len(digest) != 64 or
                  set(digest) - set("0123456789abcdef") or digest == "0" * 64))):
            raise ValueError("设备产品操作序号无效；状态为 unknown")
        firmware_digest = result["firmware_sha256"]
        runtime_abi = result["runtime_guest_abi_version"]
        package_abi = result["package_guest_abi_version"]
        schema = result["package_data_schema_version"]
        if (not isinstance(firmware_digest, str) or len(firmware_digest) != 64 or
                set(firmware_digest) - set("0123456789abcdef") or firmware_digest == "0" * 64 or
                type(runtime_abi) is not int or not 1 <= runtime_abi <= 4294967295 or
                (digest is None and (package_abi is not None or schema is not None)) or
                (digest is not None and (type(package_abi) is not int or not 1 <= package_abi <= 4294967295 or
                                        type(schema) is not int or not 1 <= schema <= 4294967295))):
            raise ValueError("设备固件摘要、运行时或绑定包元数据无效；状态为 unknown")
        active = result["active_product"]
        if active is not None:
            if not isinstance(active, dict) or set(active) != {
                    "product_id", "product_version", "package_sha256", "guest_abi_version",
                    "data_schema_version", "is_trial", "operation_id"}:
                raise ValueError("活动产品字段无效；状态为 unknown")
            identifiers = [active["product_id"], active["product_version"]]
            if (any(not isinstance(text, str) or re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", text) is None
                    for text in identifiers) or sum(map(len, identifiers)) > 4096 or
                    not isinstance(active["package_sha256"], str) or
                    re.fullmatch(r"[0-9a-f]{64}", active["package_sha256"]) is None or
                    active["package_sha256"] == "0" * 64 or
                    type(active["guest_abi_version"]) is not int or active["guest_abi_version"] != runtime_abi or
                    type(active["data_schema_version"]) is not int or
                    not 1 <= active["data_schema_version"] <= 4294967295 or type(active["is_trial"]) is not bool):
                raise ValueError("活动产品元数据无效；状态为 unknown")
            if active["is_trial"]:
                if pending is None or active["operation_id"] != pending:
                    raise ValueError("活动候选与待确认操作不一致；状态为 unknown")
            elif (active["operation_id"] is not None or active["package_sha256"] != digest or
                    active["guest_abi_version"] != package_abi or active["data_schema_version"] != schema):
                raise ValueError("活动产品与确认绑定不一致；状态为 unknown")
        return value


def product_lifecycle(port, current, command, operation_id,
                      expected_container_sequence, expected_package_sha256):
    canonical_id(operation_id)
    if (command not in {"product.stop", "product.start"} or
            type(expected_container_sequence) is not int or
            not 1 <= expected_container_sequence <= 4294967295 or
            not isinstance(expected_package_sha256, str) or
            re.fullmatch(r"[0-9a-f]{64}", expected_package_sha256) is None or
            expected_package_sha256 == "0" * 64):
        raise ValueError("产品停止／启动前置参数无效；未发送命令")
    snapshot = product_status(port, current)
    if snapshot["state"] != "succeeded":
        raise ValueError("产品绑定不可确认；未发送命令：" + str(snapshot["error_code"]))
    binding = snapshot["result"]
    active = binding["active_product"]
    if (binding["container_sequence"] != expected_container_sequence or
            binding["package_sha256"] != expected_package_sha256 or
            binding["pending_operation_id"] is not None or
            (active is not None and active["is_trial"])):
        raise ValueError("产品确认绑定与预期不符或仍在试运行；未发送命令")
    fresh = status(port)
    if (fresh["device_id"] != current["device_id"] or fresh["boot_id"] != current["boot_id"] or
            fresh["result"]["uptime_ms"] > 9007199254740991 - 10000):
        raise ValueError("产品状态查询后设备已重启或期限无效；未发送命令")
    deadline = time.monotonic() + 30
    request = {"protocol_version": 1, "request_id": operation_id, "command": command,
               "device_id": current["device_id"], "target_boot_id": current["boot_id"],
               "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000,
               "parameters": {"expected_container_sequence": expected_container_sequence,
                              "package_sha256": expected_package_sha256}}
    try:
        send(port, request)
        for receipt in read_result(port, operation_id, min(deadline, time.monotonic() + 5)):
            if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
                raise ValueError("产品停止／启动回执来自另一设备或启动；状态为 unknown")
            if receipt["result"] is not None:
                raise ValueError("产品停止／启动写回执字段无效；状态为 unknown")
            if receipt["state"] in {"failed", "expired"}:
                return receipt
            if receipt["state"] in {"running", "succeeded", "unknown"}:
                break
    except (TimeoutError, OSError):
        # A partial write or missing receipt cannot prove rejection. Query only.
        pass
    unconfirmed = {"protocol_version": 1, "device_id": current["device_id"],
                   "boot_id": current["boot_id"], "request_id": operation_id,
                   "state": "unknown", "error_code": "product_lifecycle_result_unconfirmed", "result": None}
    observed = unconfirmed
    while time.monotonic() < deadline:
        try:
            observed = product_result(port, current, operation_id,
                                      min(deadline, time.monotonic() + 5))
        except TimeoutError:
            pass
        except OSError:
            return unconfirmed
        else:
            evidence = observed["result"]
            if evidence is not None:
                if (evidence["kind"] != command.removeprefix("product.") or
                        evidence["operation_sequence"] is not None or
                        evidence["container_sequence"] != expected_container_sequence):
                    raise ValueError("产品停止／启动结果与原请求不符；状态为 unknown")
                if observed["state"] in {"succeeded", "failed"}:
                    return observed
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(0.25, remaining))
    if observed["state"] == "running":
        return unconfirmed
    return observed


def product_uninstall(port, current, operation_id, operation_sequence,
                      expected_container_sequence, expected_package_sha256):
    canonical_id(operation_id)
    if (type(operation_sequence) is not int or not 1 <= operation_sequence <= 4294967295 or
            type(expected_container_sequence) is not int or
            not 1 <= expected_container_sequence < 4294967295 or
            not isinstance(expected_package_sha256, str) or
            len(expected_package_sha256) != 64 or
            set(expected_package_sha256) - set("0123456789abcdef") or
            expected_package_sha256 == "0" * 64):
        raise ValueError("产品卸载前置参数无效；未发送命令")
    snapshot = product_status(port, current)
    if snapshot["state"] != "succeeded":
        raise ValueError("产品绑定不可确认；未发送命令：" + str(snapshot["error_code"]))
    binding = snapshot["result"]
    if (binding["next_operation_sequence"] != operation_sequence or
            binding["container_sequence"] != expected_container_sequence or
            binding["package_sha256"] != expected_package_sha256 or
            binding["pending_operation_id"] is not None):
        raise ValueError("产品持久序号或包绑定与预期不符；未发送命令")
    fresh = status(port)
    if (fresh["device_id"] != current["device_id"] or
            fresh["boot_id"] != current["boot_id"]):
        raise ValueError("产品状态查询后设备已重启；未发送命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "product.uninstall", "device_id": current["device_id"],
                "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000,
                "parameters": {"operation_id": operation_id,
                               "operation_sequence": operation_sequence,
                               "expected_container_sequence": expected_container_sequence,
                               "package_sha256": expected_package_sha256}})
    receipt_succeeded = False
    try:
        for receipt in read_result(port, request_id, time.monotonic() + 30):
            if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
                raise ValueError("产品卸载回执来自另一设备或启动；按原操作 ID 查询")
            if receipt["result"] is not None:
                raise ValueError("产品卸载回执字段无效；按原操作 ID 查询")
            if receipt["state"] in {"succeeded", "unknown"}:
                receipt_succeeded = receipt["state"] == "succeeded"
                break
            if receipt["state"] in {"failed", "expired"}:
                return receipt
    except TimeoutError:
        pass
    # A timeout is unknown, so query the durable ledger by the original ID.
    # Never resend the product write, even if the read-only query also times out.
    result = product_result(port, current, operation_id)
    evidence = result["result"]
    if result["state"] == "succeeded":
        if (not isinstance(evidence, dict) or evidence["kind"] != "uninstall" or
                evidence["operation_sequence"] != operation_sequence or
                evidence["container_sequence"] != expected_container_sequence + 1 or
                evidence["package_sha256"] != expected_package_sha256):
            raise ValueError("产品卸载终态与预期不符；状态为 unknown")
        return result
    if receipt_succeeded:
        raise ValueError("设备报告卸载成功但持久结果未确认；按原操作 ID 查询")
    return result


def trial_event_digest(path):
    maximum = 4096 - 65 - len(b"esp-base-product-event-v1\n") - 36 - 36 - 32 - 8
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or not 1 <= before.st_size <= maximum:
            raise ValueError("代表事件不是设备可接收大小的普通文件；未发送命令")
        event = os.read(descriptor, maximum + 1)
        after = os.fstat(descriptor)
        if (len(event) != before.st_size or before.st_size != after.st_size or
                before.st_mtime_ns != after.st_mtime_ns):
            raise ValueError("代表事件读取期间发生变化；未发送命令")
        return hashlib.sha256(event).hexdigest()
    finally:
        os.close(descriptor)


def product_package(port, current, command, operation_id, operation_sequence,
                    expected_container_sequence, expected_package_sha256,
                    package_file, package_url, guest_abi_version, data_schema_version,
                    trial_event_file):
    canonical_id(operation_id)
    if (command not in {"product.install", "product.upgrade"} or
            type(operation_sequence) is not int or not 1 <= operation_sequence <= 4294967295 or
            type(expected_container_sequence) is not int or
            not 1 <= expected_container_sequence <= 4294967295 - 5 or
            type(guest_abi_version) is not int or not 1 <= guest_abi_version <= 4294967295 or
            type(data_schema_version) is not int or not 1 <= data_schema_version <= 4294967295 or
            not isinstance(package_url, str) or not package_url.startswith("https://")):
        raise ValueError("产品包参数无效；未发送命令")
    if command == "product.install":
        if expected_package_sha256 is not None:
            raise ValueError("安装必须从无包绑定开始；未发送命令")
    elif (not isinstance(expected_package_sha256, str) or
          len(expected_package_sha256) != 64 or
          set(expected_package_sha256) - set("0123456789abcdef") or
          expected_package_sha256 == "0" * 64):
        raise ValueError("升级的旧包摘要无效；未发送命令")

    descriptor = os.open(package_file, os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= 2147483647:
            raise ValueError("产品包不是合法大小的普通文件；未发送命令")
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(descriptor, 65536)
            if not block:
                break
            size += len(block)
            if size > before.st_size:
                raise ValueError("产品包读取期间发生变化；未发送命令")
            digest.update(block)
        after = os.fstat(descriptor)
        if (size != before.st_size or size != after.st_size or
                before.st_mtime_ns != after.st_mtime_ns):
            raise ValueError("产品包读取期间发生变化；未发送命令")
    finally:
        os.close(descriptor)
    package_sha256 = digest.hexdigest()
    trial_event_sha256 = trial_event_digest(trial_event_file)
    snapshot = product_status(port, current)
    if snapshot["state"] != "succeeded":
        raise ValueError("产品绑定不可确认；未发送命令：" + str(snapshot["error_code"]))
    binding = snapshot["result"]
    if (binding["next_operation_sequence"] != operation_sequence or
            binding["container_sequence"] != expected_container_sequence or
            binding["package_sha256"] != expected_package_sha256 or
            binding["pending_operation_id"] is not None):
        raise ValueError("产品持久序号或旧包绑定与预期不符；未发送命令")
    fresh = status(port)
    if fresh["device_id"] != current["device_id"] or fresh["boot_id"] != current["boot_id"]:
        raise ValueError("产品状态查询后设备已重启；未发送命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": command, "device_id": current["device_id"],
                "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000,
                "parameters": {"operation_id": operation_id,
                               "operation_sequence": operation_sequence,
                               "expected_container_sequence": expected_container_sequence,
                               "previous_package_sha256": expected_package_sha256,
                               "package_url": package_url,
                               "package_sha256": package_sha256,
                               "trial_event_sha256": trial_event_sha256,
                               "package_size_bytes": size,
                               "guest_abi_version": guest_abi_version,
                               "data_schema_version": data_schema_version}})
    try:
        for receipt in read_result(port, request_id, time.monotonic() + 5):
            if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
                raise ValueError("产品包回执来自另一设备或启动；按原操作 ID 查询")
            if receipt["result"] is not None:
                raise ValueError("产品包回执字段无效；按原操作 ID 查询")
            if receipt["state"] in {"failed", "expired"}:
                return receipt
            break
    except TimeoutError:
        pass
    result = product_result(port, current, operation_id)
    evidence = result["result"]
    if evidence is not None and (evidence["operation_sequence"] != operation_sequence or
                                 evidence["kind"] != command.split(".")[1] or
                                 evidence["package_sha256"] != package_sha256):
        raise ValueError("产品包持久结果与请求不符；状态为 unknown")
    if result["state"] in {"succeeded", "failed"}:
        return result
    return result


def validate_configuration(config):
    if not isinstance(config, dict) or set(config) != {"schema_version", "wifi", "mqtt", "frp", "business"}:
        raise ValueError("配置字段不完整")
    if type(config["schema_version"]) is not int or config["schema_version"] != 3 or config["business"] is not None:
        raise ValueError("配置版本或能力尚未支持")
    network = config["wifi"]
    if network is not None:
        if not isinstance(network, dict) or set(network) != {"ssid", "password"}:
            raise ValueError("Wi-Fi 配置字段无效")
        ssid, password = network["ssid"], network["password"]
        if not isinstance(ssid, str):
            raise ValueError("SSID 长度或字符无效")
        try:
            ssid_bytes = ssid.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise ValueError("SSID 长度或字符无效") from exc
        if not 1 <= len(ssid_bytes) <= 32 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
            raise ValueError("SSID 长度或字符无效")
        if not isinstance(password, str) or not ((8 <= len(password) <= 63 and all(32 <= ord(c) <= 126 for c in password)) or
                (len(password) == 64 and all(c in "0123456789abcdefABCDEF" for c in password))):
            raise ValueError("Wi-Fi 密码格式无效")
    mqtt = config["mqtt"]
    if mqtt is not None:
        required = {"hostname", "port", "username", "password", "ca_pem", "management_key_hex"}
        if not isinstance(mqtt, dict) or set(mqtt) != required:
            raise ValueError("MQTT 配置字段无效")
        host = mqtt["hostname"]
        if not isinstance(host, str) or not 1 <= len(host) <= 253 or not host.isascii():
            raise ValueError("MQTT 主机无效")
        for label in host.split("."):
            if not 1 <= len(label) <= 63 or not label[0].isalnum() or not label[-1].isalnum() or not all(
                    ("a" <= char <= "z") or ("A" <= char <= "Z") or ("0" <= char <= "9") or char == "-"
                    for char in label):
                raise ValueError("MQTT 主机无效")
        port = mqtt["port"]
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("MQTT 端口无效")
        for name, limit in (("username", 128), ("password", 256)):
            value = mqtt[name]
            if not isinstance(value, str):
                raise ValueError("MQTT 设备凭据无效")
            try:
                encoded = value.encode("utf-8")
            except UnicodeEncodeError as exc:
                raise ValueError("MQTT 设备凭据无效") from exc
            if not 1 <= len(encoded) <= limit or any(
                    ord(char) < 32 or 127 <= ord(char) <= 159 or
                    0xFDD0 <= ord(char) <= 0xFDEF or ord(char) & 0xFFFF >= 0xFFFE
                    for char in value):
                raise ValueError("MQTT 设备凭据无效")
        ca = mqtt["ca_pem"]
        if (not isinstance(ca, str) or not 1 <= len(ca) <= 4096 or not all(
                char in "\t\r\n" or 32 <= ord(char) <= 126 for char in ca) or
                "-----BEGIN CERTIFICATE-----" not in ca or "-----END CERTIFICATE-----" not in ca):
            raise ValueError("MQTT CA PEM 无效")
        key = mqtt["management_key_hex"]
        if (not isinstance(key, str) or len(key) != 64 or any(char not in "0123456789abcdef" for char in key) or
                not any(char != "0" for char in key)):
            raise ValueError("MQTT 管理密钥无效")
    frp = config["frp"]
    if frp is not None:
        required = {"server_hostname", "server_port", "token", "ca_pem", "proxy_name",
                    "remote_port", "local_port", "management_key_hex"}
        if not isinstance(frp, dict) or set(frp) != required:
            raise ValueError("FRP 配置字段无效")
        host = frp["server_hostname"]
        if not isinstance(host, str) or not 1 <= len(host) <= 253 or not host.isascii():
            raise ValueError("FRP 主机无效")
        for label in host.split("."):
            if not 1 <= len(label) <= 63 or not label[0].isalnum() or not label[-1].isalnum() or not all(
                    ("a" <= char <= "z") or ("A" <= char <= "Z") or ("0" <= char <= "9") or char == "-"
                    for char in label):
                raise ValueError("FRP 主机无效")
        for name in ("server_port", "remote_port", "local_port"):
            value = frp[name]
            if type(value) is not int or not 1 <= value <= 65535:
                raise ValueError("FRP 端口无效")
        token = frp["token"]
        if not isinstance(token, str) or not 1 <= len(token) <= 256 or not all(32 < ord(c) < 127 for c in token):
            raise ValueError("FRP Token 无效")
        ca = frp["ca_pem"]
        if (not isinstance(ca, str) or not 1 <= len(ca) <= 2048 or not all(
                char in "\t\r\n" or 32 <= ord(char) <= 126 for char in ca) or
                "-----BEGIN CERTIFICATE-----" not in ca or "-----END CERTIFICATE-----" not in ca):
            raise ValueError("FRP CA PEM 无效")
        proxy = frp["proxy_name"]
        if (not isinstance(proxy, str) or not 1 <= len(proxy) <= 128 or
                any(ord(c) <= 32 or ord(c) >= 127 or c in "/\\*@" for c in proxy)):
            raise ValueError("FRP 代理名无效")
        key = frp["management_key_hex"]
        if (not isinstance(key, str) or len(key) != 64 or any(char not in "0123456789abcdef" for char in key) or
                not any(char != "0" for char in key)):
            raise ValueError("FRP 管理密钥无效")


def apply_configuration(port, current, config):
    validate_configuration(config)
    revision = current["result"]["revision"]
    if type(revision) is not int or not 0 <= revision < 4294967295:
        raise ValueError("配置 revision 无效或已耗尽")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id, "command": "config.set",
                "device_id": current["device_id"], "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": current["result"]["uptime_ms"] + 10000,
                "parameters": {"expected_revision": revision, "config": config}})
    for receipt in read_result(port, request_id, time.monotonic() + 30):
        if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
            continue
        if receipt["state"] in {"failed", "expired", "unknown"}:
            raise ValueError("配置未确认提交：" + str(receipt["error_code"]))
        if receipt["state"] == "succeeded":
            if not isinstance(receipt["result"], dict) or receipt["result"].get("revision") != revision + 1:
                raise ValueError("配置提交缺少 revision 证据；状态为 unknown")
            return receipt


def load_private_config(path):
    import os
    import stat
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as source:
        facts = os.fstat(source.fileno())
        if not stat.S_ISREG(facts.st_mode) or facts.st_uid != os.getuid() or facts.st_mode & 0o077 or facts.st_nlink != 1:
            raise ValueError("配置文件必须由当前用户独占，权限 0600，且不是链接")
        raw = source.read(8193)
    if len(raw) > 8192:
        raise ValueError("配置文件超限")
    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="本轮枚举的 C3 USB Serial/JTAG 或 ESP32 UART 端点")
    parser.add_argument("--device-id", help="预期持久 UUID；写命令必填")
    parser.add_argument("--config-file", help="本机 0600 JSON 完整配置文件；仅用于 config.set")
    parser.add_argument("--operation-id", help="产品或 OTA 写入及结果查询的原始操作 UUID")
    parser.add_argument("--image-file", help="ota.start 时用于计算完整摘要和长度的本地签名固件")
    parser.add_argument("--image-url", help="设备下载同一签名固件的 HTTPS URL")
    parser.add_argument("--ota-target", choices=sorted(OTA_TARGETS), help="固件构建的精确 target")
    parser.add_argument("--ota-package-mode", choices=("no_package", "reuse", "write"),
                        help="ota.start 的联合产品包模式；默认 no_package")
    parser.add_argument("--operation-sequence", type=int, help="仅安装、升级或卸载的持久操作序号")
    parser.add_argument("--expected-container-sequence", type=int,
                        help="产品写入的当前 ECS2 序号")
    parser.add_argument("--expected-package-sha256", help="卸载、升级、停止或启动的当前确认包 SHA-256")
    parser.add_argument("--package-file", help="安装／升级时用于计算整包摘要和长度的本地签名包")
    parser.add_argument("--trial-event-file", help="安装／升级时绑定随后应发布的原始代表业务事件")
    parser.add_argument("--package-url", help="设备下载同一签名包的 HTTPS URL")
    parser.add_argument("--guest-abi-version", type=int, help="签名包声明的 guest ABI 版本")
    parser.add_argument("--data-schema-version", type=int, help="签名包声明的数据 schema 版本")
    parser.add_argument("--json", action="store_true", help="输出纯 JSON 设备结果")
    parser.add_argument("command", choices=["status", "restart", "config.set", "ota.start", "ota.result", "product.status",
                                            "product.result", "product.uninstall",
                                            "product.install", "product.upgrade", "product.stop", "product.start"])
    args = parser.parse_args()
    persistent_writes = {"product.uninstall", "product.install", "product.upgrade"}
    lifecycle_writes = {"product.stop", "product.start"}
    product_writes = persistent_writes | lifecycle_writes
    package_writes = {"product.install", "product.upgrade"}
    if args.command in {"restart", "config.set", "ota.start"} | product_writes and not args.device_id:
        parser.error("写命令必须指定已核对的 --device-id")
    if (args.command == "config.set") != bool(args.config_file):
        parser.error("config.set 必须且只能配合 --config-file")
    if (args.command in product_writes | {"product.result", "ota.start", "ota.result"}) != bool(args.operation_id):
        parser.error("产品／OTA 写入和结果查询必须且只能配合 --operation-id")
    image_options = (args.image_file, args.image_url, args.ota_target)
    if args.command == "ota.start":
        if any(value is None for value in image_options):
            parser.error("ota.start 必须提供本地签名固件、HTTPS URL 和精确 target")
    elif any(value is not None for value in image_options):
        parser.error("固件参数只能用于 ota.start")
    if args.command != "ota.start" and args.ota_package_mode is not None:
        parser.error("--ota-package-mode 只能用于 ota.start")
    ota_package_mode = args.ota_package_mode or "no_package"
    if args.command in persistent_writes:
        if args.operation_sequence is None or args.expected_container_sequence is None:
            parser.error("产品写入必须提供操作序号和 Container 序号")
        if args.command != "product.install" and args.expected_package_sha256 is None:
            parser.error("产品升级／卸载必须提供当前包摘要")
        if args.command == "product.install" and args.expected_package_sha256 is not None:
            parser.error("产品安装必须从无包绑定开始")
    elif args.command in lifecycle_writes:
        if args.operation_sequence is not None:
            parser.error("产品停止／启动不接受持久 --operation-sequence")
        if args.expected_container_sequence is None or args.expected_package_sha256 is None:
            parser.error("产品停止／启动必须提供当前 Container 序号和确认包摘要")
    elif any(value is not None for value in (args.operation_sequence,
                                             args.expected_container_sequence,
                                             args.expected_package_sha256)):
        parser.error("产品写入前置参数只能用于产品写命令")
    package_options = (args.package_file, args.package_url,
                       args.guest_abi_version, args.data_schema_version,
                       args.trial_event_file)
    if args.command in package_writes:
        if any(value is None for value in package_options):
            parser.error("产品安装／升级必须提供本地包、HTTPS URL、guest ABI、数据 schema 和代表事件")
    elif args.command == "ota.start" and ota_package_mode != "no_package":
        required = (args.package_file, args.guest_abi_version,
                    args.data_schema_version, args.trial_event_file)
        if any(value is None for value in required):
            parser.error("带包 OTA 必须提供本地签名包、guest ABI、数据 schema 和代表事件")
        if ota_package_mode == "write" and args.package_url is None:
            parser.error("WRITE OTA 必须提供包的 HTTPS URL")
        if ota_package_mode == "reuse" and args.package_url is not None:
            parser.error("REUSE OTA 不下载新包，不接受包 URL")
    elif any(value is not None for value in package_options):
        parser.error("产品包参数只能用于产品安装／升级或带包 OTA")
    if args.operation_id:
        canonical_id(args.operation_id)
    config = load_private_config(args.config_file) if args.config_file else None
    if args.device_id:
        canonical_id(args.device_id)
    port = SerialPort(args.port)
    try:
        wait_ready(port)
        current = status(port)
        if args.device_id and current["device_id"] != args.device_id:
            raise ValueError("设备 UUID 与指定目标不一致")
        if args.command == "restart":
            request_id = str(uuid.uuid4())
            send(port, {"protocol_version": 1, "request_id": request_id, "command": "restart",
                        "device_id": current["device_id"], "target_boot_id": current["boot_id"],
                        "expires_at_uptime_ms": current["result"]["uptime_ms"] + 10000, "parameters": {}})
            for receipt in read_result(port, request_id, time.monotonic() + 5):
                if receipt["device_id"] != current["device_id"] or receipt["boot_id"] != current["boot_id"]:
                    continue
                if receipt["state"] in {"failed", "expired", "unknown"}:
                    raise ValueError("设备拒绝重启：" + str(receipt["error_code"]))
                if receipt["state"] == "running":
                    break
            time.sleep(0.5)
            observed = status(port)
            if observed["device_id"] != current["device_id"] or observed["boot_id"] == current["boot_id"]:
                raise ValueError("尚未观察到同一设备的新启动；状态为 unknown")
            current = dict(observed, request_id=request_id)
        if args.command == "config.set":
            current = apply_configuration(port, current, config)
        if args.command == "ota.start":
            current = ota_start(port, current, args.operation_id,
                                args.image_file, args.image_url, args.ota_target,
                                ota_package_mode, args.package_file,
                                args.package_url, args.guest_abi_version,
                                args.data_schema_version, args.trial_event_file)
        if args.command == "ota.result":
            current = ota_result(port, current, args.operation_id)
        if args.command == "product.result":
            current = product_result(port, current, args.operation_id)
        if args.command == "product.status":
            current = product_status(port, current)
        if args.command == "product.uninstall":
            current = product_uninstall(port, current, args.operation_id,
                                        args.operation_sequence, args.expected_container_sequence,
                                        args.expected_package_sha256)
        if args.command in lifecycle_writes:
            current = product_lifecycle(port, current, args.command, args.operation_id,
                                        args.expected_container_sequence, args.expected_package_sha256)
        if args.command in package_writes:
            current = product_package(port, current, args.command, args.operation_id,
                                      args.operation_sequence, args.expected_container_sequence,
                                      args.expected_package_sha256, args.package_file,
                                      args.package_url, args.guest_abi_version,
                                      args.data_schema_version, args.trial_event_file)
        if args.json:
            print(json.dumps(current, ensure_ascii=False))
        else:
            print("ESP Base 串口操作\n  状态  " + current["state"] + "\n  设备  " + current["device_id"] + "\n  启动  " + current["boot_id"])
            if current["error_code"]:
                print("  原因  " + current["error_code"])
            if args.command in product_writes | {"product.result", "ota.start", "ota.result"}:
                print("  操作  " + args.operation_id)
                if args.command in product_writes | {"product.result"} and current["result"] is not None:
                    if current["result"]["kind"] in {"stop", "start"}:
                        print("  作用域  当前启动")
                        print("  Container 序号  " + str(current["result"]["container_sequence"]))
                    else:
                        print("  序号  " + str(current["result"]["operation_sequence"]))
                if args.command in lifecycle_writes:
                    print("  后续  当前启动内用原操作 UUID 查询 product.result；不要重发写命令")
                if args.command == "ota.start":
                    print("  后续  用原操作 UUID 查询 ota.result；不要重发 ota.start")
                if args.command == "ota.result" and current["result"] is not None:
                    print("  固件 SHA-256  " + current["result"]["sha256"])
                    print("  目标  " + current["result"]["target"] + " / " +
                          current["result"]["target_slot"])
                    print("  包模式  " + current["result"]["package_mode"])
                    if current["result"]["package_sha256"] is not None:
                        print("  包 SHA-256  " + current["result"]["package_sha256"])
            if args.command == "product.status" and current["result"] is not None:
                print("  高水位  " + str(current["result"]["operation_sequence_high_watermark"]))
                print("  Container 序号  " + str(current["result"]["container_sequence"]))
                print("  运行固件 SHA-256  " + current["result"]["firmware_sha256"])
                print("  运行时 guest ABI  " + str(current["result"]["runtime_guest_abi_version"]))
                print("  绑定包 guest ABI  " + str(current["result"]["package_guest_abi_version"]))
                print("  绑定包数据 schema  " + str(current["result"]["package_data_schema_version"]))
                active = current["result"]["active_product"]
                if active is None:
                    print("  活动实例          未取得可确认的活动实例")
                else:
                    print("  活动产品 ID       " + active["product_id"])
                    print("  活动产品版本      " + active["product_version"])
                    print("  活动包 SHA-256    " + active["package_sha256"])
                    print("  活动状态          " + ("候选试运行" if active["is_trial"] else "已确认绑定"))
                    print("  试运行原操作 ID   " + (active["operation_id"] or "无"))
                print("  当前包 SHA-256  " + str(current["result"]["package_sha256"]))
                print("  下一序号  " + str(current["result"]["next_operation_sequence"]))
                if current["result"]["pending_operation_id"] is not None:
                    print("  未决操作  " + current["result"]["pending_operation_id"])
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("ESP Base 串口操作失败\n  原因  " + str(error), file=sys.stderr)
        sys.exit(1)
