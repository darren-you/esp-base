#!/usr/bin/env python3
"""有限 R6 宿主驱动：单次 100 周期或连续 72 小时，不授予实板资格。

运行前仍须按既有流程核对实体 UUID/芯片/4 MiB/安全状态、本轮恢复基线、
唯一设备租约、同一已评审实现的冻结 signed A/C pair（均由 R5 覆盖）与正式
MQTT/FRP 信任。两镜像摘要必须不同；实际运行身份不在 pair 中时禁止升级。
R5 原件只绑定调用者提供的摘要；本工具不解析原件授予 R5 通过，
不管理备份、租约或凭据。72h 表达本轮连续宿主执行观察；普通步骤的
进度空档上限为 10s，休眠每 1s 检查，MQTT 等待每 0.25s 检查。
另以墙钟对照单调时钟，增量差超过 1s、任一时钟回退均中断，防止
macOS 系统休眠不推进 monotonic 而遗漏空档。OTA 有界等待另行记录，
宿主观察不证明设备在每一时刻的实际状态。
观察结束时刻使用最后一次宿主核验的采样；此后元数据写入及其间的
时钟变化不计入观察窗，也不宣称已被检测。
周期仅观察 OTA 重启后的网络重建、最大原生业务、暂停/恢复及宿主会话关闭；
全任务释放、容量/Flash/公网/实板资格仍由外部原始观察与真实验收裁决。
依赖／key／CA 的预检在日志创建前；只有成功创建本轮独占日志后的
中断写入 interrupted 摘要。保留的 OTA ID 不表示已经发送或获准。
"""

import argparse
import hashlib
import json
import os
import queue
import re
import secrets
import stat
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import business_event
import business_event_publish as publisher
import frp_ota


CYCLE_COUNT = 100
SOAK_SECONDS = 72 * 60 * 60
MAX_EVENT_BYTES = business_event.MAX_FRAME_BYTES - publisher.FRAME_HEADER_BYTES
MAX_EVENT = b"\x01" + bytes(index % 256 for index in range(MAX_EVENT_BYTES - 1))
EVENT_INCREMENT = len(MAX_EVENT) - 1
EVENT_TIMEOUT_SECONDS = 30
HOST_PROGRESS_GAP_SECONDS = 10
CLOCK_DELTA_TOLERANCE_SECONDS = 1


class RunInterrupted(RuntimeError):
    pass


def checked_sha256(value):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{64}", value) or value == "0" * 64:
        raise ValueError("摘要须为非零的小写 SHA-256")
    return value


def file_snapshot(path, *, private=False, append_only=False, prefix_bytes=None):
    """绑定本次读取的原件字节；活动容量日志明确绑定固定长度前缀。"""
    path = Path(path).absolute()
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise ValueError("证据与镜像必须是非链接的普通文件")
        if private and (before.st_uid != os.getuid() or stat.S_IMODE(before.st_mode) != 0o600):
            raise ValueError("证据文件须由当前用户独占且权限精确为 0600")
        digest = hashlib.sha256()
        bound_size = before.st_size if prefix_bytes is None else prefix_bytes
        if bound_size > before.st_size:
            raise ValueError("容量日志的已绑定前缀被截断")
        remaining = bound_size
        while remaining:
            block = os.read(descriptor, min(65536, remaining))
            if not block:
                raise ValueError("文件在摘要读取期间截断")
            digest.update(block)
            remaining -= len(block)
        after = os.fstat(descriptor)
        path_facts = os.stat(path, follow_symlinks=False)
        same_identity = (before.st_dev, before.st_ino) == (after.st_dev, after.st_ino) == (path_facts.st_dev, path_facts.st_ino)
        if not same_identity or (append_only and after.st_size < before.st_size) or (
                not append_only and (before.st_size, before.st_mtime_ns, before.st_ctime_ns) !=
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
            raise ValueError("文件在摘要读取期间改变")
        return {"path": str(path), "size_bytes": bound_size, "sha256": digest.hexdigest(),
                "file_device": before.st_dev, "file_inode": before.st_ino,
                "scope": "prefix" if append_only else "complete_file"}
    finally:
        os.close(descriptor)


class Journal:
    def __init__(self, directory, clock=time.monotonic):
        self.directory = Path(directory).absolute()
        self.directory.mkdir(mode=0o700)
        os.chmod(self.directory, 0o700)
        descriptor = os.open(self.directory / "observations.jsonl",
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        os.fchmod(descriptor, 0o600)
        self.output = os.fdopen(descriptor, "w", encoding="utf-8")
        self.lock = threading.RLock()
        self.clock = clock
        self.run_id = str(uuid.uuid4())
        self.start = clock()

    def record(self, kind, **value):
        with self.lock:
            record = {"run_id": self.run_id, "kind": kind, "monotonic_seconds": self.clock(), **value}
            self.output.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            self.output.flush()
            os.fsync(self.output.fileno())

    def finish(self, value):
        with self.lock:
            self.record("run_finished", **value)
            descriptor = os.open(self.directory / "summary.json",
                                 os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump(value, output, ensure_ascii=False, separators=(",", ":"))
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            self.close()

    def close(self):
        with self.lock:
            self.output.close()


class BoundFrpClient(frp_ota.FrpClient):
    """现有 ota_start 内部的首次 status 同样必须绑定本轮来源 boot。"""
    expected_boot_id = None
    expected_image = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.last_ota_command_attempt_id = None
        self.last_ota_admission_confirmed_id = None
        self.observe_host = None

    def status(self):
        value = super().status()
        if self.expected_boot_id is not None and value["boot_id"] != self.expected_boot_id:
            raise RunInterrupted("OTA 提交前发生非预期重启；未继续写入")
        return value

    def command(self, command, **kwargs):
        if self.observe_host is not None:
            self.observe_host("frp_command_begin")
        if command == "ota.start":
            parameters = kwargs.get("parameters", {})
            image = self.expected_image
            if image is None or any(parameters.get(name) != image[source] for name, source in (
                    ("sha256", "sha256"), ("image_size_bytes", "size_bytes"), ("target", "target"))):
                raise RunInterrupted("ota_start 实际读取的镜像不属于冻结目标；未发送写命令")
            # This boundary precedes _open/_send. It proves a command attempt,
            # not that bytes reached the device or admission was confirmed.
            self.last_ota_command_attempt_id = parameters["operation_id"]
        value = super().command(command, **kwargs)
        if command == "ota.start" and value["state"] == "running" and value["result"] is None:
            self.last_ota_admission_confirmed_id = parameters["operation_id"]
        if self.observe_host is not None:
            self.observe_host("frp_command_end")
        return value


class BusinessSession:
    """一个有限严格 TLS 会话；不自动重连，不重发业务帧。"""
    def __init__(self, args, device_id, boot_id, key, *, clock=time.monotonic):
        import paho.mqtt.client as mqtt
        self.mqtt, self.clock = mqtt, clock
        self.device_id, self.boot_id, self.key = device_id, boot_id, key
        self.topic = f"esp-base/{device_id}"
        self.events = queue.Queue(maxsize=32)
        self.overflow = threading.Event()
        self.cancelled = threading.Event()
        self.latest = None
        self.last_attempt = None
        self.started = False
        self.observe_host = lambda stage: None
        account = publisher.credentials(args.credentials_file)
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
            client_id="base-lifecycle-" + secrets.token_hex(8), protocol=mqtt.MQTTv311,
            reconnect_on_failure=False)
        self.client.connect_timeout = 5
        self.client.username_pw_set(account["username"], account["password"])
        self.client.tls_set(ca_certs=str(args.ca_file))
        self.client.on_connect = lambda c, u, flags, code, properties: self.post(("connect", not code.is_failure))
        self.client.on_subscribe = lambda c, u, mid, codes, properties: self.post(
            ("subscribe", mid, [code.value for code in codes]))
        self.client.on_disconnect = lambda c, u, flags, code, properties: self.post(("disconnect",))
        self.client.on_message = lambda c, u, message: self.post(
            ("message", message.topic, bytes(message.payload), message.qos, message.retain))
        self.host, self.port = args.mqtt_host, args.mqtt_port

    def post(self, item):
        try:
            self.events.put_nowait(item)
        except queue.Full:
            self.overflow.set()

    def next_event(self, deadline):
        while self.clock() < deadline:
            self.observe_host("mqtt_wait")
            if self.overflow.is_set() or self.cancelled.is_set():
                raise RunInterrupted("MQTT 观察溢出或本轮取消；业务状态未知")
            try:
                item = self.events.get(timeout=min(0.25, max(0.001, deadline - self.clock())))
            except queue.Empty:
                continue
            if item[0] == "disconnect" or (item[0] == "connect" and not item[1]):
                raise RunInterrupted("MQTT 会话失联或认证拒绝；不重连或重发")
            return item
        raise RunInterrupted("MQTT reported 期限已尽；不重发业务事件")

    def reported(self, deadline):
        while True:
            item = self.next_event(deadline)
            if item[0] == "message" and item[1] == self.topic + "/reported" and item[3] == 1 and not item[4]:
                value = publisher.reported_message(item[2], self.device_id, self.boot_id)
                if value["wifi_state"] != "connected" or value["time_ready"] is not True or value["frp_state"] != "ready":
                    raise RunInterrupted("设备 reported 的 Wi-Fi／时间／FRP 未就绪")
                if self.latest is not None and value["uptime_ms"] < self.latest["uptime_ms"]:
                    raise RunInterrupted("同 boot 的设备 uptime 回退")
                self.latest = value
                return value

    def open(self):
        deadline = self.clock() + EVENT_TIMEOUT_SECONDS
        self.client.connect(self.host, self.port, keepalive=30)
        self.observe_host("mqtt_connected")
        self.client.loop_start()
        self.started = True
        while True:
            item = self.next_event(deadline)
            if item[0] == "connect":
                break
        code, mid = self.client.subscribe(self.topic + "/reported", qos=1)
        if code != self.mqtt.MQTT_ERR_SUCCESS:
            raise RunInterrupted("reported 订阅提交失败")
        while True:
            item = self.next_event(deadline)
            if item[0] == "subscribe" and item[1] == mid:
                if item[2] != [1]:
                    raise RunInterrupted("reported 订阅未精确批准 QoS 1")
                break
        self.reported(deadline)
        return self

    def publish_maximum(self, byte_count):
        if self.latest is None:
            raise RunInterrupted("业务发布缺少本 boot 的 reported")
        before = self.latest
        sequence = before["last_accepted_event_sequence"] + 1
        accepted = before["last_accepted_event_sequence"]
        if before["last_completed_event_sequence"] != (accepted if accepted else None):
            raise RunInterrupted("已有业务事件尚未完成；不覆盖原序号")
        expected = byte_count + EVENT_INCREMENT
        if expected > (1 << 31) - 1:
            raise RunInterrupted("业务计数将超过正式 int32 合同")
        frame = business_event.event_frame(self.key, self.device_id, self.boot_id, sequence, MAX_EVENT)
        sha256 = hashlib.sha256(MAX_EVENT).hexdigest()
        deadline = self.clock() + EVENT_TIMEOUT_SECONDS
        started = self.clock()
        self.last_attempt = {"device_id": self.device_id, "boot_id": self.boot_id,
            "event_sequence": sequence, "event_sha256": sha256, "event_size_bytes": len(MAX_EVENT),
            "expected_business_result": expected, "published_monotonic_seconds": started}
        info = self.client.publish(self.topic + "/event", frame, qos=1, retain=False)
        if info.rc != self.mqtt.MQTT_ERR_SUCCESS:
            raise RunInterrupted("业务帧提交未获确认；不重发或换序号")
        # PUBACK is deliberately not used as business completion evidence.
        while True:
            value = self.reported(deadline)
            accepted, completed = value["last_accepted_event_sequence"], value["last_completed_event_sequence"]
            if accepted > sequence or (completed is not None and completed > sequence):
                raise RunInterrupted("业务序号越过本帧；结果未知")
            if accepted != sequence or completed != sequence:
                continue
            if (value["last_completed_event_sha256"] != sha256 or value["last_event_outcome"] != "succeeded" or
                    value["last_business_result"] != expected):
                raise RunInterrupted("业务完成的摘要／实际返回与本帧不一致")
            return {"device_id": self.device_id, "boot_id": self.boot_id, "event_sequence": sequence,
                    "event_sha256": sha256, "event_size_bytes": len(MAX_EVENT), "frame_size_bytes": len(frame),
                    "business_result": expected, "published_monotonic_seconds": started,
                    "completed_reported_monotonic_seconds": self.clock(), "reported": value}

    def close(self):
        self.cancelled.set()
        if self.started:
            try:
                self.client.disconnect()
            finally:
                self.client.loop_stop()
                self.started = False


class LifecycleRun:
    def __init__(self, args, client, key, journal, *, session_factory=BusinessSession,
                 clock=time.monotonic, sleep=time.sleep, wall_clock=time.time):
        self.args, self.client, self.key, self.journal = args, client, key, journal
        self.session_factory, self.clock, self.sleep = session_factory, clock, sleep
        self.wall_clock = wall_clock
        self.boot_id = args.initial_boot_id
        self.cycles = 0
        self.events = 0
        self.operations = []
        self.active_operation = None
        self.host_last_progress = self.clock()
        self.clock_sample_monotonic = self.host_last_progress
        self.clock_sample_wall = self.wall_clock()
        self.maximum_clock_delta_discrepancy_seconds = 0
        self.clock_sample_lock = threading.Lock()
        self.client.observe_host = self.observe_host
        self.maximum_host_progress_gap_seconds = 0
        self.expected_ota_window_started = None
        self.maximum_expected_ota_window_seconds = 0
        self.expected_ota_window_count = 0
        self.images = []
        for name in ("a", "c"):
            image = file_snapshot(getattr(args, f"image_{name}_file"))
            target = getattr(args, f"image_{name}_target")
            if (image["sha256"] != getattr(args, f"image_{name}_sha256") or
                    image["size_bytes"] != getattr(args, f"image_{name}_size_bytes")):
                raise ValueError("完整 signed A/C 与调用者给定身份不符")
            if target not in frp_ota.OTA_TARGETS or not 288 <= image["size_bytes"] <= frp_ota.OTA_TARGETS[target][1]:
                raise ValueError("完整 signed A/C 不满足目标槽几何")
            self.images.append({**image, "target": target, "label": name})
        if self.images[0]["sha256"] == self.images[1]["sha256"] or self.images[0]["target"] != self.images[1]["target"]:
            raise ValueError("冻结 A/C 必须是同 target 的两份不同 signed bin")
        self.r5 = file_snapshot(args.r5_evidence_file, private=True)
        if self.r5["sha256"] != args.r5_evidence_sha256:
            raise ValueError("R5 原件与调用者给定摘要不符")
        self.capacity = file_snapshot(args.capacity_log_file, private=True, append_only=True)

    def observe_host(self, stage, *, record=False):
        """测量有限宿主执行的空档；OTA 等待明确单列，不授予设备连续性。"""
        # time.time crosses macOS sleep; mach_absolute_time-backed monotonic
        # does not. Compare independent deltas without extending run duration.
        with self.clock_sample_lock:
            now, wall = self.clock(), self.wall_clock()
            monotonic_delta = now - self.clock_sample_monotonic
            wall_delta = wall - self.clock_sample_wall
            discrepancy = wall_delta - monotonic_delta
            self.clock_sample_monotonic, self.clock_sample_wall = now, wall
            self.maximum_clock_delta_discrepancy_seconds = max(
                self.maximum_clock_delta_discrepancy_seconds, abs(discrepancy))
            clock_anomaly = (monotonic_delta < 0 or wall_delta < 0 or
                             abs(discrepancy) > CLOCK_DELTA_TOLERANCE_SECONDS)
            # 上传与MQTT业务线程共享同一采样顺序。不能在解锁后用较早的now
            # 覆盖另一线程已经提交的进度，否则正常调度会伪造负gap。
            gap = None
            if not clock_anomaly and self.expected_ota_window_started is None:
                gap = now - self.host_last_progress
                self.maximum_host_progress_gap_seconds = max(self.maximum_host_progress_gap_seconds, gap)
                self.host_last_progress = now
            maximum_gap = self.maximum_host_progress_gap_seconds
        # Journal有自己的锁；记录与异常均在clock锁外，避免锁顺序反转。
        if clock_anomaly:
            self.journal.record("host_clock_anomaly", stage=stage,
                monotonic_delta_seconds=monotonic_delta, wall_delta_seconds=wall_delta,
                discrepancy_seconds=discrepancy, allowed_discrepancy_seconds=CLOCK_DELTA_TOLERANCE_SECONDS)
            raise RunInterrupted("宿主时钟回退、墙钟跳变或系统休眠；本轮连续宿主观察中断")
        if gap is None:
            return now
        if record or gap < 0 or gap > HOST_PROGRESS_GAP_SECONDS:
            self.journal.record("host_progress_checked", stage=stage, gap_seconds=gap,
                allowed_gap_seconds=HOST_PROGRESS_GAP_SECONDS,
                maximum_gap_seconds=maximum_gap,
                scope="host_execution_between_bounded_operations")
        if gap < 0 or gap > HOST_PROGRESS_GAP_SECONDS:
            raise RunInterrupted("宿主观察进度空档超过固定 10s 上界；本轮不具备连续执行观察")
        return now

    def command(self, name, **kwargs):
        self.observe_host("before_command")
        value = self.client.command(name, **kwargs)
        self.observe_host("after_command")
        return value

    def response(self, value, boot_id=None):
        if value["device_id"] != self.args.device_id or value["boot_id"] != (boot_id or self.boot_id):
            raise RunInterrupted("设备或非预期 boot 身份变化")
        if value["state"] != "succeeded" or not isinstance(value["result"], dict):
            raise RunInterrupted("设备返回非成功或未知状态")
        return value["result"]

    def ready(self):
        self.observe_host("before_status")
        self.client.expected_boot_id = self.boot_id
        value = self.client.status()
        self.observe_host("after_status")
        result = self.response(value)
        required = {"wifi": "connected", "mqtt": "ready", "frp": "ready", "config": "ready", "ota": "ready"}
        capabilities = result.get("capabilities")
        if not isinstance(capabilities, dict) or any(capabilities.get(k) != v for k, v in required.items()):
            raise RunInterrupted("设备网络／MQTT／FRP／配置／OTA 未就绪")
        self.journal.record("network_ready", boot_id=self.boot_id, response=value)
        return value

    def firmware(self, expected_image=None, slot=None):
        value = self.command("firmware.status")
        result = self.response(value)
        candidates = self.images if expected_image is None else [expected_image]
        if (set(result) != {"firmware_sha256", "image_size_bytes", "target", "ota_slot"} or
                not any(result["firmware_sha256"] == image["sha256"] and result["image_size_bytes"] == image["size_bytes"] and
                        result["target"] == image["target"] for image in candidates) or
                result["ota_slot"] not in {"ota_0", "ota_1"} or (slot is not None and result["ota_slot"] != slot)):
            raise RunInterrupted("实际运行固件／target／槽不在冻结 R5 A/C 集合中")
        self.journal.record("firmware_observed", response=value)
        return result

    def business(self):
        value = self.command("business.status")
        result = self.response(value)
        if (set(result) != {"byte_count", "state", "window_deadline_uptime_ms"} or
                type(result["byte_count"]) is not int or not 0 <= result["byte_count"] <= (1 << 31) - 1 or
                result["state"] not in {"idle", "active"} or
                type(result["window_deadline_uptime_ms"]) is not int or result["window_deadline_uptime_ms"] < 0):
            raise RunInterrupted("原生业务状态不适合本轮有限事件")
        return result

    def new_session(self):
        session = self.session_factory(self.args, self.args.device_id, self.boot_id, self.key, clock=self.clock)
        session.observe_host = self.observe_host
        try:
            return session.open()
        except BaseException:
            session.close()
            self.observe_host("mqtt_closed")
            raise

    def event(self):
        self.ready()
        count = self.business()["byte_count"]
        session = self.new_session()
        completed = False
        try:
            value = session.publish_maximum(count)
            self.events += 1
            self.journal.record("business_completed", **value)
            completed = True
        finally:
            if session.last_attempt is not None:
                self.journal.record("business_attempt_observed", **session.last_attempt, completion_verified=completed)
            session.close()
            self.journal.record("host_mqtt_session_closed", boot_id=self.boot_id)
            self.observe_host("mqtt_closed")

    def cleanup_observation(self):
        current = self.ready()
        count = self.business()["byte_count"]
        paused = self.response(self.command("business.pause", parameters={}, current=current))
        if paused.get("state") != "paused" or paused.get("byte_count") != count or paused.get("window_deadline_uptime_ms") != 0:
            raise RunInterrupted("业务暂停未清定时窗口或改变计数")
        current = self.ready()
        resumed = self.response(self.command("business.resume", parameters={}, current=current))
        if resumed != {"byte_count": count, "state": "idle", "window_deadline_uptime_ms": 0}:
            raise RunInterrupted("业务恢复未到同计数 idle 状态")
        self.journal.record("cleanup_observed", boot_id=self.boot_id, business=resumed,
            scope="business_idle_and_host_session_close", device_resource_reclamation_verified=False)

    def operation_boundary(self, operation_id):
        return {"reserved": True,
            "attempted_command": getattr(self.client, "last_ota_command_attempt_id", None) == operation_id,
            "admission_confirmed": getattr(self.client, "last_ota_admission_confirmed_id", None) == operation_id,
            "scope": "ota_start_command_call_and_running_reply; transmitted_bytes_not_observed"}

    def reconcile_interrupted_operation(self, operation, error):
        """异常后仅一次有限原 ID 读取；任何结果不替换主异常或授予成功。"""
        operation_id = operation["operation_id"]
        boundary = self.operation_boundary(operation_id)
        record = {"operation_id": operation_id, "boundary": boundary,
                  "primary_error_type": type(error).__name__, "changes_run_outcome": False}
        try:
            if not boundary["attempted_command"]:
                self.journal.record("ota_exception_read_skipped", **record, reason="reserved_without_command_attempt")
                return
            try:
                image = operation["image"]
                value = self.client.result(operation_id)
                # Preserve the actual authenticated result first. A legitimate
                # reboot can expose pending/rollback facts without a final
                # candidate receipt; identity certainty is a separate verdict.
                self.journal.record("ota_exception_original_id_response_observed", **record,
                    response=value, protocol_scope="existing_frp_result_hmac_and_envelope_validation",
                    qualification_verified=False)
                expected = {"operation_id": operation_id, "sha256": image["sha256"],
                    "image_size_bytes": image["size_bytes"], "target": image["target"],
                    "target_slot": operation["target_slot"]}
                device_verified = isinstance(value, dict) and value.get("device_id") == self.args.device_id
                receipt_verified = isinstance(value, dict) and value.get("result") == expected
                try:
                    boot = business_event.canonical_uuid(value["boot_id"])
                    boot_relation = "source" if boot == operation["source_boot_id"] else "new"
                except (KeyError, TypeError, ValueError):
                    boot_relation = "invalid"
                boot_verified = boot_relation == "source" or (boot_relation == "new" and receipt_verified)
                verified = device_verified and boot_verified and receipt_verified
                self.journal.record("ota_exception_original_id_read", **record, response=value,
                    device_identity_verified=device_verified, receipt_identity_verified=receipt_verified,
                    boot_relation=boot_relation, boot_identity_verified=boot_verified,
                    identity_verified=verified, identity_verdict="verified_receipt" if verified else "uncertain",
                    qualification_verified=False)
            except BaseException as read_error:
                self.journal.record("ota_exception_original_id_read_failed", **record,
                    error={"type": type(read_error).__name__, "message": str(read_error)[:512]})
        except BaseException:
            # Logging/readonly failure must not hide the original interruption.
            pass

    def cycle(self):
        self.active_operation = None
        try:
            self.cycle_observations()
            if self.active_operation is not None:
                operation_id = self.active_operation["operation_id"]
                self.journal.record("ota_operation_boundary_observed", operation_id=operation_id,
                    boundary=self.operation_boundary(operation_id))
        except BaseException as error:
            if self.active_operation is not None:
                self.reconcile_interrupted_operation(self.active_operation, error)
            raise
        finally:
            self.active_operation = None

    def cycle_observations(self):
        for image in self.images:
            if file_snapshot(image["path"]) != {key: value for key, value in image.items() if key not in {"label", "target"}}:
                raise RunInterrupted("冻结 signed A/C 在周期开始前改变")
            self.observe_host("frozen_image_bound")
        self.ready()
        source = self.firmware()
        destination = next(image for image in self.images if image["sha256"] != source["firmware_sha256"])
        self.event()
        count = self.business()["byte_count"]
        session = self.new_session()
        signal, cancel, business_failed = threading.Event(), threading.Event(), threading.Event()
        outcomes = queue.Queue(maxsize=1)
        transfer = {"first_progress_monotonic_seconds": None, "last_progress_monotonic_seconds": None,
                    "uploaded_bytes": 0}

        def worker():
            while not signal.wait(0.1):
                if cancel.is_set():
                    return
            if cancel.is_set():
                return
            try:
                outcomes.put((session.publish_maximum(count), None))
            except BaseException as error:
                business_failed.set()
                outcomes.put((None, error))

        def progress(uploaded, total):
            self.observe_host("ota_upload_progress")
            if business_failed.is_set():
                raise RunInterrupted("上传期间业务结果异常；中止本次传输并保留原 OTA ID")
            if total != destination["size_bytes"] or not transfer["uploaded_bytes"] < uploaded <= total:
                raise RunInterrupted("上传进度尺寸或单调性异常")
            now = self.clock()
            if transfer["first_progress_monotonic_seconds"] is None:
                transfer["first_progress_monotonic_seconds"] = now
                signal.set()
            transfer["last_progress_monotonic_seconds"] = now
            transfer["uploaded_bytes"] = uploaded

        operation_id = str(uuid.uuid4())
        self.operations.append(operation_id)
        self.active_operation = {"operation_id": operation_id, "source_boot_id": self.boot_id,
            "image": destination, "target_slot": "ota_1" if source["ota_slot"] == "ota_0" else "ota_0"}
        self.journal.record("ota_operation_created", operation_id=operation_id, source_boot_id=self.boot_id,
                            image=destination, source_slot=source["ota_slot"])
        thread = threading.Thread(target=worker, name="base-finite-business")
        business_completed = False
        thread_started = False
        self.observe_host("before_expected_ota_window", record=True)
        with self.clock_sample_lock:
            self.expected_ota_window_started = self.clock()
        primary_error = None
        try:
            thread.start()
            thread_started = True
            self.client.expected_image = destination
            final = self.client.ota_start(operation_id, destination["path"], destination["target"],
                result_timeout_seconds=self.args.ota_result_timeout_seconds, progress=progress)
            self.journal.record("ota_result_observed", operation_id=operation_id, response=final)
            if transfer["first_progress_monotonic_seconds"] is None:
                raise RunInterrupted("上传没有触发有限业务事件")
            thread.join(EVENT_TIMEOUT_SECONDS + 1)
            if thread.is_alive():
                raise RunInterrupted("有限业务线程未按原期限结束")
            if outcomes.empty():
                raise RunInterrupted("上传没有触发有限业务事件")
            business, error = outcomes.get_nowait()
            if error is not None:
                raise RunInterrupted("上传期间业务结果未能核实；停止并保留原 OTA ID") from error
            self.events += 1
            self.journal.record("business_completed", **business)
            business_completed = True
            start = max(transfer["first_progress_monotonic_seconds"], business["published_monotonic_seconds"])
            end = min(transfer["last_progress_monotonic_seconds"], business["completed_reported_monotonic_seconds"])
            self.journal.record("ota_upload_observed", operation_id=operation_id, **transfer,
                business_observation_overlap=end > start,
                overlap_start_monotonic_seconds=start if end > start else None,
                overlap_end_monotonic_seconds=end if end > start else None,
                scope="host_upload_progress_and_same_boot_reported", device_instantaneous_peak_verified=False)
            receipt = final.get("result")
            expected_slot = "ota_1" if source["ota_slot"] == "ota_0" else "ota_0"
            if (final.get("device_id") != self.args.device_id or final.get("state") != "succeeded" or
                    final.get("boot_id") == self.boot_id or not isinstance(receipt, dict) or receipt != {
                        "operation_id": operation_id, "sha256": destination["sha256"],
                        "image_size_bytes": destination["size_bytes"], "target": destination["target"],
                        "target_slot": expected_slot}):
                raise RunInterrupted("原 OTA ID 没有新 boot 的精确持久成功；禁止重发或另起操作")
            self.boot_id = business_event.canonical_uuid(final["boot_id"])
            self.journal.record("ota_persisted", operation_id=operation_id, response=final)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            try:
                cancel.set()
                session.cancelled.set()
                signal.set()
                if thread_started:
                    thread.join(1)
                session.close()
                if thread_started:
                    thread.join(1)
                if session.last_attempt is not None:
                    self.journal.record("business_attempt_observed", **session.last_attempt,
                                        completion_verified=business_completed, operation_id=operation_id)
                self.journal.record("ota_upload_closed", operation_id=operation_id, **transfer)
                self.journal.record("host_mqtt_session_closed", operation_id=operation_id,
                                    worker_released=not thread.is_alive())
                ended = self.clock()
                elapsed = ended - self.expected_ota_window_started
                # Existing FRP: two 5s commands, 300s transfer, 30s prepared,
                # original-result budget plus one final 5s query; business join 31s,
                # cancellation joins/host close use a further 5s observation margin.
                limit = 381 + self.args.ota_result_timeout_seconds
                self.maximum_expected_ota_window_seconds = max(self.maximum_expected_ota_window_seconds, elapsed)
                self.expected_ota_window_count += 1
                self.journal.record("expected_ota_window_closed", operation_id=operation_id,
                    started_monotonic_seconds=self.expected_ota_window_started,
                    ended_monotonic_seconds=ended, elapsed_seconds=elapsed, allowed_seconds=limit,
                    scope="bounded_transfer_prepare_original_result_and_business_join",
                    device_continuity_verified=False)
                try:
                    self.observe_host("expected_ota_window_closed")
                finally:
                    with self.clock_sample_lock:
                        self.expected_ota_window_started = None
                        self.host_last_progress = ended
                if elapsed < 0 or elapsed > limit:
                    raise RunInterrupted("预期 OTA 等待窗口超过既有传输／结果与有限线程预算")
                if thread.is_alive():
                    raise RunInterrupted("有限业务线程仍未释放，禁止进入后续周期")
            except BaseException as cleanup_error:
                if primary_error is None:
                    raise
                try:
                    self.journal.record("ota_cleanup_failed_after_interruption", operation_id=operation_id,
                        error={"type": type(cleanup_error).__name__, "message": str(cleanup_error)[:512]},
                        primary_error_type=type(primary_error).__name__)
                except BaseException:
                    pass
        self.ready()
        self.firmware(destination, expected_slot)
        for image in self.images:
            if file_snapshot(image["path"]) != {key: value for key, value in image.items() if key not in {"label", "target"}}:
                raise RunInterrupted("输入 signed A/C 在运行期间改变")
        self.event()
        self.cleanup_observation()
        self.cycles += 1
        self.journal.record("cycle_completed", cycle=self.cycles, boot_id=self.boot_id,
            composition=["same_boot_network_ready", "maximum_business", "single_frp_ota_with_independent_business",
                         "new_boot_network_and_firmware", "maximum_business", "pause_resume_idle", "host_session_close"],
            device_resource_reclamation_verified=False)

    def run(self):
        started = self.clock()
        self.host_last_progress = started
        self.clock_sample_monotonic = started
        self.clock_sample_wall = self.wall_clock()
        interrupted = None
        self.journal.record("run_started", mode=self.args.mode, started_utc=datetime.now(timezone.utc).isoformat(),
            initial_boot_id=self.boot_id, images=self.images, r5_evidence=self.r5,
            r5_qualification_verified=False, capacity_log_start=self.capacity,
            allowed_host_progress_gap_seconds=HOST_PROGRESS_GAP_SECONDS,
            allowed_clock_delta_discrepancy_seconds=CLOCK_DELTA_TOLERANCE_SECONDS,
            continuity_scope="single_host_execution_with_separate_bounded_ota_windows")
        try:
            if self.args.mode == "cycles-100":
                for _ in range(CYCLE_COUNT):
                    self.cycle()
            else:
                deadline = started + SOAK_SECONDS
                next_ota, next_event = started, started
                while True:
                    # Check the wake gap before deadline: a suspended process
                    # waking after 72h must never inherit continuous observation.
                    self.observe_host("soak_loop_wake")
                    now = self.clock()
                    if now >= deadline:
                        self.observe_host("soak_deadline", record=True)
                        break
                    if now >= next_ota:
                        self.observe_host("soak_ota_due", record=True)
                        self.cycle()
                        next_ota = self.clock() + self.args.ota_interval_seconds
                        next_event = self.clock() + self.args.business_interval_seconds
                    elif now >= next_event:
                        self.observe_host("soak_business_due", record=True)
                        self.event()
                        next_event = self.clock() + self.args.business_interval_seconds
                    else:
                        self.sleep(min(1, deadline - now, next_ota - now, next_event - now))
            self.ready()
            self.firmware()
            # These are immutable original evidence and candidate bindings, not R5 verdicts.
            if file_snapshot(self.args.r5_evidence_file, private=True) != self.r5:
                raise RunInterrupted("R5 原件在本轮改变")
        except (Exception, KeyboardInterrupt) as error:
            interrupted = {"type": type(error).__name__, "message": str(error)[:512]}
            self.journal.record("interrupted", error=interrupted, original_operation_ids=self.operations)
        capacity = None
        try:
            capacity = file_snapshot(self.args.capacity_log_file, private=True, append_only=True)
            if (capacity["file_device"], capacity["file_inode"]) != (self.capacity["file_device"], self.capacity["file_inode"]) or capacity["size_bytes"] < self.capacity["size_bytes"]:
                raise RunInterrupted("外部容量日志被替换或截断")
            prefix = file_snapshot(self.args.capacity_log_file, private=True, append_only=True,
                                   prefix_bytes=self.capacity["size_bytes"])
            if prefix != self.capacity:
                raise RunInterrupted("外部容量日志的已绑定前缀被改写")
            self.journal.record("external_capacity_log_bound", snapshot=capacity, capacity_verified=False)
        except (Exception, KeyboardInterrupt) as error:
            binding_error = {"type": type(error).__name__, "message": str(error)[:512]}
            capacity = None
            self.journal.record("external_capacity_binding_failed", error=binding_error)
            if interrupted is None:
                interrupted = binding_error
                self.journal.record("interrupted", error=interrupted, original_operation_ids=self.operations)
        try:
            # 最终原件摘要／前缀核对仍属本轮宿主执行；完成判定前同样检查
            # 进度空档及双时钟，不让收尾休眠或回退继承completed。
            ended = self.observe_host("run_final_boundary")
        except (Exception, KeyboardInterrupt) as error:
            with self.clock_sample_lock:
                ended = self.clock_sample_monotonic
            final_error = {"type": type(error).__name__, "message": str(error)[:512]}
            if interrupted is None:
                interrupted = final_error
                self.journal.record("interrupted", error=interrupted, original_operation_ids=self.operations)
            else:
                try:
                    self.journal.record("final_host_observation_failed_after_interruption", error=final_error,
                        primary_error_type=interrupted["type"], changes_run_outcome=False)
                except BaseException:
                    pass
        # 正常终点是本次已核验采样；异常保留最近已记录的原始采样，
        # 采样前失败时不会伪称本次取得了新时刻，也不纳入未经核验的时刻。
        # 时钟回退时保留原始负时长；interrupted和时钟原始增量说明原因。
        complete = interrupted is None and ((self.args.mode == "cycles-100" and self.cycles == CYCLE_COUNT) or
                    (self.args.mode == "soak-72h" and ended - started >= SOAK_SECONDS and self.cycles > 0))
        summary = {"run_id": self.journal.run_id, "mode": self.args.mode,
            "state": "completed" if complete else "interrupted", "observation_completed": complete,
            "started_monotonic_seconds": started, "ended_monotonic_seconds": ended,
            "observation_end_scope": "last_recorded_host_clock_sample",
            "elapsed_seconds": ended - started, "required_cycles": CYCLE_COUNT if self.args.mode == "cycles-100" else None,
            "required_continuous_seconds": SOAK_SECONDS if self.args.mode == "soak-72h" else None,
            "requested_observation_end_monotonic_seconds": started + SOAK_SECONDS if self.args.mode == "soak-72h" else None,
            "completed_cycles": self.cycles, "completed_business_events": self.events,
            "original_operation_ids": self.operations, "last_boot_id": self.boot_id, "error": interrupted,
            "r5_evidence": self.r5, "images": self.images, "qualified": False, "r6_passed": False,
            "allowed_host_progress_gap_seconds": HOST_PROGRESS_GAP_SECONDS,
            "maximum_host_progress_gap_seconds": self.maximum_host_progress_gap_seconds,
            "allowed_clock_delta_discrepancy_seconds": CLOCK_DELTA_TOLERANCE_SECONDS,
            "maximum_clock_delta_discrepancy_seconds": self.maximum_clock_delta_discrepancy_seconds,
            "expected_ota_window_count": self.expected_ota_window_count,
            "maximum_expected_ota_window_seconds": self.maximum_expected_ota_window_seconds,
            "continuity_scope": "single_host_execution_with_separate_bounded_ota_windows",
            "device_instantaneous_continuity_verified": False,
            "capacity_log_start": self.capacity, "capacity_log_end": capacity,
            "external_checks_required": ["device_identity_recovery_lease_and_formal_trust", "original_r5_verdict",
                "all_device_resource_reclamation", "capacity_and_task_stack_trends", "real_public_frps_path",
                "physical_board_and_flash_lifetime"]}
        self.journal.finish(summary)
        return summary


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("cycles-100", "soak-72h"), required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--initial-boot-id", required=True)
    for name in ("a", "c"):
        parser.add_argument(f"--image-{name}", dest=f"image_{name}_file", type=Path, required=True)
        parser.add_argument(f"--image-{name}-sha256", required=True)
        parser.add_argument(f"--image-{name}-size-bytes", type=int, required=True)
        parser.add_argument(f"--image-{name}-target", choices=frp_ota.OTA_TARGETS, required=True)
    parser.add_argument("--r5-evidence-file", type=Path, required=True)
    parser.add_argument("--r5-evidence-sha256", required=True)
    parser.add_argument("--capacity-log-file", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--frp-management-key-env", default="ESP_BASE_FRP_MANAGEMENT_KEY_HEX")
    parser.add_argument("--business-management-key-file", type=Path, required=True)
    parser.add_argument("--mqtt-host", required=True)
    parser.add_argument("--mqtt-port", type=int, required=True)
    parser.add_argument("--ca-file", type=Path, required=True)
    parser.add_argument("--credentials-file", type=Path, required=True)
    parser.add_argument("--ota-result-timeout-seconds", type=float, default=120)
    parser.add_argument("--ota-interval-seconds", type=int, default=3600)
    parser.add_argument("--business-interval-seconds", type=int, default=5)
    args = parser.parse_args(argv)
    business_event.canonical_uuid(args.device_id)
    business_event.canonical_uuid(args.initial_boot_id)
    checked_sha256(args.image_a_sha256)
    checked_sha256(args.image_c_sha256)
    checked_sha256(args.r5_evidence_sha256)
    if (not args.mqtt_host or any(ord(char) < 33 for char in args.mqtt_host) or
            not 1 <= args.mqtt_port <= 65535 or not 5 <= args.business_interval_seconds <= 60 or
            not 60 <= args.ota_interval_seconds <= 12 * 3600 or not 0 < args.ota_result_timeout_seconds <= 3600):
        raise ValueError("MQTT 端点或有限运行间隔／期限无效")
    return args


def main(argv=None):
    journal = None
    runner = None
    try:
        args = arguments(argv)
        text = os.environ.get(args.frp_management_key_env, "")
        if not re.fullmatch("[0-9a-f]{64}", text):
            raise ValueError("既有 FRP 管理 key 环境变量缺失或无效")
        key = business_event.private_key(args.business_management_key_file)
        publisher.credentials(args.credentials_file)
        file_snapshot(args.ca_file)
        import paho.mqtt.client  # Dependency check occurs before any device operation.
        client = BoundFrpClient(args.endpoint, args.device_id, bytes.fromhex(text))
        journal = Journal(args.output_directory)
        runner = LifecycleRun(args, client, key, journal)
        summary = runner.run()
    except (Exception, KeyboardInterrupt) as error:
        if journal is not None and not journal.output.closed:
            ended = journal.clock()
            journal.finish({"run_id": journal.run_id, "state": "interrupted", "qualified": False,
                            "r6_passed": False, "observation_completed": False,
                            "started_monotonic_seconds": journal.start,
                            "ended_monotonic_seconds": ended, "elapsed_seconds": ended - journal.start,
                            "original_operation_ids": getattr(runner, "operations", []),
                            "error": {"type": type(error).__name__, "message": str(error)[:512]}})
        print(f"有限原生运行未完成：{error}", file=sys.stderr)
        return 1
    print("有限原生运行记录")
    print(f"  Run：{summary['run_id']}")
    print(f"  状态：{summary['state']}")
    print(f"  已完成周期：{summary['completed_cycles']}；连续观察：{summary['elapsed_seconds']:.3f} 秒")
    print(f"  证据：{journal.directory / 'summary.json'}")
    print("  实体资源、容量、Flash及R6资格仍由外部原件裁决")
    return 0 if summary["observation_completed"] else 1


if __name__ == "__main__":
    sys.exit(main())
