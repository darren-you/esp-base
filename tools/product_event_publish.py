#!/usr/bin/env python3
"""经严格 TLS 发布一帧已签名产品事件，并核对设备 reported。"""

import argparse
import hashlib
import hmac
import json
import os
import pathlib
import queue
import secrets
import stat
import sys
import time

import product_event

FRAME_HEADER_BYTES = 65 + len(product_event.DOMAIN) + 36 + 36 + 32 + 8


def private_file(path: pathlib.Path, limit: int) -> bytes:
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        facts = os.fstat(descriptor)
        if (not stat.S_ISREG(facts.st_mode) or facts.st_uid != os.getuid() or
                stat.S_IMODE(facts.st_mode) != 0o600 or facts.st_nlink != 1):
            raise ValueError("输入文件须由当前用户独占、权限精确为 0600，且不是链接")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            raw = source.read(limit + 1)
    finally:
        os.close(descriptor)
    if len(raw) > limit:
        raise ValueError("输入文件超过上限")
    return raw


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("JSON 存在重复字段")
        value[key] = item
    return value


def credentials(path: pathlib.Path) -> dict:
    value = json.loads(private_file(path, 8192).decode("utf-8"),
                       object_pairs_hook=unique_object)
    if (not isinstance(value, dict) or set(value) != {"username", "password"} or
            any(not isinstance(value[key], str) or not value[key] or
                any(ord(char) < 32 for char in value[key]) for key in value)):
        raise ValueError("控制账户文件须只含非空 username/password")
    return value


def verified_frame(path: pathlib.Path, key_file: pathlib.Path, device_id: str,
                   boot_id: str, package_sha256: str, sequence: int) -> bytes:
    frame = private_file(path, product_event.MAX_FRAME_BYTES)
    if len(frame) <= FRAME_HEADER_BYTES:
        raise ValueError("业务事件帧不完整")
    key = product_event.private_key(key_file)
    expected = product_event.event_frame(key, device_id, boot_id,
                                         package_sha256, sequence, frame[FRAME_HEADER_BYTES:])
    if not hmac.compare_digest(frame, expected):
        raise ValueError("业务事件帧与本轮设备、启动、包、序号或管理密钥不匹配")
    return frame


def reported_message(payload: bytes, device_id: str, boot_id: str) -> dict:
    if len(payload) > 1024:
        raise ValueError("设备 reported 超出上限")
    value = json.loads(payload.decode("utf-8"), object_pairs_hook=unique_object)
    required = {"protocol_version", "device_id", "boot_id", "uptime_ms", "revision",
                "wifi_state", "time_ready", "frp_state",
                "last_accepted_event_sequence", "last_completed_event_sequence",
                "last_completed_package_sha256", "last_completed_event_sha256",
                "last_event_outcome", "last_guest_result"}
    if not isinstance(value, dict) or set(value) != required:
        raise ValueError("设备 reported 字段不符合当前协议")
    if (type(value["protocol_version"]) is not int or value["protocol_version"] != 1 or
            value["device_id"] != device_id or value["boot_id"] != boot_id):
        raise ValueError("设备身份或本次启动已变化")
    for name in ("uptime_ms", "revision", "last_accepted_event_sequence"):
        if type(value[name]) is not int or value[name] < 0:
            raise ValueError("设备 reported 数值无效")
    if value["last_accepted_event_sequence"] > product_event.MAX_SEQUENCE:
        raise ValueError("设备接收序号越界")
    completed = value["last_completed_event_sequence"]
    if completed is not None and (type(completed) is not int or
                                  not 1 <= completed <= value["last_accepted_event_sequence"]):
        raise ValueError("设备完成序号无效")
    outcome = value["last_event_outcome"]
    if (not isinstance(outcome, str) or
            outcome not in {"none", "busy", "succeeded", "business_failed", "runtime_failed"}):
        raise ValueError("设备业务事件结果无效")
    package = value["last_completed_package_sha256"]
    event_sha256 = value["last_completed_event_sha256"]
    guest_result = value["last_guest_result"]
    if completed is None:
        if (package is not None or event_sha256 is not None or
                guest_result is not None or outcome not in {"none", "busy"}):
            raise ValueError("设备尚无完成事件却提供了结果")
    elif (not isinstance(package, str) or len(package) != 64 or
          any(char not in "0123456789abcdef" for char in package) or
          not isinstance(event_sha256, str) or len(event_sha256) != 64 or
          any(char not in "0123456789abcdef" for char in event_sha256) or
          (guest_result is not None and type(guest_result) is not int) or
          outcome in {"none", "busy"} or
          (outcome == "runtime_failed" and guest_result is not None) or
          (outcome == "succeeded" and (guest_result is None or guest_result < 0)) or
          (outcome == "business_failed" and (guest_result is None or guest_result >= 0))):
        raise ValueError("设备完成事件的摘要或业务结果无效")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--ca-file", required=True, type=pathlib.Path)
    parser.add_argument("--credentials-file", required=True, type=pathlib.Path)
    parser.add_argument("--management-key-file", required=True, type=pathlib.Path)
    parser.add_argument("--frame-file", required=True, type=pathlib.Path)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--boot-id", required=True)
    parser.add_argument("--package-sha256", required=True)
    parser.add_argument("--event-sequence", required=True, type=int)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    args = parser.parse_args()
    if (not args.host or any(ord(char) < 33 for char in args.host) or
            not 1 <= args.port <= 65535 or not 5 <= args.timeout_seconds <= 120):
        parser.error("Broker 地址、端口或等待期限无效")
    try:
        frame = verified_frame(args.frame_file, args.management_key_file,
                               args.device_id, args.boot_id,
                               args.package_sha256, args.event_sequence)
        event_sha256 = hashlib.sha256(frame[FRAME_HEADER_BYTES:]).hexdigest()
        account = credentials(args.credentials_file)
        import paho.mqtt.client as mqtt
    except (OSError, ValueError, UnicodeError, ImportError) as error:
        parser.error(str(error))

    topic = f"esp-base/{args.device_id}"
    events = queue.Queue(maxsize=32)
    overflow = False

    def post(item):
        nonlocal overflow
        try:
            events.put_nowait(item)
        except queue.Full:
            overflow = True

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id="base-event-" + secrets.token_hex(8),
                             protocol=mqtt.MQTTv311,
                             reconnect_on_failure=False)
        client.username_pw_set(account["username"], account["password"])
        client.tls_set(ca_certs=str(args.ca_file))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    client.on_connect = lambda c, u, flags, code, properties: post(("connect", not code.is_failure))
    client.on_subscribe = lambda c, u, mid, codes, properties: post(
        ("subscribe", mid, [code.value for code in codes]))
    client.on_disconnect = lambda c, u, flags, code, properties: post(("disconnect",))
    client.on_message = lambda c, u, message: post(
        ("message", message.topic, bytes(message.payload), message.qos, message.retain))

    def wait_reported(deadline):
        while time.monotonic() < deadline:
            if overflow:
                raise RuntimeError("观察队列溢出，事件状态 unknown")
            try:
                item = events.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty:
                break
            if item[0] == "connect" and not item[1]:
                raise RuntimeError("Broker 拒绝控制账户")
            if item[0] == "disconnect":
                raise RuntimeError("Broker 会话中断，事件状态 unknown")
            if item[0] == "message" and item[1] == topic + "/reported" and item[3] == 1 and not item[4]:
                try:
                    return reported_message(item[2], args.device_id, args.boot_id)
                except (ValueError, UnicodeError):
                    raise RuntimeError("设备 reported 无法核对，事件状态 unknown") from None
        raise TimeoutError("没有收到本次启动的非 retained reported，事件状态 unknown")

    published = False
    started = False
    try:
        client.connect(args.host, args.port, keepalive=30)
        client.loop_start()
        started = True
        deadline = time.monotonic() + args.timeout_seconds
        while True:
            item = events.get(timeout=max(0.001, deadline - time.monotonic()))
            if item[0] == "connect":
                if not item[1]:
                    raise RuntimeError("Broker 拒绝控制账户")
                break
            if item[0] == "disconnect" or overflow:
                raise RuntimeError("Broker 连接未就绪")
        code, mid = client.subscribe(topic + "/reported", qos=1)
        if code != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError("控制账户订阅提交失败")
        while True:
            item = events.get(timeout=max(0.001, deadline - time.monotonic()))
            if item[0] == "subscribe" and item[1] == mid:
                if item[2] != [1]:
                    raise RuntimeError("Broker 未批准精确 reported 订阅")
                break
            if item[0] == "disconnect" or overflow:
                raise RuntimeError("订阅时 Broker 会话中断")
        before = wait_reported(deadline)
        if before["last_accepted_event_sequence"] != args.event_sequence - 1:
            raise ValueError("设备高水位与指定下一序号不一致；未发布")
        info = client.publish(topic + "/event", frame, qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError("事件发布提交失败；状态 unknown")
        published = True
        info.wait_for_publish(timeout=max(0.001, deadline - time.monotonic()))
        if not info.is_published():
            raise TimeoutError("事件 PUBACK 未确认；状态 unknown，不自动重发")
        while True:
            current = wait_reported(deadline)
            accepted = current["last_accepted_event_sequence"]
            completed = current["last_completed_event_sequence"]
            if accepted > args.event_sequence or (completed is not None and completed > args.event_sequence):
                raise RuntimeError("设备事件序号已越过本帧；状态 unknown")
            if accepted != args.event_sequence or completed != args.event_sequence:
                continue
            if (current["last_completed_package_sha256"] != args.package_sha256 or
                    current["last_completed_event_sha256"] != event_sha256 or
                    current["last_event_outcome"] not in {"succeeded", "business_failed", "runtime_failed"}):
                raise RuntimeError("设备完成结果与本帧不一致；状态 unknown")
            result = {"device_id": args.device_id, "boot_id": args.boot_id,
                      "package_sha256": args.package_sha256,
                      "event_sha256": event_sha256,
                      "event_sequence": args.event_sequence,
                      "event_outcome": current["last_event_outcome"],
                      "guest_result": current["last_guest_result"]}
            print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
            return 0 if current["last_event_outcome"] == "succeeded" else 2
    except (OSError, ValueError, RuntimeError, TimeoutError, queue.Empty) as error:
        print(f"事件{'已发布，' if published else '未发布，'}{error}", file=sys.stderr)
        return 2 if published else 1
    finally:
        if started:
            client.disconnect()
            client.loop_stop()


if __name__ == "__main__":
    sys.exit(main())
