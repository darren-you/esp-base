#!/usr/bin/env python3
"""用真实 POSIX 伪终端验证串口示例的字节传输与背压期限。"""
import importlib.util
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import pty
import select
import termios
import tempfile
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("device_control", Path(__file__).with_name("device_control.py"))
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class SerialTransportTests(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        options = termios.tcgetattr(self.slave)
        options[2] |= termios.HUPCL
        termios.tcsetattr(self.slave, termios.TCSANOW, options)
        self.port = control.SerialPort(os.ttyname(self.slave))

    def tearDown(self):
        self.port.close()
        os.close(self.slave)
        os.close(self.master)

    def test_raw_duplex_preserves_bytes_and_disables_hangup(self):
        self.assertEqual(termios.tcgetattr(self.port.fd)[2] & termios.HUPCL, 0)
        payload = bytes(range(256))
        os.write(self.master, payload)
        actual = bytearray()
        deadline = time.monotonic() + 2
        while len(actual) < len(payload) and time.monotonic() < deadline:
            actual.extend(self.port.read(len(payload) - len(actual)))
        self.assertEqual(actual, payload)
        self.assertEqual(self.port.write(payload), len(payload))
        self.assertTrue(select.select([self.master], [], [], 2)[0])
        self.assertEqual(os.read(self.master, len(payload)), payload)

    def test_backpressure_ends_as_unknown_within_bound(self):
        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, "unknown"):
            self.port.write(b"x" * (4 * 1024 * 1024))
        self.assertLess(time.monotonic() - started, 2.5)


class ConfigurationV3Tests(unittest.TestCase):
    def setUp(self):
        self.config = {
            "schema_version": 3,
            "wifi": {"ssid": "test-network", "password": "test-password"},
            "mqtt": {
                "hostname": "broker.example.test", "port": 8883,
                "username": "device", "password": "secret",
                "ca_pem": "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n",
                "management_key_hex": "01" + "00" * 31,
            },
            "frp": None, "business": None,
        }

    def test_complete_v3_and_null_capabilities(self):
        control.validate_configuration(self.config)
        candidate = copy.deepcopy(self.config)
        candidate["wifi"] = None
        candidate["mqtt"] = None
        control.validate_configuration(candidate)
        candidate["frp"] = {
            "server_hostname": "frp.example.test", "server_port": 7000,
            "token": "test-frp-token",
            "ca_pem": "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n",
            "proxy_name": "base-device", "remote_port": 10200, "local_port": 8123,
            "management_key_hex": "02" + "00" * 31,
        }
        control.validate_configuration(candidate)
        for field, value in (("server_hostname", "-frp.example.test"), ("server_port", 0),
                             ("token", "bad token"), ("ca_pem", "missing CA"),
                             ("proxy_name", "bad/name"), ("remote_port", True),
                             ("local_port", 0), ("management_key_hex", "00" * 32)):
            rejected = copy.deepcopy(candidate)
            rejected["frp"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                control.validate_configuration(rejected)

    def test_v1_and_malformed_fields_are_rejected(self):
        variants = [
            ("schema_version", 1),
            ("schema_version", 2),
            ("schema_version", True),
            ("frp", {}),
            ("mqtt.hostname", "-broker.example.test"),
            ("mqtt.hostname", "broker..example.test"),
            ("mqtt.port", 0),
            ("mqtt.port", True),
            ("mqtt.username", ""),
            ("mqtt.username", "\u0000"),
            ("mqtt.password", "\ud800"),
            ("mqtt.ca_pem", "missing certificate"),
            ("mqtt.ca_pem", "-----BEGIN CERTIFICATE-----\u0000-----END CERTIFICATE-----"),
            ("mqtt.management_key_hex", "00" * 32),
            ("mqtt.management_key_hex", "A1" + "00" * 31),
            ("mqtt.management_key_hex", "01"),
        ]
        for key, value in variants:
            with self.subTest(field=key):
                candidate = copy.deepcopy(self.config)
                if key.startswith("mqtt."):
                    candidate["mqtt"][key[5:]] = value
                else:
                    candidate[key] = value
                with self.assertRaises(ValueError):
                    control.validate_configuration(candidate)
        candidate = copy.deepcopy(self.config)
        candidate["mqtt"]["extra"] = 1
        with self.assertRaises(ValueError):
            control.validate_configuration(candidate)

    def test_max_length_and_frame_limit(self):
        candidate = copy.deepcopy(self.config)
        candidate["mqtt"]["hostname"] = ".".join(["a" * 63] * 3 + ["a" * 61])
        candidate["mqtt"]["username"] = "u" * 128
        candidate["mqtt"]["password"] = "p" * 256
        candidate["mqtt"]["ca_pem"] = "-----BEGIN CERTIFICATE-----" + "A" * (4096 - 52) + "-----END CERTIFICATE-----"
        control.validate_configuration(candidate)
        candidate["mqtt"]["ca_pem"] += "A"
        with self.assertRaises(ValueError):
            control.validate_configuration(candidate)
        class NeverWrite:
            def write(self, payload):
                raise AssertionError("oversized serial frame was sent")
        with self.assertRaisesRegex(ValueError, "9216"):
            control.send(NeverWrite(), {"config": "A" * 9216})


class OtaClientTests(unittest.TestCase):
    def test_signed_image_request_and_original_id_query(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        operation = "44444444-4444-4444-8444-444444444444"
        current = {"device_id": device, "boot_id": boot}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "candidate.bin")
            path.write_bytes(b"signed-image-fixture")
            observed = []
            fresh = {**current, "result": {"uptime_ms": 1234,
                     "capabilities": {"ota": "ready"}}}

            def capture(_port, request):
                observed.append(request)

            def accepted(_port, request_id, _deadline):
                yield {**current, "request_id": request_id, "state": "running",
                       "error_code": None, "result": None}

            with (mock.patch.object(control, "status", return_value=fresh),
                  mock.patch.object(control, "send", side_effect=capture),
                  mock.patch.object(control, "read_result", side_effect=accepted)):
                pending = control.ota_start(object(), current, operation, str(path),
                                            "https://images.example.test/candidate.bin",
                                            "esp32c3/esp_base")
            self.assertEqual(len(observed), 1)
            self.assertEqual(observed[0]["command"], "ota.start")
            self.assertEqual(observed[0]["target_boot_id"], boot)
            self.assertEqual(observed[0]["expires_at_uptime_ms"], 11234)
            self.assertEqual(observed[0]["parameters"], {
                "operation_id": operation,
                "image_url": "https://images.example.test/candidate.bin",
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "image_size_bytes": len(path.read_bytes()),
                "target": "esp32c3/esp_base",
                "signature": {"scheme": "esp_secure_boot_v2_rsa3072"}})
            self.assertEqual(pending["state"], "running")
            self.assertIsNone(pending["result"])

            next_boot = "55555555-5555-4555-8555-555555555555"
            final = {**current, "boot_id": next_boot, "state": "succeeded",
                     "error_code": None, "result": {
                         "operation_id": operation,
                         "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "image_size_bytes": len(path.read_bytes()),
                         "target": "esp32c3/esp_base", "target_slot": "ota_1"}}

            def reply(_port, _request_id, _deadline):
                yield final

            observed.clear()
            with (mock.patch.object(control, "send", side_effect=capture),
                  mock.patch.object(control, "read_result", side_effect=reply)):
                self.assertEqual(control.ota_result(object(), current, operation,
                    final["result"]["sha256"], final["result"]["image_size_bytes"]), final)
            self.assertEqual(observed[0]["command"], "ota.result")
            self.assertEqual(observed[0]["parameters"], {"operation_id": operation})


    def test_no_write_for_wrong_boot_or_oversized_image(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        current = {"device_id": device, "boot_id": boot}
        operation = "44444444-4444-4444-8444-444444444444"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "candidate.bin")
            path.write_bytes(b"test")
            fresh = {**current, "boot_id": "55555555-5555-4555-8555-555555555555",
                     "result": {"uptime_ms": 1234,
                                "capabilities": {"ota": "ready"}}}
            with (mock.patch.object(control, "status", return_value=fresh),
                  mock.patch.object(control, "send") as send):
                with self.assertRaisesRegex(ValueError, "已重启"):
                    control.ota_start(object(), current, operation, str(path),
                                      "https://images.example.test/candidate.bin",
                                      "esp32c3/esp_base")
                send.assert_not_called()
            with path.open("wb") as output:
                output.truncate(0x1e0000 + 1)
            with mock.patch.object(control, "send") as send:
                with self.assertRaisesRegex(ValueError, "目标槽"):
                    control.ota_start(object(), current, operation, str(path),
                                      "https://images.example.test/candidate.bin",
                                      "esp32c3/esp_base")
                send.assert_not_called()

    def test_query_rejects_different_image_and_never_writes_again(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        operation = "44444444-4444-4444-8444-444444444444"
        current = {"device_id": device, "boot_id": boot}
        reply = {**current, "state": "succeeded", "error_code": None,
                 "result": {"operation_id": operation, "sha256": "ab" * 32,
                            "image_size_bytes": 512, "target": "esp32c3/esp_base",
                            "target_slot": "ota_1"}}
        sent = []

        def capture(_port, request):
            sent.append(request)

        def response(_port, _request_id, _deadline):
            yield reply

        with (mock.patch.object(control, "send", side_effect=capture),
              mock.patch.object(control, "read_result", side_effect=response)):
            with self.assertRaisesRegex(ValueError, "与请求不符"):
                control.ota_result(object(), current, operation, "cd" * 32, 512)
        self.assertEqual([request["command"] for request in sent], ["ota.result"])


class NativeControlTests(unittest.TestCase):
    def setUp(self):
        self.current = {"device_id": "22222222-2222-4222-8222-222222222222",
                        "boot_id": "33333333-3333-4333-8333-333333333333"}
        self.firmware = {"firmware_sha256": "ab" * 32, "image_size_bytes": 512,
                         "target": "esp32c3/esp_base", "ota_slot": "ota_0"}
        self.business = {"byte_count": 123, "state": "active",
                         "window_deadline_uptime_ms": 1300}

    def response(self, result, **fields):
        return {**self.current, "state": "succeeded", "error_code": None,
                "result": result, **fields}

    def query(self, command, reply):
        seen = []
        with (mock.patch.object(control, "send", side_effect=lambda _port, value: seen.append(value)),
              mock.patch.object(control, "read_result", side_effect=lambda *args: iter([reply]))):
            result = control.query_capability(object(), self.current, command)
        self.assertEqual(len(seen), 1)
        self.assertEqual(set(seen[0]), {"protocol_version", "request_id", "command"})
        self.assertEqual(seen[0]["command"], command)
        return result

    def test_independent_firmware_and_business_fields(self):
        for command, expected in (("firmware.status", self.firmware),
                                  ("business.status", self.business)):
            with self.subTest(command=command):
                self.assertEqual(self.query(command, self.response(expected))["result"], expected)
                for state in ("failed", "unknown"):
                    reply = self.response(None, state=state, error_code="state_unavailable")
                    self.assertEqual(self.query(command, reply), reply)

    def test_status_refuses_malformed_fields_and_changed_identity(self):
        for command, source, malformed in (
            ("firmware.status", self.firmware, (("firmware_sha256", "0" * 64),
                ("firmware_sha256", "AB" * 32), ("image_size_bytes", True),
                ("image_size_bytes", 0x1e0001), ("target", []), ("ota_slot", []))),
            ("business.status", self.business, (("byte_count", True), ("byte_count", -1),
                ("byte_count", 4294967296), ("state", []), ("state", "running"),
                ("window_deadline_uptime_ms", True), ("window_deadline_uptime_ms", -1)))):
            for key, value in malformed:
                with self.subTest(command=command, key=key, value=value), self.assertRaisesRegex(ValueError, "unknown"):
                    self.query(command, self.response({**source, key: value}))
            for removed in source:
                with self.subTest(command=command, removed=removed), self.assertRaisesRegex(ValueError, "unknown"):
                    self.query(command, self.response({key: value for key, value in source.items() if key != removed}))
            with self.assertRaisesRegex(ValueError, "unknown"):
                self.query(command, self.response({**source, "extra": None}))
            with self.assertRaisesRegex(ValueError, "另一设备或启动"):
                self.query(command, self.response(source, boot_id="55555555-5555-4555-8555-555555555555"))
            with self.assertRaisesRegex(ValueError, "unknown"):
                self.query(command, self.response(None, state="running"))

    def test_business_writes_bind_fresh_boot_and_send_once(self):
        fresh = {**self.current, "result": {"uptime_ms": 1234}}
        for command in ("business.pause", "business.resume"):
            seen = []
            def accepted(_port, request_id, _deadline):
                yield self.response(None, request_id=request_id)
            with (mock.patch.object(control, "status", return_value=fresh),
                  mock.patch.object(control, "send", side_effect=lambda _port, request: seen.append(request)),
                  mock.patch.object(control, "read_result", side_effect=accepted)):
                self.assertEqual(control.business_action(object(), self.current, command)["state"], "succeeded")
            self.assertEqual(len(seen), 1)
            self.assertEqual(seen[0]["command"], command)
            self.assertEqual(seen[0]["device_id"], self.current["device_id"])
            self.assertEqual(seen[0]["target_boot_id"], self.current["boot_id"])
            self.assertEqual(seen[0]["expires_at_uptime_ms"], 11234)
            self.assertEqual(seen[0]["parameters"], {})
        with (mock.patch.object(control, "status", return_value={**fresh, "boot_id": "55555555-5555-4555-8555-555555555555"}),
              mock.patch.object(control, "send") as send):
            with self.assertRaisesRegex(ValueError, "未发送命令"):
                control.business_action(object(), self.current, "business.pause")
            send.assert_not_called()
        with (mock.patch.object(control, "status", return_value=fresh),
              mock.patch.object(control, "send") as send,
              mock.patch.object(control, "read_result", side_effect=TimeoutError)):
            result = control.business_action(object(), self.current, "business.pause")
            self.assertEqual(result["state"], "unknown")
            self.assertEqual(result["error_code"], "business_write_receipt_missing")
            send.assert_called_once()


class CommandLineTests(unittest.TestCase):
    def test_old_product_commands_and_joint_arguments_refuse_before_serial_open(self):
        cases = [["product.status"], ["product.install"], ["product.result"],
                 ["ota.start", "--package-mode", "no_package"],
                 ["status", "--operation-id", "44444444-4444-4444-8444-444444444444"],
                 ["business.pause"], ["ota.result"], ["ota.start", "--device-id", "22222222-2222-4222-8222-222222222222",
                  "--operation-id", "44444444-4444-4444-8444-444444444444"]]
        for arguments in cases:
            with (self.subTest(arguments=arguments),
                  mock.patch.object(control.sys, "argv", ["device_control.py", "--port", "test-pty", *arguments]),
                  mock.patch.object(control, "SerialPort") as serial,
                  mock.patch("sys.stderr", new=io.StringIO()), self.assertRaises(SystemExit) as error):
                control.main()
            self.assertEqual(error.exception.code, 2)
            serial.assert_not_called()

    def test_business_status_json_dispatch_and_close(self):
        current = {"device_id": "22222222-2222-4222-8222-222222222222",
                   "boot_id": "33333333-3333-4333-8333-333333333333",
                   "state": "succeeded", "error_code": None, "result": {"uptime_ms": 10}}
        result = {**current, "result": {"byte_count": 5, "state": "paused", "window_deadline_uptime_ms": 0}}
        port = mock.Mock()
        output = io.StringIO()
        with (mock.patch.object(control.sys, "argv", ["device_control.py", "--port", "test-pty", "--json", "business.status"]),
              mock.patch.object(control, "SerialPort", return_value=port),
              mock.patch.object(control, "wait_ready"), mock.patch.object(control, "status", return_value=current),
              mock.patch.object(control, "query_capability", return_value=result) as query,
              mock.patch("sys.stdout", new=output)):
            control.main()
        self.assertEqual(json.loads(output.getvalue()), result)
        query.assert_called_once_with(port, current, "business.status")
        port.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
