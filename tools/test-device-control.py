#!/usr/bin/env python3
"""用真实 POSIX 伪终端验证串口示例的字节传输与背压期限。"""
import importlib.util
import copy
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


if __name__ == "__main__":
    unittest.main()
