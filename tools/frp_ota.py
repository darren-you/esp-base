#!/usr/bin/env python3
"""经设备 FRP 管理入口上传完整签名固件；宿主工具，不在 MCU 上运行。"""
import argparse
import hashlib
import hmac
import json
import os
import re
import socket
import ssl
import stat
import subprocess
import sys
import time
import uuid
from pathlib import Path
from urllib.parse import urlsplit

from device_control import OTA_TARGETS, canonical_id, unique_object

COMMAND_PATHS = {
    "status": "status", "firmware.status": "firmware-status",
    "ota.start": "ota-start", "ota.result": "ota-result",
    "business.status": "business-status", "business.pause": "business-pause",
    "business.resume": "business-resume",
}
MAX_RESPONSE_BYTES = 8192
UPLOAD_CHUNK_BYTES = 4096
DNS_LOOKUP = (
    "import json,socket,sys; "
    "print(json.dumps(socket.getaddrinfo(sys.argv[1],int(sys.argv[2]),"
    "0,socket.SOCK_STREAM)))"
)


class UnknownOperation(RuntimeError):
    """写操作已经尝试；只按 operation_id 查询，不重发或另起操作。"""

    def __init__(self, operation_id, message):
        self.operation_id = operation_id
        super().__init__(message)


def wire_json(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def upload_metadata(operation_id, device_id, boot_id, size_bytes, sha256):
    for value in (operation_id, device_id, boot_id):
        canonical_id(value)
    if type(size_bytes) is not int or not 1 <= size_bytes <= 0xffffffff:
        raise ValueError("完整签名镜像尺寸无效")
    if not isinstance(sha256, str) or not re.fullmatch("[0-9a-f]{64}", sha256):
        raise ValueError("完整签名镜像摘要无效")
    return (f"esp-base-ota-upload-v1\n{operation_id}\n{device_id}\n{boot_id}\n"
            f"{size_bytes}\n{sha256}\n").encode("ascii")


def validate_envelope(value, device_id, request_id, boot_id=None):
    if not isinstance(value, dict) or set(value) != {
            "protocol_version", "device_id", "boot_id", "request_id", "state", "error_code", "result"}:
        raise ValueError("设备响应字段无效")
    if type(value["protocol_version"]) is not int or value["protocol_version"] != 1:
        raise ValueError("设备响应版本无效")
    canonical_id(value["device_id"])
    canonical_id(value["boot_id"])
    canonical_id(value["request_id"])
    if value["device_id"] != device_id or value["request_id"] != request_id or (
            boot_id is not None and value["boot_id"] != boot_id):
        raise ValueError("设备、请求或启动身份不符")
    if value["state"] not in {"accepted", "running", "succeeded", "failed", "expired", "unknown"}:
        raise ValueError("设备响应状态无效")
    if value["state"] in {"failed", "expired", "unknown"}:
        if not isinstance(value["error_code"], str) or not value["error_code"]:
            raise ValueError("设备失败状态缺少原因")
    elif value["error_code"] is not None:
        raise ValueError("设备成功／进行状态包含错误")
    if value["result"] is not None and not isinstance(value["result"], dict):
        raise ValueError("设备结果类型无效")
    return value


class FrpClient:
    def __init__(self, endpoint, device_id, key, *, clock=time.monotonic):
        canonical_id(device_id)
        if not isinstance(key, bytes) or len(key) != 32 or not any(key):
            raise ValueError("管理 HMAC key 必须是非零 32 字节")
        if (not isinstance(endpoint, str) or not 1 <= len(endpoint) <= 1024 or
                any(ord(character) <= 32 or ord(character) >= 127 or character == "\\"
                    for character in endpoint)):
            raise ValueError("endpoint 必须是规范 ASCII 地址，不能带空白或控制字符")
        address = urlsplit(endpoint)
        path_prefix = "" if address.path in {"", "/"} else address.path
        if (address.scheme not in {"http", "https"} or not address.hostname or
                address.username is not None or address.password is not None or
                "?" in endpoint or "#" in endpoint or
                (path_prefix and not re.fullmatch(r"(?:/[a-z0-9]+(?:-[a-z0-9]+)*)+", path_prefix))):
            raise ValueError("endpoint 必须是明确的 HTTP(S) 地址及规范受控路径前缀，不能带凭据或路径别名")
        self.address = address
        self.path_prefix = path_prefix
        self.device_id = device_id
        self.key = key
        self.clock = clock

    def _connection_remaining(self, deadline):
        left = deadline - self.clock()
        if left <= 0:
            raise TimeoutError("连接期限已尽")
        return left

    def _resolve_addresses(self, port, deadline):
        self._connection_remaining(deadline)
        try:
            # Numeric addresses never enter a resolver or spawn a process.
            addresses = socket.getaddrinfo(self.address.hostname, port, 0,
                socket.SOCK_STREAM, 0, socket.AI_NUMERICHOST)
        except socket.gaierror:
            # getaddrinfo has no cancellable timeout. A short-lived process is
            # killed and reaped on expiry; no DNS thread can outlive this call.
            process = subprocess.Popen([sys.executable, "-I", "-c", DNS_LOOKUP,
                self.address.hostname, str(port)], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                output, _ = process.communicate(timeout=self._connection_remaining(deadline))
            except BaseException as error:
                process.kill()
                process.wait()
                if process.stdout is not None:
                    process.stdout.close()
                if isinstance(error, subprocess.TimeoutExpired):
                    raise TimeoutError("DNS 解析超过连接期限") from error
                raise
            if process.returncode:
                raise socket.gaierror("设备端点 DNS 解析失败")
            addresses = json.loads(output)
        self._connection_remaining(deadline)
        return addresses

    def _open(self, deadline):
        deadline = min(deadline, self.clock() + 5)
        port = self.address.port or (443 if self.address.scheme == "https" else 80)
        addresses = self._resolve_addresses(port, deadline)
        error = OSError("设备端点没有可连接地址")
        for family, kind, protocol, _, address in addresses:
            timeout = self._connection_remaining(deadline)
            connection = socket.socket(family, kind, protocol)
            try:
                connection.settimeout(timeout)
                connection.connect(tuple(address))
                self._connection_remaining(deadline)
                if self.address.scheme == "https":
                    context = ssl.create_default_context()
                    connection.settimeout(self._connection_remaining(deadline))
                    connection = context.wrap_socket(connection,
                        server_hostname=self.address.hostname)
                    self._connection_remaining(deadline)
                return connection
            except OSError as failure:
                error = failure
                connection.close()
            except BaseException:
                connection.close()
                raise
        self._connection_remaining(deadline)
        raise error

    def _send(self, connection, payload, deadline, idle_deadline=None):
        sent = 0
        view = memoryview(payload)
        while sent < len(view):
            now = self.clock()
            left = min(deadline, idle_deadline if idle_deadline is not None else deadline) - now
            if left <= 0:
                raise TimeoutError("请求传输期限已尽")
            io_budget = min(1, left)
            connection.settimeout(io_budget)
            try:
                count = connection.send(view[sent:])
            except socket.timeout:
                continue
            if count <= 0:
                raise OSError("请求流已断开")
            finished = self.clock()
            if (finished - now >= io_budget or finished >= deadline or
                    (idle_deadline is not None and finished >= idle_deadline)):
                raise TimeoutError("迟到的发送进度不能延长原期限")
            sent += count
            if idle_deadline is not None:
                idle_deadline = finished + 30
        return idle_deadline

    def _response(self, connection, deadline):
        header = bytearray()
        while b"\r\n\r\n" not in header:
            left = deadline - self.clock()
            if left <= 0:
                raise TimeoutError("设备响应超时")
            connection.settimeout(min(1, left))
            try:
                byte = connection.recv(1)
            except socket.timeout:
                continue
            if not byte:
                raise OSError("设备响应在头部结束前断开")
            header.extend(byte)
            if len(header) > 1024:
                raise ValueError("设备响应头超过上界")
        lines = bytes(header[:-4]).decode("ascii").split("\r\n")
        if not re.fullmatch(r"HTTP/1\.1 (200|202|400|401|409|500) [\x20-\x7e]+", lines[0]):
            raise ValueError("设备 HTTP 状态无效")
        fields = {}
        for line in lines[1:]:
            name, separator, value = line.partition(": ")
            name = name.lower()
            if not separator or not re.fullmatch("[a-z0-9-]+", name) or name in fields:
                raise ValueError("设备响应头重复或无效")
            fields[name] = value
        if "transfer-encoding" in fields or fields.get("content-type") != "application/json":
            raise ValueError("设备响应 framing 无效")
        if not re.fullmatch(r"[1-9][0-9]{0,4}", fields.get("content-length", "")):
            raise ValueError("设备响应没有精确正文长度")
        length = int(fields["content-length"])
        if length > MAX_RESPONSE_BYTES:
            raise ValueError("设备响应正文超过上界")
        tag = fields.get("x-esp-management-tag", "")
        if not re.fullmatch("[0-9a-f]{64}", tag):
            raise ValueError("设备响应没有合法 HMAC")
        body = bytearray()
        while len(body) < length:
            left = deadline - self.clock()
            if left <= 0:
                raise TimeoutError("设备响应正文超时")
            connection.settimeout(min(1, left))
            try:
                block = connection.recv(length - len(body))
            except socket.timeout:
                continue
            if not block:
                raise OSError("设备响应正文截断")
            body.extend(block)
        if self.clock() >= deadline:
            raise TimeoutError("设备响应超过原期限")
        if not hmac.compare_digest(tag, hmac.digest(self.key, body, "sha256").hex()):
            raise ValueError("设备响应 HMAC 不符")
        return json.loads(body.decode("utf-8"), object_pairs_hook=unique_object,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError("非有限 JSON 数字")))

    def _headers(self, method, path, content_type, size, tag):
        host = self.address.netloc
        return (f"{method} {self.path_prefix}{path} HTTP/1.1\r\nHost: {host}\r\n"
                f"Content-Type: {content_type}\r\nContent-Length: {size}\r\n"
                f"X-ESP-Management-Tag: {tag}\r\nConnection: close\r\n\r\n").encode("ascii")

    def command(self, command, *, parameters=None, current=None, request_id=None, deadline=None):
        request_id = canonical_id(request_id) if request_id else str(uuid.uuid4())
        if command not in COMMAND_PATHS:
            raise ValueError("不支持此 FRP 命令")
        request = {"protocol_version": 1, "device_id": self.device_id,
            "request_id": request_id, "command": command}
        if parameters is not None:
            request["parameters"] = parameters
        boot_id = None
        if command in {"ota.start", "business.pause", "business.resume"}:
            if (not isinstance(current, dict) or current.get("device_id") != self.device_id or
                    not isinstance(current.get("result"), dict)):
                raise ValueError("写命令缺少当前设备的已认证启动状态")
            boot_id = canonical_id(current["boot_id"])
            uptime = current["result"].get("uptime_ms")
            if type(uptime) is not int or not 0 <= uptime <= 9007199254730991:
                raise ValueError("当前 uptime 无效")
            request.update(target_boot_id=boot_id, expires_at_uptime_ms=uptime + 10000)
        payload = wire_json(request)
        limit = 384 if command == "status" else 1024
        if len(payload) > limit:
            raise ValueError("命令超过设备正文上界")
        deadline = min(deadline, self.clock() + 5) if deadline is not None else self.clock() + 5
        self._connection_remaining(deadline)
        with self._open(deadline) as connection:
            # The device's loopback parser owns its separate 2-second budget.
            # Public DNS/TCP/TLS, request and response share the original deadline.
            headers = self._headers("POST", "/api/v1/commands/" + COMMAND_PATHS[command],
                "application/json", len(payload), hmac.digest(self.key, payload, "sha256").hex())
            self._send(connection, headers + payload, deadline)
            value = self._response(connection, deadline)
        return validate_envelope(value, self.device_id, request_id, boot_id)

    def status(self):
        value = self.command("status")
        if value["state"] != "succeeded" or not isinstance(value["result"], dict):
            raise ValueError("设备状态查询失败")
        uptime = value["result"].get("uptime_ms")
        if type(uptime) is not int or not 0 <= uptime <= 9007199254740991:
            raise ValueError("设备状态 uptime 无效")
        return value

    def result(self, operation_id, *, expected_sha256=None, expected_size=None, expected_target=None, deadline=None):
        canonical_id(operation_id)
        value = self.command("ota.result", parameters={"operation_id": operation_id}, deadline=deadline)
        result = value["result"]
        if result is None:
            if value["state"] != "unknown":
                raise ValueError("OTA 查询缺少原操作持久身份")
            return value
        if set(result) != {"operation_id", "sha256", "image_size_bytes", "target", "target_slot"}:
            raise ValueError("OTA 持久结果字段无效")
        if (result["operation_id"] != operation_id or not isinstance(result["target"], str) or
                result["target"] not in OTA_TARGETS or
                not isinstance(result["sha256"], str) or not re.fullmatch("[0-9a-f]{64}", result["sha256"]) or
                type(result["image_size_bytes"]) is not int or
                not 1 <= result["image_size_bytes"] <= OTA_TARGETS[result["target"]][1] or
                result["target_slot"] not in {"ota_0", "ota_1"} or
                (expected_sha256 is not None and result["sha256"] != expected_sha256) or
                (expected_size is not None and result["image_size_bytes"] != expected_size) or
                (expected_target is not None and result["target"] != expected_target)):
            raise ValueError("OTA 持久结果与原操作身份不符")
        return value

    def firmware_status(self, *, deadline=None):
        value = self.command("firmware.status", deadline=deadline)
        result = value["result"]
        if value["state"] != "succeeded" or not isinstance(result, dict) or set(result) != {
                "firmware_sha256", "image_size_bytes", "target", "ota_slot"}:
            raise ValueError("当前运行固件缺少完整身份")
        target = result["target"]
        if (not isinstance(target, str) or target not in OTA_TARGETS or
                not isinstance(result["firmware_sha256"], str) or
                not re.fullmatch("[0-9a-f]{64}", result["firmware_sha256"]) or
                not any(bytes.fromhex(result["firmware_sha256"])) or
                type(result["image_size_bytes"]) is not int or
                not 1 <= result["image_size_bytes"] <= OTA_TARGETS[target][1] or
                result["ota_slot"] not in {"ota_0", "ota_1"}):
            raise ValueError("当前运行固件身份无效")
        return value

    def ota_start(self, operation_id, image_file, target, *, result_timeout_seconds=120,
                  poll_interval_seconds=1, progress=None):
        canonical_id(operation_id)
        if (not isinstance(target, str) or target not in OTA_TARGETS or
                not 0 < result_timeout_seconds <= 3600 or not 0 < poll_interval_seconds <= 1):
            raise ValueError("固件目标或结果查询期限无效")
        scheme, slot_size = OTA_TARGETS[target]
        descriptor = os.open(image_file, os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0))
        write_attempted = False
        try:
            before = os.fstat(descriptor)
            if not stat.S_ISREG(before.st_mode) or not 288 <= before.st_size <= slot_size:
                raise ValueError("完整签名固件必须是目标槽内的普通文件")
            digest = hashlib.sha256()
            while block := os.read(descriptor, 65536):
                digest.update(block)
            after = os.fstat(descriptor)
            identity = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
            if identity(before) != identity(after):
                raise ValueError("固件在读取身份期间发生变化，未发送写命令")
            sha256 = digest.hexdigest()
            current = self.status()
            capabilities = current["result"].get("capabilities")
            if not isinstance(capabilities, dict) or capabilities.get("ota") != "ready":
                raise ValueError("设备 OTA 当前不可用")
            request_id = str(uuid.uuid4())
            if identity(before) != identity(os.fstat(descriptor)):
                raise ValueError("固件在提交前发生变化，未发送写命令")
            write_attempted = True
            submitted = self.command("ota.start", current=current, request_id=request_id,
                parameters={"operation_id": operation_id, "sha256": sha256,
                    "image_size_bytes": before.st_size, "target": target,
                    "signature": {"scheme": scheme}})
            if submitted["state"] in {"failed", "expired", "unknown"}:
                return submitted
            if submitted["state"] != "running" or submitted["result"] is not None:
                raise ValueError("升级提交没有返回待接收状态")
            # One upload connection only. A lost response is reconciled by
            # read-only queries, never by repeating the write or opening a new op.
            transfer_deadline = self.clock() + 300
            try:
                with self._open(min(transfer_deadline, self.clock() + 5)) as connection:
                    metadata = upload_metadata(operation_id, self.device_id, current["boot_id"],
                        before.st_size, sha256)
                    headers = self._headers("PUT", "/api/v1/ota-images/" + operation_id,
                        "application/octet-stream", before.st_size,
                        hmac.digest(self.key, metadata, "sha256").hex())
                    idle_deadline = self._send(connection, headers, transfer_deadline, self.clock() + 30)
                    os.lseek(descriptor, 0, os.SEEK_SET)
                    uploaded = 0
                    while uploaded < before.st_size:
                        block = os.read(descriptor, min(UPLOAD_CHUNK_BYTES, before.st_size - uploaded))
                        if not block:
                            raise ValueError("上传期间固件截断")
                        idle_deadline = self._send(connection, block, transfer_deadline, idle_deadline)
                        uploaded += len(block)
                        if progress:
                            progress(uploaded, before.st_size)
                    if os.read(descriptor, 1) or identity(before) != identity(os.fstat(descriptor)):
                        raise ValueError("上传期间固件身份改变")
                    # Flash readback/signature/selector may outlive an individual
                    # read; this separate response wait does not extend transfer.
                    prepared = validate_envelope(self._response(connection, self.clock() + 30),
                        self.device_id, request_id, current["boot_id"])
                    if (prepared["result"] is not None or
                            prepared["state"] not in {"running", "unknown"} or
                            (prepared["state"] == "unknown" and
                                prepared["error_code"] != "storage_uncertain")):
                        raise ValueError("上传响应不是有效的准备阶段状态")
            except (OSError, ValueError, TimeoutError):
                # Reboot or a broken transport can lose an already committed
                # result. PUT can only report a nonterminal prepare observation;
                # even a claimed terminal result must be checked by original ID.
                pass
            result_deadline = self.clock() + result_timeout_seconds
            while self.clock() < result_deadline:
                try:
                    final = self.result(operation_id, expected_sha256=sha256,
                        expected_size=before.st_size, expected_target=target, deadline=result_deadline)
                    if final["state"] == "succeeded":
                        if final["boot_id"] == current["boot_id"]:
                            raise ValueError("成功收据未对应新启动")
                        running = self.firmware_status(deadline=result_deadline)
                        identity = running["result"]
                        if (running["boot_id"] != final["boot_id"] or
                                identity["firmware_sha256"] != sha256 or
                                identity["image_size_bytes"] != before.st_size or
                                identity["target"] != target or
                                identity["ota_slot"] != final["result"]["target_slot"]):
                            raise ValueError("成功收据与当前运行固件不符")
                        self._connection_remaining(result_deadline)
                        return final
                    if final["state"] in {"failed", "expired", "unknown"}:
                        self._connection_remaining(result_deadline)
                        return final
                except OSError:
                    pass
                time.sleep(min(poll_interval_seconds, max(0, result_deadline - self.clock())))
            raise UnknownOperation(operation_id, "原操作结果查询期限已尽；只查原 ID，不重发升级")
        except UnknownOperation:
            raise
        except (OSError, ValueError, TimeoutError) as error:
            if write_attempted:
                raise UnknownOperation(operation_id, "升级状态未能核实；只查原 ID，不重发升级") from error
            raise
        finally:
            os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--management-key-env", default="ESP_BASE_FRP_MANAGEMENT_KEY_HEX")
    parser.add_argument("--json", action="store_true")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("status", "firmware-status", "business-status", "business-pause", "business-resume"):
        subparsers.add_parser(command)
    query = subparsers.add_parser("ota-result")
    query.add_argument("--operation-id", required=True)
    query.add_argument("--sha256")
    upload = subparsers.add_parser("ota-start")
    upload.add_argument("--operation-id", required=True)
    upload.add_argument("--image-file", type=Path, required=True)
    upload.add_argument("--target", choices=OTA_TARGETS, required=True)
    upload.add_argument("--result-timeout-seconds", type=float, default=120)
    args = parser.parse_args()
    try:
        text = os.environ.get(args.management_key_env, "")
        if not re.fullmatch("[0-9a-f]{64}", text):
            raise ValueError("管理 key 环境变量缺失或无效；未发送请求")
        client = FrpClient(args.endpoint, args.device_id, bytes.fromhex(text))
        if args.command == "ota-start":
            value = client.ota_start(args.operation_id, args.image_file, args.target,
                result_timeout_seconds=args.result_timeout_seconds)
        elif args.command == "ota-result":
            value = client.result(args.operation_id, expected_sha256=args.sha256)
        elif args.command == "status":
            value = client.status()
        elif args.command in {"business-pause", "business-resume"}:
            value = client.command(args.command.replace("-", "."), parameters={}, current=client.status())
        else:
            value = client.firmware_status() if args.command == "firmware-status" else client.command(args.command.replace("-", "."))
        output = {"device_id": args.device_id, "operation_id": getattr(args, "operation_id", None),
            "response": value}
        exit_code = 0 if value["state"] == "succeeded" else 2 if value["state"] == "unknown" else 1
    except UnknownOperation as error:
        output = {"device_id": args.device_id, "operation_id": error.operation_id,
            "state": "unknown", "error": str(error)}
        exit_code = 2
    except (OSError, ValueError, TimeoutError) as error:
        output = {"device_id": args.device_id, "state": "failed", "error": str(error)}
        exit_code = 1
    if args.json:
        print(json.dumps(output, separators=(",", ":"), ensure_ascii=False))
    else:
        value = output.get("response", output)
        print("FRP 固件／控制结果")
        print(f"  设备：{args.device_id}")
        if output.get("operation_id"):
            print(f"  操作：{output['operation_id']}")
        print(f"  状态：{value['state']}")
        if output.get("error") or value.get("error_code"):
            print(f"  原因：{output.get('error') or value['error_code']}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
