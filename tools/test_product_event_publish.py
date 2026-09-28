#!/usr/bin/env python3
"""产品事件发布器的精确帧、前置高水位与设备结果验证。"""

import contextlib
import hashlib
import io
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock

import product_event
import product_event_publish


DEVICE = "22222222-2222-4222-8222-222222222222"
BOOT = "33333333-3333-4333-8333-333333333333"
PACKAGE = "11" * 32


def private(path, payload):
    path.write_bytes(payload)
    path.chmod(0o600)


def reported(sequence, completed=None, outcome="none"):
    return json.dumps({
        "protocol_version": 1, "device_id": DEVICE, "boot_id": BOOT,
        "uptime_ms": 1000, "revision": 1, "wifi_state": "connected",
        "time_ready": True, "frp_state": "unconfigured",
        "last_accepted_event_sequence": sequence,
        "last_completed_event_sequence": completed,
        "last_completed_package_sha256": PACKAGE if completed else None,
        "last_completed_event_sha256": hashlib.sha256(b"\x01\x02\x03").hexdigest() if completed else None,
        "last_event_outcome": outcome,
        "last_guest_result": 3 if completed else None,
    }).encode()


class FakeClient:
    published = []
    before = reported(0)
    after = reported(1, 1, "succeeded")
    disconnect_after_publish = False

    def __init__(self, *args, **kwargs):
        if kwargs.get("reconnect_on_failure") is not False:
            raise AssertionError("业务事件客户端不得自动重连并重发未确认的 QoS 1 消息")
        self.on_connect = self.on_subscribe = self.on_disconnect = self.on_message = None

    def username_pw_set(self, *args):
        pass

    def tls_set(self, **kwargs):
        pass

    def connect(self, *args, **kwargs):
        pass

    def loop_start(self):
        self.on_connect(self, None, None, types.SimpleNamespace(is_failure=False), None)

    def subscribe(self, topic, qos):
        self.on_subscribe(self, None, 7, [types.SimpleNamespace(value=1)], None)
        self.message(topic, self.before)
        return 0, 7

    def message(self, topic, payload):
        self.on_message(self, None, types.SimpleNamespace(
            topic=topic, payload=payload, qos=1, retain=False))

    def publish(self, topic, payload, qos, retain):
        self.published.append((topic, payload, qos, retain))
        if self.disconnect_after_publish:
            self.on_disconnect(self, None, None, None, None)
        else:
            self.message(topic.removesuffix("/event") + "/reported", self.after)
        return types.SimpleNamespace(rc=0, wait_for_publish=lambda timeout: None,
                                     is_published=lambda: not self.disconnect_after_publish)

    def disconnect(self):
        pass

    def loop_stop(self):
        pass


class ProductEventPublishTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = pathlib.Path(self.directory.name)
        self.key = root / "key"
        self.frame = root / "frame"
        self.account = root / "account"
        private(self.key, bytes(range(32)).hex().encode())
        private(self.frame, product_event.event_frame(
            bytes(range(32)), DEVICE, BOOT, PACKAGE, 1, b"\x01\x02\x03"))
        private(self.account, b'{"username":"controller","password":"secret"}')
        FakeClient.published = []
        FakeClient.before = reported(0)
        FakeClient.after = reported(1, 1, "succeeded")
        FakeClient.disconnect_after_publish = False
        client_module = types.ModuleType("paho.mqtt.client")
        client_module.Client = FakeClient
        client_module.MQTTv311 = 4
        client_module.MQTT_ERR_SUCCESS = 0
        client_module.CallbackAPIVersion = types.SimpleNamespace(VERSION2=2)
        mqtt_module = types.ModuleType("paho.mqtt")
        mqtt_module.client = client_module
        paho_module = types.ModuleType("paho")
        paho_module.mqtt = mqtt_module
        patcher = mock.patch.dict(sys.modules, {"paho": paho_module,
            "paho.mqtt": mqtt_module, "paho.mqtt.client": client_module})
        patcher.start()
        self.addCleanup(patcher.stop)

    def invoke(self):
        args = ["product_event_publish.py", "--host", "broker.example.test",
                "--port", "8883", "--ca-file", str(self.key),
                "--credentials-file", str(self.account),
                "--management-key-file", str(self.key),
                "--frame-file", str(self.frame), "--device-id", DEVICE,
                "--boot-id", BOOT, "--package-sha256", PACKAGE,
                "--event-sequence", "1", "--timeout-seconds", "5"]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            code = product_event_publish.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_publish_after_fresh_sequence_and_exact_guest_result(self):
        code, output, error = self.invoke()
        self.assertEqual((code, error), (0, ""))
        self.assertEqual(len(FakeClient.published), 1)
        topic, payload, qos, retained = FakeClient.published[0]
        self.assertEqual((topic, qos, retained), (f"esp-base/{DEVICE}/event", 1, False))
        self.assertEqual(payload, self.frame.read_bytes())
        self.assertEqual(json.loads(output)["event_outcome"], "succeeded")

    def test_stale_highwater_prevents_publication(self):
        FakeClient.before = reported(1, 1, "succeeded")
        code, output, error = self.invoke()
        self.assertEqual(code, 1)
        self.assertEqual(output, "")
        self.assertIn("未发布", error)
        self.assertEqual(FakeClient.published, [])

    def test_wrong_post_publish_boot_is_unknown_without_resend(self):
        value = json.loads(reported(1, 1, "succeeded"))
        value["boot_id"] = DEVICE
        FakeClient.after = json.dumps(value).encode()
        code, output, error = self.invoke()
        self.assertEqual(code, 2)
        self.assertEqual(output, "")
        self.assertIn("已发布", error)
        self.assertEqual(len(FakeClient.published), 1)

    def test_same_sequence_and_package_with_other_event_is_unknown(self):
        value = json.loads(reported(1, 1, "succeeded"))
        value["last_completed_event_sha256"] = "22" * 32
        FakeClient.after = json.dumps(value).encode()
        code, output, error = self.invoke()
        self.assertEqual((code, output), (2, ""))
        self.assertIn("结果与本帧不一致", error)
        self.assertEqual(len(FakeClient.published), 1)

    def test_disconnect_after_publish_is_unknown_without_reconnect_or_resend(self):
        FakeClient.disconnect_after_publish = True
        code, output, error = self.invoke()
        self.assertEqual((code, output), (2, ""))
        self.assertIn("已发布", error)
        self.assertEqual(len(FakeClient.published), 1)

    def test_wrong_frame_or_reported_boot_is_not_accepted(self):
        with self.assertRaises(ValueError):
            product_event_publish.verified_frame(self.frame, self.key, DEVICE, BOOT,
                                                 "22" * 32, 1)
        bad = json.loads(reported(0))
        bad["boot_id"] = DEVICE
        with self.assertRaises(ValueError):
            product_event_publish.reported_message(json.dumps(bad).encode(), DEVICE, BOOT)


if __name__ == "__main__":
    unittest.main()
