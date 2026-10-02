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
    def testBootLocalExactFourFieldsAndFailureCodes(self):
        current = {"device_id": "22222222-2222-4222-8222-222222222222",
                   "boot_id": "33333333-3333-4333-8333-333333333333"}
        operation = "44444444-4444-4444-8444-444444444444"
        for kind in ("stop", "start"):
            result = {"operation_id": operation, "operation_sequence": None,
                      "kind": kind, "container_sequence": 4294967295}
            reply = {**current, "state": "succeeded", "error_code": None, "result": result}
            with (self.subTest(kind=kind), mock.patch.object(control, "send"),
                  mock.patch.object(control, "read_result", side_effect=lambda *args: iter([reply]))):
                self.assertEqual(control.product_result(object(), current, operation)["result"], result)
                for code in ("ota_verification_pending", "operation_busy", "product_precondition_conflict",
                             "product_previous_unresolved", "resource_failure", "product_not_configured"):
                    reply.update(state="failed", error_code=code)
                    self.assertEqual(control.product_result(object(), current, operation)["error_code"], code)
                for code in ("storage_uncertain", "product_state_uncertain"):
                    reply.update(state="unknown", error_code=code)
                    self.assertEqual(control.product_result(object(), current, operation)["error_code"], code)
                reply.update(state="succeeded", error_code=None)
                for missing in result:
                    reply["result"] = {key: value for key, value in result.items() if key != missing}
                    with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, "unknown"):
                        control.product_result(object(), current, operation)

    def testPersistentKindsKeepExactSixFields(self):
        current = {"device_id": "22222222-2222-4222-8222-222222222222",
                   "boot_id": "33333333-3333-4333-8333-333333333333"}
        operation = "44444444-4444-4444-8444-444444444444"
        for kind in ("install", "upgrade", "uninstall"):
            result = {"operation_id": operation, "operation_sequence": 7, "kind": kind,
                      "package_sha256": "ab" * 32, "container_sequence": 12, "result_code": 0}
            reply = {**current, "state": "succeeded", "error_code": None, "result": result}
            with (self.subTest(kind=kind), mock.patch.object(control, "send"),
                  mock.patch.object(control, "read_result", side_effect=lambda *args: iter([reply]))):
                self.assertEqual(control.product_result(object(), current, operation)["result"], result)
                reply["result"] = {**result, "extra": 1}
                with self.assertRaisesRegex(ValueError, "unknown"):
                    control.product_result(object(), current, operation)
                reply["result"] = {key: value for key, value in result.items() if key != "result_code"}
                with self.assertRaisesRegex(ValueError, "unknown"):
                    control.product_result(object(), current, operation)

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
            def __init__(self, watermark=None, pending_id=None, digest=None, changes=None, missing=None):
                self.watermark = watermark
                self.pending_id = pending_id
                self.digest = digest
                self.changes = changes or {}
                self.missing = missing
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
                    "package_sha256": self.digest, "firmware_sha256": "f" * 64,
                    "runtime_guest_abi_version": 2, "package_guest_abi_version": 2 if self.digest else None,
                    "package_data_schema_version": 1 if self.digest else None, "active_product": None}
                if result is not None:
                    result.update(self.changes)
                    if self.missing is not None: result.pop(self.missing)
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

        self.assertEqual(result["firmware_sha256"], "f" * 64)
        self.assertEqual(result["runtime_guest_abi_version"], 2)
        self.assertIsNone(result["package_guest_abi_version"])
        for changes in [{"firmware_sha256": "0" * 64}, {"runtime_guest_abi_version": 0},
                        {"runtime_guest_abi_version": True}, {"package_guest_abi_version": None},
                        {"package_data_schema_version": 0}, {"package_data_schema_version": 4294967296}]:
            with self.assertRaises(ValueError):
                control.product_status(Device(7, digest="ab" * 32, changes=changes), current)
        for missing in ["firmware_sha256", "runtime_guest_abi_version", "package_guest_abi_version", "package_data_schema_version"]:
            with self.assertRaises(ValueError):
                control.product_status(Device(7, missing=missing), current)
        with self.assertRaises(ValueError):
            control.product_status(Device(7, changes={"package_guest_abi_version": 2}), current)

        active = {"product_id": "counter", "product_version": "v" * 3000, "package_sha256": "ab" * 32,
                  "guest_abi_version": 2, "data_schema_version": 1, "is_trial": False, "operation_id": None}
        self.assertEqual(control.product_status(Device(7, digest="ab" * 32, changes={"active_product": active}), current)
                         ["result"]["active_product"]["product_version"], active["product_version"])
        trial = {**active, "is_trial": True, "operation_id": pending, "package_sha256": "cd" * 32}
        self.assertTrue(control.product_status(Device(7, pending, changes={"active_product": trial}), current)
                        ["result"]["active_product"]["is_trial"])
        for bad in [{**active, "is_trial": 1}, {**active, "product_version": "v.1"},
                    {**active, "product_version": "v" * 4096}, {**active, "guest_abi_version": 3},
                    {**active, "package_sha256": "cd" * 32}, {**trial, "operation_id": None},
                    {**active, "operation_id": pending}, {**active, "data_schema_version": 0},
                    {**active, "extra": 1}, *[{k: v for k, v in active.items() if k != missing} for missing in active]]:
            with self.assertRaises(ValueError):
                control.product_status(Device(7, pending, "ab" * 32, {"active_product": bad}), current)
        with self.assertRaises(ValueError):
            control.product_status(Device(7, missing="active_product"), current)


class ProductLifecycleTests(unittest.TestCase):
    device_id = "22222222-2222-4222-8222-222222222222"
    boot_id = "33333333-3333-4333-8333-333333333333"
    operation_id = "44444444-4444-4444-8444-444444444444"
    digest = "ab" * 32

    class Clock:
        def __init__(self):
            self.now = 0

        def monotonic(self):
            self.now += 0.0001
            return self.now

        def sleep(self, duration):
            self.now += duration

    class Device:
        def __init__(self, case, clock, command, query_states=("running", "succeeded"),
                     write_state="running", drop_write=False, write_error=None,
                     binding_changes=None, query_changes=None, fresh_boot=None,
                     query_boot=None, drop_query=False, write_result=None):
            self.case, self.clock, self.command = case, clock, command
            self.query_states = list(query_states)
            self.write_state, self.drop_write, self.write_error = write_state, drop_write, write_error
            self.binding_changes, self.query_changes = binding_changes or {}, query_changes or {}
            self.fresh_boot = fresh_boot
            self.query_boot, self.drop_query, self.write_result = query_boot, drop_query, write_result
            self.requests = []
            self.response = bytearray()

        def write(self, payload):
            request = json.loads(payload.strip())
            self.requests.append(request)
            command = request["command"]
            state, error, result = "succeeded", None, None
            boot = self.case.boot_id
            if command == "product.status":
                result = {"operation_sequence_high_watermark": 4294967295,
                          "next_operation_sequence": None, "pending_operation_id": None,
                          "container_sequence": 4294967295,
                          "package_sha256": self.case.digest, "firmware_sha256": "f" * 64,
                          "runtime_guest_abi_version": 2, "package_guest_abi_version": 2,
                          "package_data_schema_version": 1, "active_product": None,
                          **self.binding_changes}
            elif command == "status":
                result = {"uptime_ms": 2000}
                boot = self.fresh_boot or boot
            elif command == self.command:
                self.write_started = self.clock.now
                if self.drop_write:
                    if self.write_error is not None:
                        raise self.write_error
                    return len(payload)
                state = self.write_state
                result = self.write_result
                error = {"failed": "product_precondition_conflict", "expired": "expired",
                         "unknown": "product_state_uncertain"}.get(state)
            elif command == "product.result":
                self.case.assertEqual(set(request), {"protocol_version", "request_id", "command", "parameters"})
                self.case.assertEqual(request["parameters"], {"operation_id": self.case.operation_id})
                self.case.assertNotEqual(request["request_id"], self.case.operation_id)
                boot = self.query_boot or boot
                if self.drop_query:
                    return len(payload)
                state = self.query_states[0]
                if len(self.query_states) > 1:
                    self.query_states.pop(0)
                result = {"operation_id": self.case.operation_id, "operation_sequence": None,
                          "kind": self.command.split(".")[1], "container_sequence": 4294967295,
                          **self.query_changes}
                error = {"failed": "resource_failure", "unknown": "product_state_uncertain",
                         "expired": "expired"}.get(state)
                if state == "not_found":
                    state, error, result = "unknown", "product_operation_not_found", None
            else:
                raise AssertionError("unexpected command " + command)
            reply = {"protocol_version": 1, "device_id": self.case.device_id,
                     "boot_id": boot, "request_id": request["request_id"],
                     "state": state, "error_code": error, "result": result}
            self.response = bytearray(json.dumps(reply).encode() + b"\n")
            return len(payload)

        def read(self, size=1):
            if not self.response:
                self.clock.now += 0.05
                return b""
            value = bytes(self.response[:size])
            del self.response[:size]
            return value

        def close(self):
            self.closed = True

    def run_case(self, command="product.stop", **options):
        clock = self.Clock()
        device = self.Device(self, clock, command, **options)
        current = {"device_id": self.device_id, "boot_id": self.boot_id}
        with (mock.patch.object(control.time, "monotonic", side_effect=clock.monotonic),
              mock.patch.object(control.time, "sleep", side_effect=clock.sleep)):
            result = control.product_lifecycle(device, current, command, self.operation_id,
                                               4294967295, self.digest)
        return result, device, clock

    def testExactWriteOriginalIdAndReadOnlyPolling(self):
        for command in ("product.stop", "product.start"):
            with self.subTest(command=command):
                result, device, clock = self.run_case(command)
                self.assertEqual(result["state"], "succeeded")
                self.assertIsNone(result["result"]["operation_sequence"])
                self.assertEqual(result["result"]["container_sequence"], 4294967295)
                self.assertEqual([item["command"] for item in device.requests],
                                 ["product.status", "status", command, "product.result", "product.result"])
                self.assertEqual(device.requests[2], {
                    "protocol_version": 1, "request_id": self.operation_id, "command": command,
                    "device_id": self.device_id, "target_boot_id": self.boot_id,
                    "expires_at_uptime_ms": 12000,
                    "parameters": {"expected_container_sequence": 4294967295,
                                   "package_sha256": self.digest}})
                self.assertLess(clock.now, 30)
        confirmed = {"product_id": "counter", "product_version": "v0-1-0", "package_sha256": self.digest,
                     "guest_abi_version": 2, "data_schema_version": 1, "is_trial": False, "operation_id": None}
        self.assertEqual(self.run_case("product.start", binding_changes={"active_product": confirmed})[0]
                         ["state"], "succeeded")

    def testMissingOrUnknownReceiptQueriesWithoutResending(self):
        for options in ({"drop_write": True}, {"drop_write": True, "write_error": TimeoutError("partial")},
                        {"drop_write": True, "write_error": OSError("partial")}, {"write_state": "unknown"},
                        {"write_state": "succeeded"}):
            with self.subTest(options=options):
                result, device, _ = self.run_case(query_states=("succeeded",), **options)
                self.assertEqual(result["state"], "succeeded")
                self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)
                self.assertEqual(device.requests[-1]["command"], "product.result")

    def testUnknownAndRunningHaveOneTotalDeadline(self):
        for state in ("running", "unknown", "not_found"):
            with self.subTest(state=state):
                result, device, clock = self.run_case(query_states=(state,))
                self.assertEqual(result["state"], "unknown")
                self.assertLess(clock.now - device.write_started, 30.1)
                self.assertGreater(sum(item["command"] == "product.result" for item in device.requests), 1)
                self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)
        result, device, clock = self.run_case(drop_query=True)
        self.assertEqual(result["state"], "unknown")
        self.assertLess(clock.now - device.write_started, 30.1)
        self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)

    def testDisconnectedQueryIsUnknownAndCannotResend(self):
        with mock.patch.object(control, "product_result", side_effect=OSError("closed serial")) as query:
            result, device, _ = self.run_case()
        self.assertEqual((result["state"], result["result"]), ("unknown", None))
        self.assertEqual(query.call_count, 1)
        self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)

    def testDefiniteRejectionAndFailedResultNeverRestart(self):
        for state in ("failed", "expired"):
            result, device, _ = self.run_case(write_state=state)
            self.assertEqual(result["state"], state)
            self.assertEqual([item["command"] for item in device.requests],
                             ["product.status", "status", "product.stop"])
        result, device, _ = self.run_case(query_states=("failed",))
        self.assertEqual(result["state"], "failed")
        self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)
        self.assertNotIn("product.start", [item["command"] for item in device.requests])

    def testBindingConflictAndChangedBootRejectBeforeWrite(self):
        trial = {"product_id": "counter", "product_version": "v0-1-0", "package_sha256": "cd" * 32,
                 "guest_abi_version": 2, "data_schema_version": 1, "is_trial": True,
                 "operation_id": self.operation_id}
        variants = [{"package_sha256": "cd" * 32}, {"package_sha256": None,
                    "package_guest_abi_version": None, "package_data_schema_version": None},
                    {"container_sequence": 1}, {"operation_sequence_high_watermark": 7,
                    "next_operation_sequence": 8, "pending_operation_id": self.operation_id},
                    {"operation_sequence_high_watermark": 7, "next_operation_sequence": 8,
                    "pending_operation_id": self.operation_id, "active_product": trial}]
        for changes in variants:
            clock = self.Clock()
            device = self.Device(self, clock, "product.stop", binding_changes=changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                control.product_lifecycle(device, {"device_id": self.device_id, "boot_id": self.boot_id},
                                          "product.stop", self.operation_id, 4294967295, self.digest)
            self.assertFalse(any(item["command"] == "product.stop" for item in device.requests))
        clock = self.Clock()
        device = self.Device(self, clock, "product.start",
                             fresh_boot="55555555-5555-4555-8555-555555555555")
        with self.assertRaises(ValueError):
            control.product_lifecycle(device, {"device_id": self.device_id, "boot_id": self.boot_id},
                                      "product.start", self.operation_id, 4294967295, self.digest)
        self.assertEqual([item["command"] for item in device.requests], ["product.status", "status"])

    def testMalformedOrConflictingResultIsUnknownWithoutResend(self):
        changes = [{"operation_sequence": 1}, {"operation_sequence": False}, {"kind": []},
                   {"kind": "start"}, {"container_sequence": True}, {"container_sequence": 0},
                   {"container_sequence": 4294967296}, {"container_sequence": 1},
                   {"operation_id": "55555555-5555-4555-8555-555555555555"},
                   {"package_sha256": self.digest}, {"result_code": 0}]
        for change in changes:
            clock = self.Clock()
            device = self.Device(self, clock, "product.stop", query_states=("succeeded",), query_changes=change)
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "unknown"):
                control.product_lifecycle(device, {"device_id": self.device_id, "boot_id": self.boot_id},
                                          "product.stop", self.operation_id, 4294967295, self.digest)
            self.assertEqual(sum(item["command"] == "product.stop" for item in device.requests), 1)
        with self.assertRaisesRegex(ValueError, "unknown"):
            self.run_case(query_states=("expired",))
        for options in ({"query_boot": "55555555-5555-4555-8555-555555555555"},
                        {"write_result": {"kind": "stop"}}):
            with self.subTest(options=options), self.assertRaisesRegex(ValueError, "unknown"):
                self.run_case(**options)

    def testInvalidParametersSendNothing(self):
        for sequence, digest in ((True, self.digest), (0, self.digest), (4294967296, self.digest),
                                 (1, "0" * 64), (1, "AB" * 32), (1, None)):
            device = self.Device(self, self.Clock(), "product.stop")
            with self.subTest(sequence=sequence, digest=digest), self.assertRaises(ValueError):
                control.product_lifecycle(device, {"device_id": self.device_id, "boot_id": self.boot_id},
                                          "product.stop", self.operation_id, sequence, digest)
            self.assertEqual(device.requests, [])

    def testCliCompleteParametersAndPureJson(self):
        for command in ("product.stop", "product.start"):
            clock = self.Clock()
            device = self.Device(self, clock, command, query_states=("succeeded",))
            argv = ["device_control.py", "--port", "/dev/unused-test", "--device-id", self.device_id,
                    "--operation-id", self.operation_id, "--expected-container-sequence", "4294967295",
                    "--expected-package-sha256", self.digest, "--json", command]
            output = io.StringIO()
            with (self.subTest(command=command), mock.patch.object(control.sys, "argv", argv),
                  mock.patch.object(control, "SerialPort", return_value=device),
                  mock.patch.object(control, "wait_ready"), mock.patch.object(control.sys, "stdout", output),
                  mock.patch.object(control.time, "monotonic", side_effect=clock.monotonic),
                  mock.patch.object(control.time, "sleep", side_effect=clock.sleep)):
                control.main()
            self.assertEqual(json.loads(output.getvalue())["state"], "succeeded")
            self.assertTrue(device.closed)
            self.assertEqual(sum(item["command"] == command for item in device.requests), 1)

    def testCliRejectsPersistentSequenceOrMissingBindingBeforeOpeningPort(self):
        base = ["device_control.py", "--port", "/dev/unused-test", "--device-id", self.device_id,
                "--operation-id", self.operation_id]
        options = [["--operation-sequence", "1", "--expected-container-sequence", "1",
                    "--expected-package-sha256", self.digest],
                   ["--expected-container-sequence", "1"], ["--expected-package-sha256", self.digest]]
        for command in ("product.stop", "product.start"):
            for extra in options:
                with (self.subTest(command=command, options=extra),
                      mock.patch.object(control.sys, "argv", base + extra + [command]),
                      mock.patch.object(control.sys, "stderr", io.StringIO()),
                      mock.patch.object(control, "SerialPort") as open_port,
                      self.assertRaises(SystemExit) as error):
                    control.main()
                self.assertEqual(error.exception.code, 2)
                open_port.assert_not_called()


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
                "signature": {"scheme": "esp_secure_boot_v2_rsa3072"},
                "package_mode": "no_package"})
            self.assertEqual(pending["state"], "running")
            self.assertIsNone(pending["result"])

            next_boot = "55555555-5555-4555-8555-555555555555"
            final = {**current, "boot_id": next_boot, "state": "succeeded",
                     "error_code": None, "result": {
                         "operation_id": operation,
                         "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "image_size_bytes": len(path.read_bytes()),
                         "target": "esp32c3/esp_base", "target_slot": "ota_1",
                         "package_mode": "no_package", "package_sha256": None}}

            def reply(_port, _request_id, _deadline):
                yield final

            observed.clear()
            with (mock.patch.object(control, "send", side_effect=capture),
                  mock.patch.object(control, "read_result", side_effect=reply)):
                self.assertEqual(control.ota_result(object(), current, operation,
                    final["result"]["sha256"], final["result"]["image_size_bytes"]), final)
            self.assertEqual(observed[0]["command"], "ota.result")
            self.assertEqual(observed[0]["parameters"], {"operation_id": operation})

    def testJointOtaRequestBindsPackageAndRepresentativeEvent(self):
        current = {"device_id": "22222222-2222-4222-8222-222222222222",
                   "boot_id": "33333333-3333-4333-8333-333333333333"}
        operation = "44444444-4444-4444-8444-444444444444"
        fresh = {**current, "result": {"uptime_ms": 1234,
                 "capabilities": {"ota": "busy"}}}
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory, "candidate.bin")
            package = Path(directory, "candidate.pkg")
            event = Path(directory, "event.bin")
            image.write_bytes(b"signed-image-fixture")
            package.write_bytes(b"signed-package-fixture")
            event.write_bytes(b"authorized-event")
            observed = []

            def accepted(_port, request_id, _deadline):
                yield {**current, "request_id": request_id, "state": "failed",
                       "error_code": "product_ota_unavailable", "result": None}

            for mode in ("reuse", "write"):
                observed.clear()
                kwargs = {"package_mode": mode, "package_file": str(package),
                          "guest_abi_version": 2, "data_schema_version": 1,
                          "trial_event_file": str(event)}
                if mode == "write":
                    kwargs["package_url"] = "https://packages.example.test/candidate.pkg"
                binding = {"state": "succeeded", "result": {
                    "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest()}}
                with (mock.patch.object(control, "status", return_value=fresh),
                      mock.patch.object(control, "product_status", return_value=binding),
                      mock.patch.object(control, "send", side_effect=lambda _port, request:
                                        observed.append(request)),
                      mock.patch.object(control, "read_result", side_effect=accepted)):
                    response = control.ota_start(
                        object(), current, operation, str(image),
                        "https://images.example.test/candidate.bin",
                        "esp32c3/esp_base", **kwargs)
                self.assertEqual(response["error_code"], "product_ota_unavailable")
                params = observed[0]["parameters"]
                self.assertEqual(params["package_mode"], mode)
                self.assertEqual(params["package_sha256"],
                                 hashlib.sha256(package.read_bytes()).hexdigest())
                self.assertEqual(params["trial_event_sha256"],
                                 hashlib.sha256(event.read_bytes()).hexdigest())
                self.assertEqual(params["package_size_bytes"], len(package.read_bytes()))
                self.assertEqual(params.get("package_url"), kwargs.get("package_url"))

            with (mock.patch.object(control, "status", return_value=fresh),
                  mock.patch.object(control, "product_status", return_value={
                      "state": "succeeded", "result": {"package_sha256": "aa" * 32}}),
                  mock.patch.object(control, "send") as send):
                with self.assertRaisesRegex(ValueError, "当前确认包"):
                    control.ota_start(object(), current, operation, str(image),
                        "https://images.example.test/candidate.bin",
                        "esp32c3/esp_base", package_mode="reuse",
                        package_file=str(package), guest_abi_version=2,
                        data_schema_version=1, trial_event_file=str(event))
                send.assert_not_called()

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
                            "target_slot": "ota_1", "package_mode": "no_package",
                            "package_sha256": None}}
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

    def testJointOtaResultRequiresPackageEvidence(self):
        current = {"device_id": "22222222-2222-4222-8222-222222222222",
                   "boot_id": "33333333-3333-4333-8333-333333333333"}
        operation = "44444444-4444-4444-8444-444444444444"
        reply = {**current, "state": "succeeded", "error_code": None,
                 "result": {"operation_id": operation, "sha256": "ab" * 32,
                            "image_size_bytes": 512, "target": "esp32c3/esp_base",
                            "target_slot": "ota_1", "package_mode": "write",
                            "package_sha256": "cd" * 32}}

        def response(_port, _request_id, _deadline):
            yield reply

        with (mock.patch.object(control, "send"),
              mock.patch.object(control, "read_result", side_effect=response)):
            self.assertEqual(control.ota_result(
                object(), current, operation, "ab" * 32, 512, "write", "cd" * 32),
                reply)
            with self.assertRaisesRegex(ValueError, "与请求不符"):
                control.ota_result(object(), current, operation,
                                   "ab" * 32, 512, "write", "ef" * 32)
            reply["result"]["package_sha256"] = None
            with self.assertRaisesRegex(ValueError, "与请求不符"):
                control.ota_result(object(), current, operation)
            reply["result"]["package_mode"] = []
            with self.assertRaisesRegex(ValueError, "与请求不符"):
                control.ota_result(object(), current, operation)


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
                              "package_sha256": digest, "firmware_sha256": "f" * 64,
                              "runtime_guest_abi_version": 2, "package_guest_abi_version": 2,
                              "package_data_schema_version": 1, "active_product": None}
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
