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


OTA_TARGETS = {
    "esp32c3/esp_base": ("esp_secure_boot_v2_rsa3072", 0x1e0000),
    "esp32/esp_base": ("esp_secure_boot_v1_ecdsa_p256", 0x1e0000),
}


def ota_result(port, current, operation_id, expected_sha256=None,
               expected_size_bytes=None):
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
                "operation_id", "sha256", "image_size_bytes", "target", "target_slot"}:
            raise ValueError("OTA 持久结果字段无效；状态为 unknown")
        digest = result["sha256"]
        target = result["target"]
        if (result["operation_id"] != operation_id or
                not isinstance(digest, str) or len(digest) != 64 or
                set(digest) - set("0123456789abcdef") or digest == "0" * 64 or
                type(result["image_size_bytes"]) is not int or
                not isinstance(target, str) or target not in OTA_TARGETS or
                not 1 <= result["image_size_bytes"] <= OTA_TARGETS[target][1] or
                not isinstance(result["target_slot"], str) or
                result["target_slot"] not in {"ota_0", "ota_1"} or
                (expected_sha256 is not None and digest != expected_sha256) or
                (expected_size_bytes is not None and
                 result["image_size_bytes"] != expected_size_bytes)):
            raise ValueError("OTA 持久结果与请求不符；状态为 unknown")
        return value


def ota_start(port, current, operation_id, image_file, image_url, target):
    canonical_id(operation_id)
    if not isinstance(target, str) or target not in OTA_TARGETS or not isinstance(image_url, str) or not image_url.startswith("https://") or len(image_url) > 512:
        raise ValueError("OTA 目标或 HTTPS URL 无效；未发送命令")
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
    fresh = status(port)
    if fresh["device_id"] != current["device_id"] or fresh["boot_id"] != current["boot_id"]:
        raise ValueError("OTA 发送前设备已重启；未发送命令")
    if fresh["result"].get("capabilities", {}).get("ota") != "ready":
        raise ValueError("设备 OTA 当前不可用；未发送命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id,
                "command": "ota.start", "device_id": current["device_id"],
                "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000,
                "parameters": {"operation_id": operation_id, "image_url": image_url,
                               "sha256": digest.hexdigest(), "image_size_bytes": size,
                               "target": target, "signature": {"scheme": scheme}}})
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


def query_capability(port, current, command):
    if command not in {"firmware.status", "business.status"}:
        raise ValueError("未知只读能力")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id, "command": command})
    for value in read_result(port, request_id, time.monotonic() + 5):
        if value["device_id"] != current["device_id"] or value["boot_id"] != current["boot_id"]:
            raise ValueError("状态来自另一设备或启动；状态为 unknown")
        result = value["result"]
        if value["state"] != "succeeded":
            if result is not None or value["state"] not in {"failed", "unknown"}:
                raise ValueError("状态回执无效；状态为 unknown")
            return value
        if command == "firmware.status":
            if (not isinstance(result, dict) or set(result) != {
                    "firmware_sha256", "image_size_bytes", "target", "ota_slot"} or
                    not isinstance(result["firmware_sha256"], str) or
                    re.fullmatch(r"[0-9a-f]{64}", result["firmware_sha256"]) is None or
                    result["firmware_sha256"] == "0" * 64 or
                    not isinstance(result["target"], str) or result["target"] not in OTA_TARGETS or
                    type(result["image_size_bytes"]) is not int or
                    not 1 <= result["image_size_bytes"] <= OTA_TARGETS[result["target"]][1] or
                    not isinstance(result["ota_slot"], str) or
                    result["ota_slot"] not in {"ota_0", "ota_1"}):
                raise ValueError("固件状态字段无效；状态为 unknown")
        elif (not isinstance(result, dict) or set(result) != {
                "byte_count", "state", "window_deadline_uptime_ms"} or
                type(result["byte_count"]) is not int or not 0 <= result["byte_count"] <= 4294967295 or
                not isinstance(result["state"], str) or result["state"] not in {"idle", "active", "paused"} or
                type(result["window_deadline_uptime_ms"]) is not int or
                not 0 <= result["window_deadline_uptime_ms"] <= 9007199254740991):
            raise ValueError("原生业务状态字段无效；状态为 unknown")
        return value


def business_action(port, current, command):
    if command not in {"business.pause", "business.resume"}:
        raise ValueError("未知原生业务操作")
    fresh = status(port)
    if fresh["device_id"] != current["device_id"] or fresh["boot_id"] != current["boot_id"]:
        raise ValueError("业务发送前设备或启动已改变；未发送命令")
    request_id = str(uuid.uuid4())
    send(port, {"protocol_version": 1, "request_id": request_id, "command": command,
                "device_id": current["device_id"], "target_boot_id": current["boot_id"],
                "expires_at_uptime_ms": fresh["result"]["uptime_ms"] + 10000, "parameters": {}})
    try:
        for value in read_result(port, request_id, time.monotonic() + 5):
            if value["device_id"] != current["device_id"] or value["boot_id"] != current["boot_id"] or value["result"] is not None:
                raise ValueError("业务写回执不符；状态为 unknown，不自动重发")
            return value
    except TimeoutError:
        return {"device_id": current["device_id"], "boot_id": current["boot_id"],
                "request_id": request_id, "state": "unknown",
                "error_code": "business_write_receipt_missing", "result": None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="本轮枚举的 C3 USB Serial/JTAG 或 ESP32 UART 端点")
    parser.add_argument("--device-id", help="预期持久 UUID；写命令必填")
    parser.add_argument("--config-file", help="本机 0600 JSON 完整配置文件；仅用于 config.set")
    parser.add_argument("--operation-id", help="固件 OTA 写入及结果查询的原始操作 UUID")
    parser.add_argument("--image-file", help="ota.start 的本地完整签名固件")
    parser.add_argument("--image-url", help="设备下载同一签名固件的 HTTPS URL")
    parser.add_argument("--ota-target", choices=sorted(OTA_TARGETS), help="固件构建的精确 target")
    parser.add_argument("--json", action="store_true", help="输出纯 JSON 设备结果")
    parser.add_argument("command", choices=["status", "firmware.status", "business.status",
        "business.pause", "business.resume", "restart", "config.set", "ota.start", "ota.result"])
    args = parser.parse_args()
    if args.command in {"restart", "config.set", "ota.start", "business.pause", "business.resume"} and not args.device_id:
        parser.error("写命令必须指定已核对的 --device-id")
    if (args.command == "config.set") != bool(args.config_file):
        parser.error("config.set 必须且只能配合 --config-file")
    if (args.command in {"ota.start", "ota.result"}) != bool(args.operation_id):
        parser.error("OTA 写入和结果查询必须且只能配合 --operation-id")
    image_options = (args.image_file, args.image_url, args.ota_target)
    if args.command == "ota.start":
        if any(value is None for value in image_options):
            parser.error("ota.start 必须提供本地签名固件、HTTPS URL 和精确 target")
    elif any(value is not None for value in image_options):
        parser.error("固件参数只能用于 ota.start")
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
        elif args.command == "config.set":
            current = apply_configuration(port, current, config)
        elif args.command == "ota.start":
            current = ota_start(port, current, args.operation_id,
                                args.image_file, args.image_url, args.ota_target)
        elif args.command == "ota.result":
            current = ota_result(port, current, args.operation_id)
        elif args.command in {"firmware.status", "business.status"}:
            current = query_capability(port, current, args.command)
        elif args.command in {"business.pause", "business.resume"}:
            current = business_action(port, current, args.command)
        if args.json:
            print(json.dumps(current, ensure_ascii=False))
        else:
            print("ESP Base 串口操作\n  状态  " + current["state"] + "\n  设备  " + current["device_id"] + "\n  启动  " + current["boot_id"])
            if current["error_code"]:
                print("  原因  " + current["error_code"])
            if args.command in {"ota.start", "ota.result"}:
                print("  操作  " + args.operation_id)
                if args.command == "ota.start":
                    print("  后续  用原操作 UUID 查询 ota.result；不要重发 ota.start")
                elif current["result"] is not None:
                    print("  固件 SHA-256  " + current["result"]["sha256"])
                    print("  目标  " + current["result"]["target"] + " / " + current["result"]["target_slot"])
            elif args.command == "firmware.status" and current["result"] is not None:
                print("  固件 SHA-256  " + current["result"]["firmware_sha256"])
                print("  目标  " + current["result"]["target"] + " / " + current["result"]["ota_slot"])
            elif args.command == "business.status" and current["result"] is not None:
                print("  业务状态  " + current["result"]["state"])
                print("  消息字节  " + str(current["result"]["byte_count"]))
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("ESP Base 串口操作失败\n  原因  " + str(error), file=sys.stderr)
        sys.exit(1)
