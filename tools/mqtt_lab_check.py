#!/usr/bin/env python3
"""宿主侧 MQTT 实验应用检查；只连接显式指定的隔离 Broker，明文须单独启用。"""
import argparse
import importlib.util
import json
from pathlib import Path
import queue
import secrets
import statistics
import sys
import threading
import time
import uuid

import paho.mqtt.client as mqtt


def load_credentials(path):
    spec = importlib.util.spec_from_file_location("device_control", Path(__file__).with_name("device-control.py"))
    control = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(control)
    value = control.load_private_config(path)
    if not isinstance(value, dict) or set(value) != {"username", "password"} or any(
        not isinstance(value[key], str) or not value[key] or "\0" in value[key] for key in value
    ):
        raise ValueError("实验账号文件必须包含非空 username/password")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--ca", help="严格 TLS 的实验 CA")
    transport.add_argument("--plaintext-lab", action="store_true", help="显式连接隔离明文实验 Broker，不用于产品")
    parser.add_argument("--credentials-file", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--cycles", type=int, default=0, help="完整销毁/重建并验证新在线与往返的次数，最大 100")
    parser.add_argument("--resource-samples", action="store_true", help="每次重建后请求串口资源观测，结果仍须读取串口证据")
    parser.add_argument("--subscriptions", action="store_true", help="验证 extra 主题的动态订阅、重连保持与退订")
    parser.add_argument("--wifi-cycles", type=int, default=0, help="本板 station 暂停五秒再恢复，最大 10 次；不等同于外部 AP 断电")
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--ready-timeout", type=float, default=120)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    identity = uuid.UUID(args.device_id)
    if identity.version != 4 or str(identity) != args.device_id or not 0 <= args.cycles <= 100 or not 1 <= args.port <= 65535:
        parser.error("设备身份、循环次数或端口不合法")
    if not 0 < args.timeout <= 120 or not 0 < args.ready_timeout <= 180:
        parser.error("等待期限必须为正数且不超过上限")
    if not 0 <= args.wifi_cycles <= 10:
        parser.error("station 重连次数必须在 0–10 之间")
    credentials = load_credentials(args.credentials_file)
    prefix = "esp-base-lab/" + args.device_id
    messages = queue.Queue(maxsize=2048)
    overflow = threading.Event()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id="lab-controller-" + secrets.token_hex(8), protocol=mqtt.MQTTv311)
    client.username_pw_set(credentials["username"], credentials["password"])
    if not args.plaintext_lab:
        client.tls_set(ca_certs=args.ca)

    def post(value):
        try:
            messages.put_nowait(value)
        except queue.Full:
            overflow.set()

    client.on_connect = lambda c, u, f, code, p: post(("connect", not code.is_failure))
    client.on_subscribe = lambda c, u, mid, codes, p: post(("subscribe", mid, [v.value for v in codes]))
    client.on_message = lambda c, u, msg: post(("message", msg.topic, bytes(msg.payload), msg.qos, msg.retain))

    def wait(predicate, timeout=None):
        deadline = time.monotonic() + (args.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            if overflow.is_set():
                raise RuntimeError("宿主观察队列溢出，无法确认设备结果")
            try:
                value = messages.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty:
                break
            if value[0] == "connect" and not value[1]:
                raise RuntimeError("Broker 拒绝实验账号连接")
            if predicate(value):
                return value
        raise TimeoutError("没有取得设备网络结果；不自动重发实验控制字")

    def publish(payload, qos=0, topic="in"):
        info = client.publish(prefix + "/" + topic, payload, qos=qos, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError("宿主发送失败")
        info.wait_for_publish(timeout=args.timeout)
        if not info.is_published():
            raise TimeoutError("宿主发送未确认；不自动重发控制字")

    def echo(payload, qos, topic="in"):
        started = time.monotonic()
        publish(payload, qos, topic)
        wait(lambda v: v[0] == "message" and v[1] == prefix + "/out" and
             v[2] == payload and v[3] == qos and not v[4])
        return round((time.monotonic() - started) * 1000, 3)

    def ready(timeout=None):
        wait(lambda v: v[0] == "message" and v[1] == prefix + "/status" and v[2] == b"online", timeout)
        # retained online 不足以证明当前在线；必须完成新随机字节的往返。
        echo(b"ready-proof-" + secrets.token_bytes(32), 1)

    def live_ready():
        wait(lambda v: v[0] == "message" and v[1] == prefix + "/status" and v[2] == b"online" and not v[4], args.ready_timeout)
        echo(b"fresh-proof-" + secrets.token_bytes(32), 1)

    def no_extra_echo():
        payload = b"unsubscribed-proof-" + secrets.token_bytes(32)
        publish(payload, 1, "extra")
        # 使用正向 in 往返证明设备仍在处理消息；负例仍保留独立观察窗口。
        sentinel = b"still-live-" + secrets.token_bytes(32)
        publish(sentinel, 1)
        live = False
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                value = messages.get(timeout=min(0.1, max(0.001, deadline - time.monotonic())))
            except queue.Empty:
                continue
            if overflow.is_set():
                raise RuntimeError("宿主观察队列溢出，无法验证退订")
            if value[0] == "message" and value[1] == prefix + "/out":
                if value[2] == payload:
                    raise RuntimeError("未订阅或已退订的 extra 主题仍交付消息")
                if value[2] == sentinel:
                    live = True
        if not live:
            raise TimeoutError("退订负例缺少设备仍在线的正向证明")

    def resource_sample():
        publish(b":stats", 0)
        echo(b"after-stats-" + secrets.token_bytes(32), 1)

    cases = []
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()
    try:
        wait(lambda v: v[0] == "connect" and v[1])
        result, mid = client.subscribe([(prefix + "/out", 1), (prefix + "/status", 1)])
        if result != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError("宿主订阅提交失败")
        subscribed = wait(lambda v: v[0] == "subscribe" and v[1] == mid)
        if subscribed[2] != [1, 1]:
            raise RuntimeError("Broker 未完整批准宿主订阅")
        ready(args.ready_timeout)
        if args.resource_samples:
            resource_sample()
        for qos in (0, 1):
            for size in (0, 1, 127, 1024, 4096):
                payload = secrets.token_bytes(size)
                latency = echo(payload, qos)
                cases.append({"qos": qos, "bytes": size, "roundtrip_ms": latency})
        if args.subscriptions:
            no_extra_echo()
            publish(b":subscribe", 0)
            live_ready()
            echo(b"subscribed-proof-" + secrets.token_bytes(32), 1, "extra")
            publish(b":restart", 0)
            live_ready()
            echo(b"resubscribed-proof-" + secrets.token_bytes(32), 1, "extra")
            publish(b":unsubscribe", 0)
            live_ready()
            no_extra_echo()
            publish(b":restart", 0)
            live_ready()
            no_extra_echo()
        for index in range(args.wifi_cycles):
            publish(b":wifi-cycle", 0)
            live_ready()
            print("MQTT 实板检查：已确认 %d 次 station 恢复后的消息往返" % (index + 1), file=sys.stderr, flush=True)
        for index in range(args.cycles):
            publish(b":cycle", 0)
            # 当前订阅保持连接，新一轮 online 必须是实时发布而非 retained 回放。
            wait(lambda v: v[0] == "message" and v[1] == prefix + "/status" and v[2] == b"online" and not v[4])
            echo(b"cycle-proof-" + secrets.token_bytes(32), 1)
            if args.resource_samples:
                resource_sample()
            if (index + 1) % 10 == 0:
                print("MQTT 实板检查：已确认 %d 次重建后的消息往返" % (index + 1), file=sys.stderr, flush=True)
        latencies = sorted(v["roundtrip_ms"] for v in cases)
        result = {"scope": "mqtt-lab-network-roundtrip", "transport": "tcp-lab" if args.plaintext_lab else "tls",
                  "cases": cases, "cycles_with_live_echo": args.cycles,
                  "roundtrip_p50_ms": statistics.median(latencies), "roundtrip_p95_ms": latencies[-1],
                  "dynamic_subscriptions": args.subscriptions, "station_cycles_with_live_echo": args.wifi_cycles,
                  "requested_serial_resource_samples": args.cycles + 1 if args.resource_samples else 0,
                  "resource_acceptance": "requires separate device telemetry"}
        if args.json:
            print(json.dumps(result, ensure_ascii=False))
        else:
            print("MQTT 实板检查\n  状态  通过\n  往返  %d 项\n  重建  %d 次\n  资源  需另外核对实板遥测" % (len(cases), args.cycles))
    finally:
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("MQTT 实板检查失败\n  原因  " + str(error), file=sys.stderr)
        sys.exit(1)
