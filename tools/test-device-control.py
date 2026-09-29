#!/usr/bin/env python3
"""用真实 POSIX 伪终端验证串口示例的字节传输与背压期限。"""
import importlib.util
import copy
import hashlib
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

spec = importlib.util.spec_from_file_location("device_control", Path(__file__).with_name("device-control.py"))
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

    def testRawDuplexPreservesBytesAndDisablesHangup(self):
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

    def testBackpressureEndsAsUnknownWithinBound(self):
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

    def testCompleteV3AndNullCapabilities(self):
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

    def testV1AndMalformedFieldsAreRejected(self):
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

    def testMaxLengthAndFrameLimit(self):
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


class ProductResultTests(unittest.TestCase):
    def testOriginalIdQueryAndEvictedUnknown(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        operation = "44444444-4444-4444-8444-444444444444"

        class Device:
            def __init__(self, known):
                self.known = known
                self.response = bytearray()

            def write(self, payload):
                request = json.loads(payload.strip())
                assert request["command"] == "product.result"
                assert request["parameters"] == {"operation_id": operation}
                result = ({"operation_id": operation, "operation_sequence": 7,
                           "container_sequence": 12, "result_code": 0,
                           "kind": "install", "package_sha256": "ab" * 32}
                          if self.known else None)
                response = {"protocol_version": 1, "device_id": device,
                            "boot_id": boot, "request_id": request["request_id"],
                            "state": "succeeded" if self.known else "unknown",
                            "error_code": None if self.known else "product_operation_not_found",
                            "result": result}
                self.response = bytearray(b"\n" + json.dumps(response).encode() + b"\n")
                return len(payload)

            def read(self, size=1):
                if not self.response:
                    return b""
                value = bytes(self.response[:size])
                del self.response[:size]
                return value

        current = {"device_id": device, "boot_id": boot}
        self.assertEqual(control.product_result(Device(True), current, operation)["state"], "succeeded")
        unknown = control.product_result(Device(False), current, operation)
        self.assertEqual((unknown["state"], unknown["error_code"]),
                         ("unknown", "product_operation_not_found"))


class ProductStatusTests(unittest.TestCase):
    def testPersistentSequenceAndUninitialized(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        pending = "44444444-4444-4444-8444-444444444444"

        class Device:
            def __init__(self, watermark=None, pending_id=None, digest=None):
                self.watermark = watermark
                self.pending_id = pending_id
                self.digest = digest
                self.response = bytearray()

            def write(self, payload):
                request = json.loads(payload.strip())
                assert set(request) == {"protocol_version", "request_id", "command"}
                assert request["command"] == "product.status"
                result = None if self.watermark is None else {
                    "operation_sequence_high_watermark": self.watermark,
                    "next_operation_sequence": self.watermark + 1 if self.watermark < 4294967295 else None,
                    "pending_operation_id": self.pending_id,
                    "container_sequence": 6,
                    "package_sha256": self.digest}
                response = {"protocol_version": 1, "device_id": device,
                            "boot_id": boot, "request_id": request["request_id"],
                            "state": "unknown" if result is None else "succeeded",
                            "error_code": "product_ledger_uninitialized" if result is None else None,
                            "result": result}
                self.response = bytearray(b"\n" + json.dumps(response).encode() + b"\n")
                return len(payload)

            def read(self, size=1):
                if not self.response:
                    return b""
                value = bytes(self.response[:size])
                del self.response[:size]
                return value

        current = {"device_id": device, "boot_id": boot}
        self.assertEqual(control.product_status(Device(), current)["error_code"],
                         "product_ledger_uninitialized")
        result = control.product_status(Device(7, pending), current)["result"]
        self.assertEqual(result["next_operation_sequence"], 8)
        self.assertEqual(result["pending_operation_id"], pending)
        self.assertEqual(result["container_sequence"], 6)
        self.assertIsNone(result["package_sha256"])
        self.assertEqual(control.product_status(Device(7, digest="ab" * 32), current)
                         ["result"]["package_sha256"], "ab" * 32)
        with self.assertRaises(ValueError):
            control.product_status(Device(7, digest="0" * 64), current)
        self.assertIsNone(control.product_status(Device(4294967295), current)
                          ["result"]["next_operation_sequence"])


class ProductPackageTests(unittest.TestCase):
    def test_signed_bytes_bind_one_write_and_original_id_query(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        operation = "44444444-4444-4444-8444-444444444444"
        current = {"device_id": device, "boot_id": boot}
        package = b"signed-package-fixture" * 19
        digest = hashlib.sha256(package).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "candidate.pkg")
            path.write_bytes(package)
            event_path = Path(directory, "trial_event.bin")
            event_path.write_bytes(b"authorized-business-event")
            snapshot = {"state": "succeeded", "result": {
                "next_operation_sequence": 7, "container_sequence": 12,
                "package_sha256": None, "pending_operation_id": None}}
            fresh = {**current, "result": {"uptime_ms": 1234}}
            seen = []

            def capture(_port, request):
                seen.append(request)

            def receipt(_port, request_id, _deadline):
                yield {**current, "request_id": request_id, "state": "running",
                       "error_code": None, "result": None}

            unresolved = {**current, "state": "unknown",
                          "error_code": "product_operation_unresolved", "result": {
                              "operation_id": operation, "operation_sequence": 7,
                              "container_sequence": 13, "kind": "install",
                              "package_sha256": digest}}
            with (mock.patch.object(control, "product_status", return_value=snapshot),
                  mock.patch.object(control, "status", return_value=fresh),
                  mock.patch.object(control, "send", side_effect=capture),
                  mock.patch.object(control, "read_result", side_effect=receipt),
                  mock.patch.object(control, "product_result", return_value=unresolved) as query):
                result = control.product_package(object(), current, "product.install",
                    operation, 7, 12, None, str(path),
                    "https://packages.example.test/candidate.pkg", 2, 1,
                    str(event_path))
            self.assertEqual(result, unresolved)
            query.assert_called_once()
            self.assertEqual(len(seen), 1)
            self.assertEqual(seen[0]["command"], "product.install")
            self.assertEqual(seen[0]["target_boot_id"], boot)
            self.assertEqual(seen[0]["parameters"], {
                "operation_id": operation, "operation_sequence": 7,
                "expected_container_sequence": 12,
                "previous_package_sha256": None,
                "package_url": "https://packages.example.test/candidate.pkg",
                "package_sha256": digest,
                "trial_event_sha256": hashlib.sha256(event_path.read_bytes()).hexdigest(),
                "package_size_bytes": len(package),
                "guest_abi_version": 2, "data_schema_version": 1})

    def test_changed_binding_blocks_write(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        current = {"device_id": device, "boot_id": boot}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "candidate.pkg")
            path.write_bytes(b"signed-package-fixture")
            event_path = Path(directory, "trial_event.bin")
            event_path.write_bytes(b"authorized-business-event")
            snapshot = {"state": "succeeded", "result": {
                "next_operation_sequence": 8, "container_sequence": 12,
                "package_sha256": None, "pending_operation_id": None}}
            with (mock.patch.object(control, "product_status", return_value=snapshot),
                  mock.patch.object(control, "send") as send):
                with self.assertRaisesRegex(ValueError, "预期不符"):
                    control.product_package(object(), current, "product.install",
                        "44444444-4444-4444-8444-444444444444", 7, 12, None,
                        str(path), "https://packages.example.test/candidate.pkg", 2, 1,
                        str(event_path))
                send.assert_not_called()


class OtaClientTests(unittest.TestCase):
    def testSignedImageRequestAndOriginalIdQuery(self):
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

    def testNoWriteForWrongBootOrOversizedImage(self):
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
                output.truncate(0x130000 + 1)
            with mock.patch.object(control, "send") as send:
                with self.assertRaisesRegex(ValueError, "目标槽"):
                    control.ota_start(object(), current, operation, str(path),
                                      "https://images.example.test/candidate.bin",
                                      "esp32c3/esp_base")
                send.assert_not_called()

    def testQueryRejectsDifferentImageAndNeverWritesAgain(self):
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


class ProductUninstallTests(unittest.TestCase):
    def testExactBindingWriteAndOriginalIdResult(self):
        device = "22222222-2222-4222-8222-222222222222"
        boot = "33333333-3333-4333-8333-333333333333"
        operation = "44444444-4444-4444-8444-444444444444"
        digest = "ab" * 32

        class Device:
            def __init__(self, write_state="succeeded"):
                self.write_state = write_state
                self.commands = []
                self.response = bytearray()

            def write(self, payload):
                request = json.loads(payload.strip())
                command = request["command"]
                self.commands.append(command)
                result = None
                state = "succeeded"
                error = None
                if command == "status":
                    result = {"uptime_ms": 1000}
                elif command == "product.status":
                    result = {"operation_sequence_high_watermark": 0,
                              "next_operation_sequence": 1,
                              "pending_operation_id": None,
                              "container_sequence": 6,
                              "package_sha256": digest}
                elif command == "product.uninstall":
                    assert request["device_id"] == device
                    assert request["target_boot_id"] == boot
                    assert request["expires_at_uptime_ms"] == 11000
                    assert request["parameters"] == {
                        "operation_id": operation, "operation_sequence": 1,
                        "expected_container_sequence": 6, "package_sha256": digest}
                    state = self.write_state
                    error = "storage_uncertain" if state == "unknown" else None
                elif command == "product.result":
                    assert request["parameters"] == {"operation_id": operation}
                    result = {"operation_id": operation, "operation_sequence": 1,
                              "container_sequence": 7 if self.write_state in {"succeeded", "timeout"} else 6,
                              "result_code": 0, "kind": "uninstall",
                              "package_sha256": digest}
                    if self.write_state == "unknown":
                        state = "unknown"
                        error = "product_operation_unresolved"
                else:
                    raise AssertionError(command)
                response = {"protocol_version": 1, "device_id": device,
                            "boot_id": boot, "request_id": request["request_id"],
                            "state": state, "error_code": error, "result": result}
                self.response = bytearray(b"\n" + json.dumps(response).encode() + b"\n")
                return len(payload)

            def read(self, size=1):
                if not self.response:
                    return b""
                value = bytes(self.response[:size])
                del self.response[:size]
                return value

        current = {"device_id": device, "boot_id": boot}
        port = Device()
        result = control.product_uninstall(port, current, operation, 1, 6, digest)
        self.assertEqual((result["state"], result["result"]["kind"]),
                         ("succeeded", "uninstall"))
        self.assertEqual(port.commands,
                         ["product.status", "status", "product.uninstall", "product.result"])
        port = Device()
        with self.assertRaisesRegex(ValueError, "未发送命令"):
            control.product_uninstall(port, current, operation, 1, 6, "cd" * 32)
        self.assertEqual(port.commands, ["product.status"])
        port = Device("unknown")
        result = control.product_uninstall(port, current, operation, 1, 6, digest)
        self.assertEqual((result["state"], result["error_code"]),
                         ("unknown", "product_operation_unresolved"))
        self.assertEqual(port.commands.count("product.uninstall"), 1)
        port = Device("timeout")
        real_read_result = control.read_result

        def timeout_uninstall_result(port_arg, request_id, deadline):
            if port_arg.commands[-1] == "product.uninstall":
                raise TimeoutError("卸载回执超时")
            yield from real_read_result(port_arg, request_id, deadline)

        with mock.patch.object(control, "read_result", timeout_uninstall_result):
            result = control.product_uninstall(port, current, operation, 1, 6, digest)
        self.assertEqual((result["state"], result["result"]["container_sequence"]),
                         ("succeeded", 7))
        self.assertEqual(port.commands,
                         ["product.status", "status", "product.uninstall", "product.result"])


if __name__ == "__main__":
    unittest.main()
