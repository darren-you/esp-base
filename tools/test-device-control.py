#!/usr/bin/env python3
"""用真实 POSIX 伪终端验证串口示例的字节传输与背压期限。"""
import importlib.util
import copy
import json
import os
from pathlib import Path
import pty
import select
import termios
import time
import unittest

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
            def __init__(self, watermark=None, pending_id=None):
                self.watermark = watermark
                self.pending_id = pending_id
                self.response = bytearray()

            def write(self, payload):
                request = json.loads(payload.strip())
                assert set(request) == {"protocol_version", "request_id", "command"}
                assert request["command"] == "product.status"
                result = None if self.watermark is None else {
                    "operation_sequence_high_watermark": self.watermark,
                    "next_operation_sequence": self.watermark + 1 if self.watermark < 4294967295 else None,
                    "pending_operation_id": self.pending_id}
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
        self.assertIsNone(control.product_status(Device(4294967295), current)
                          ["result"]["next_operation_sequence"])


if __name__ == "__main__":
    unittest.main()
