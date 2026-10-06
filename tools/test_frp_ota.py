"""真实宿主 HTTP/HMAC 字节回归；不访问 FRPS 或设备。"""
import hashlib
import hmac
import json
import socket
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

import frp_ota

DEVICE = "22222222-2222-4222-8222-222222222222"
BOOT = "33333333-3333-4333-8333-333333333333"
NEXT_BOOT = "44444444-4444-4444-8444-444444444444"
OPERATION = "11111111-1111-4111-8111-111111111111"
KEY = bytes(range(32))


class Fixture:
    def __init__(self):
        self.requests = []
        self.uploads = []
        self.query_count = 0
        self.error = None
        self.bad_response_tag = False
        self.wrong_device = False
        self.bad_result_sha = False
        self.lost_upload_response = False
        self.reject_submit = False
        self.never_finishes = False
        self.extra_result_key = False
        self.upload_state = "running"
        self.upload_error = None
        self.final_state = "succeeded"
        self.final_boot = NEXT_BOOT
        self.firmware_change = {}
        self.firmware_boot = NEXT_BOOT
        self.final_error = None
        self.start_request = None
        self.final_digest = None
        self.final_size = None
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def reply(self, value, code=200):
                payload = frp_ota.wire_json(value)
                tag = hmac.digest(KEY, payload, "sha256").hex()
                if fixture.bad_response_tag:
                    tag = "0" * 64
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("X-ESP-Management-Tag", tag)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(payload)

            def envelope(self, request, state="succeeded", result=None, error=None, boot=BOOT):
                return {"protocol_version": 1,
                    "device_id": NEXT_BOOT if fixture.wrong_device else DEVICE,
                    "boot_id": boot, "request_id": request["request_id"],
                    "state": state, "error_code": error, "result": result}

            def do_POST(self):
                try:
                    size = int(self.headers["Content-Length"])
                    payload = self.rfile.read(size)
                    if not hmac.compare_digest(self.headers["X-ESP-Management-Tag"],
                            hmac.digest(KEY, payload, "sha256").hex()):
                        raise AssertionError("client command HMAC")
                    request = json.loads(payload)
                    fixture.requests.append((self.path, request, payload))
                    if request["device_id"] != DEVICE:
                        raise AssertionError("client device")
                    command = request["command"]
                    if command == "status":
                        self.reply(self.envelope(request, result={"uptime_ms": 1234,
                            "capabilities": {"ota": "ready"}}))
                    elif command == "ota.start":
                        if self.path != "/api/v1/commands/ota-start":
                            raise AssertionError("start path")
                        if set(request) != {"protocol_version", "device_id", "request_id", "command",
                                "target_boot_id", "expires_at_uptime_ms", "parameters"}:
                            raise AssertionError("start envelope")
                        if set(request["parameters"]) != {
                                "operation_id", "sha256", "image_size_bytes", "target", "signature"}:
                            raise AssertionError("must be inbound without URL")
                        if request["target_boot_id"] != BOOT or request["expires_at_uptime_ms"] != 11234:
                            raise AssertionError("boot deadline")
                        fixture.start_request = request
                        if fixture.reject_submit:
                            self.reply(self.envelope(request, state="failed", error="operation_busy"), 409)
                        else:
                            self.reply(self.envelope(request, state="running"), 202)
                    elif command == "ota.result":
                        fixture.query_count += 1
                        if request["parameters"] != {"operation_id": OPERATION}:
                            raise AssertionError("original operation only")
                        parameters = fixture.start_request["parameters"]
                        result = {"operation_id": OPERATION,
                            "sha256": "0" * 64 if fixture.bad_result_sha else parameters["sha256"],
                            "image_size_bytes": parameters["image_size_bytes"],
                            "target": parameters["target"], "target_slot": "ota_1"}
                        if fixture.extra_result_key:
                            result["package_digest"] = "old"
                        if fixture.final_state == "unknown":
                            state, result = "unknown", None
                        else:
                            state = "running" if fixture.never_finishes or fixture.query_count == 1 else fixture.final_state
                        error = fixture.final_error if state != "running" else None
                        code = 202 if state == "running" else 409 if state in {"failed", "expired"} else 200
                        self.reply(self.envelope(request, state=state, result=result, error=error,
                            boot=fixture.final_boot), code)
                    elif command == "firmware.status":
                        parameters = fixture.start_request["parameters"] if fixture.start_request else {
                            "sha256": "a" * 64, "image_size_bytes": 4096, "target": "esp32c3/esp_base"}
                        result = {"firmware_sha256": parameters["sha256"],
                            "image_size_bytes": parameters["image_size_bytes"],
                            "target": parameters["target"], "ota_slot": "ota_1"}
                        result.update(fixture.firmware_change)
                        self.reply(self.envelope(request, result=result, boot=fixture.firmware_boot))
                    elif command == "business.status":
                        self.reply(self.envelope(request, result={}))
                    elif command in {"business.pause", "business.resume"}:
                        if request["parameters"] != {} or request["target_boot_id"] != BOOT:
                            raise AssertionError("business write guard")
                        self.reply(self.envelope(request, result={}))
                    else:
                        raise AssertionError("unexpected command")
                except Exception as error:
                    fixture.error = error
                    self.close_connection = True

            def do_PUT(self):
                try:
                    request = fixture.start_request
                    parameters = request["parameters"]
                    size = int(self.headers["Content-Length"])
                    if size != parameters["image_size_bytes"]:
                        raise AssertionError("exact signed length")
                    metadata = frp_ota.upload_metadata(OPERATION, DEVICE, BOOT,
                        size, parameters["sha256"])
                    if self.path != "/api/v1/ota-images/" + OPERATION or (
                            self.headers["Content-Type"] != "application/octet-stream") or (
                            self.headers.get("Transfer-Encoding") is not None) or (
                            not hmac.compare_digest(self.headers["X-ESP-Management-Tag"],
                                hmac.digest(KEY, metadata, "sha256").hex())):
                        raise AssertionError("upload framing/HMAC")
                    payload = bytearray()
                    while len(payload) < size:
                        block = self.rfile.read(min(511, size - len(payload)))
                        if not block:
                            raise AssertionError("upload truncated")
                        payload.extend(block)
                    fixture.uploads.append(bytes(payload))
                    if hashlib.sha256(payload).hexdigest() != parameters["sha256"]:
                        raise AssertionError("complete signed digest")
                    if fixture.lost_upload_response:
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.close_connection = True
                    else:
                        state = fixture.upload_state
                        code = 202 if state == "running" else 409 if state in {"failed", "expired"} else 200
                        self.reply(self.envelope(request, state=state, error=fixture.upload_error), code)
                except Exception as error:
                    fixture.error = error
                    self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = frp_ota.FrpClient(f"http://127.0.0.1:{self.server.server_port}", DEVICE, KEY)

    def close(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()


class FrpOtaTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.image = Path(self.directory.name) / "signed_fixture.bin"
        # Opaque bytes test transport/HMAC identity only; no signature claim.
        self.payload = bytes(range(256)) * 37
        self.image.write_bytes(self.payload)

    def upload(self, **kwargs):
        return self.fixture.client.ota_start(OPERATION, self.image, "esp32c3/esp_base",
            result_timeout_seconds=kwargs.pop("result_timeout_seconds", 2),
            poll_interval_seconds=0.005, **kwargs)

    def test_complete_firmware_uses_frp_body_and_durable_result(self):
        progress = []
        final = self.upload(progress=lambda received, total: progress.append((received, total)))
        self.assertEqual(final["state"], "succeeded")
        self.assertEqual(final["boot_id"], NEXT_BOOT)
        self.assertEqual(self.fixture.uploads, [self.payload])
        self.assertEqual(len(progress), 3)
        self.assertEqual(progress[-1], (len(self.payload), len(self.payload)))
        starts = [value for _, value, _ in self.fixture.requests if value["command"] == "ota.start"]
        self.assertEqual(len(starts), 1)
        self.assertNotIn("image_url", starts[0]["parameters"])
        self.assertIsNone(self.fixture.error)

    def test_localhost_resolver_process_completes_real_http_original_id_flow(self):
        self.fixture.client = frp_ota.FrpClient(
            f"http://localhost:{self.fixture.server.server_port}", DEVICE, KEY)
        processes = []
        real_popen = frp_ota.subprocess.Popen

        def spawn(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            processes.append(process)
            return process

        with patch("frp_ota.subprocess.Popen", side_effect=spawn):
            final = self.upload()
        self.assertEqual(final["state"], "succeeded")
        self.assertEqual(final["result"]["operation_id"], OPERATION)
        self.assertEqual(self.fixture.uploads, [self.payload])
        self.assertEqual(sum(req["command"] == "ota.start"
            for _, req, _ in self.fixture.requests), 1)
        self.assertGreaterEqual(len(processes), 4)
        for process in processes:
            self.assertEqual(process.returncode, 0)
            with self.assertRaises(ChildProcessError):
                frp_ota.os.waitpid(process.pid, frp_ota.os.WNOHANG)
        self.assertIsNone(self.fixture.error)

    def test_lost_upload_response_only_queries_original_id(self):
        self.fixture.lost_upload_response = True
        final = self.upload()
        self.assertEqual(final["state"], "succeeded")
        self.assertEqual(len(self.fixture.uploads), 1)
        self.assertEqual(sum(req["command"] == "ota.start" for _, req, _ in self.fixture.requests), 1)
        self.assertGreaterEqual(self.fixture.query_count, 2)
        self.assertIsNone(self.fixture.error)

    def test_upload_unknown_is_reconciled_to_original_durable_failure(self):
        self.fixture.upload_state = "unknown"
        self.fixture.upload_error = "storage_uncertain"
        self.fixture.final_state = "failed"
        self.fixture.final_error = "ota_update_failed"
        final = self.upload()
        self.assertEqual(final["state"], "failed")
        self.assertEqual(final["error_code"], "ota_update_failed")
        self.assertEqual(final["boot_id"], NEXT_BOOT)
        self.assertEqual(final["result"]["operation_id"], OPERATION)
        self.assertEqual(self.fixture.uploads, [self.payload])
        self.assertEqual(sum(req["command"] == "ota.start" for _, req, _ in self.fixture.requests), 1)
        self.assertGreaterEqual(self.fixture.query_count, 2)
        self.assertIsNone(self.fixture.error)

    def test_uncertain_failure_persistence_stays_unknown_without_reupload(self):
        self.fixture.upload_state = "unknown"
        self.fixture.upload_error = "storage_uncertain"
        self.fixture.final_state = "unknown"
        self.fixture.final_error = "storage_uncertain"
        final = self.upload()
        self.assertEqual(final["state"], "unknown")
        self.assertEqual(final["error_code"], "storage_uncertain")
        self.assertEqual(final["boot_id"], NEXT_BOOT)
        self.assertIsNone(final["result"])
        self.assertEqual(self.fixture.uploads, [self.payload])
        self.assertEqual(sum(req["command"] == "ota.start" for _, req, _ in self.fixture.requests), 1)
        self.assertEqual(self.fixture.query_count, 1)
        self.assertIsNone(self.fixture.error)

    def test_upload_terminal_claims_only_use_original_result_query(self):
        for state in ("failed", "expired", "succeeded"):
            with self.subTest(state=state):
                self.fixture.upload_state = state
                self.fixture.upload_error = "upload_stage_only" if state != "succeeded" else None
                self.fixture.requests.clear()
                self.fixture.uploads.clear()
                self.fixture.query_count = 0
                final = self.upload()
                self.assertEqual(final["state"], "succeeded")
                self.assertEqual(final["boot_id"], NEXT_BOOT)
                self.assertEqual(final["result"]["operation_id"], OPERATION)
                self.assertEqual(self.fixture.uploads, [self.payload])
                self.assertEqual(sum(req["command"] == "ota.start" for _, req, _ in self.fixture.requests), 1)
                self.assertGreaterEqual(self.fixture.query_count, 2)
                self.assertIsNone(self.fixture.error)

    def test_unknown_has_no_repeat_write(self):
        self.fixture.never_finishes = True
        with self.assertRaises(frp_ota.UnknownOperation) as caught:
            self.upload(result_timeout_seconds=0.03)
        self.assertEqual(caught.exception.operation_id, OPERATION)
        self.assertEqual(len(self.fixture.uploads), 1)
        self.assertEqual(sum(req["command"] == "ota.start" for _, req, _ in self.fixture.requests), 1)

    def test_busy_submit_has_no_upload(self):
        self.fixture.reject_submit = True
        final = self.upload()
        self.assertEqual(final["state"], "failed")
        self.assertEqual(final["error_code"], "operation_busy")
        self.assertFalse(self.fixture.uploads)
        self.assertEqual(self.fixture.query_count, 0)

    def test_response_hmac_or_wrong_device_blocks_write(self):
        for field in ("bad_response_tag", "wrong_device"):
            with self.subTest(field=field):
                setattr(self.fixture, field, True)
                with self.assertRaises(ValueError):
                    self.upload()
                self.assertFalse(self.fixture.uploads)
                self.assertFalse(any(req["command"] == "ota.start" for _, req, _ in self.fixture.requests))
                setattr(self.fixture, field, False)

    def test_wrong_digest_or_old_result_fields_do_not_report_success(self):
        for field in ("bad_result_sha", "extra_result_key"):
            with self.subTest(field=field):
                setattr(self.fixture, field, True)
                with self.assertRaises(frp_ota.UnknownOperation):
                    self.upload()
                setattr(self.fixture, field, False)

    def test_same_boot_success_is_unknown_and_never_resubmitted(self):
        self.fixture.final_boot = BOOT
        with self.assertRaises(frp_ota.UnknownOperation):
            self.upload()
        self.assertEqual(sum(req["command"] == "ota.start"
            for _, req, _ in self.fixture.requests), 1)
        self.assertEqual(len(self.fixture.uploads), 1)

    def test_durable_success_requires_independent_running_firmware(self):
        for change in ({"firmware_sha256": "f" * 64}, {"image_size_bytes": 1},
                {"target": "esp32/esp_base"}, {"ota_slot": "ota_0"}, {"extra": 1}):
            with self.subTest(change=change):
                self.fixture.firmware_change = change
                with self.assertRaises(frp_ota.UnknownOperation):
                    self.upload()
                self.fixture.query_count = 0
                self.fixture.requests.clear()
                self.fixture.uploads.clear()
        self.fixture.firmware_change = {}
        self.fixture.firmware_boot = BOOT
        with self.assertRaises(frp_ota.UnknownOperation):
            self.upload()

    def test_result_query_late_success_cannot_extend_original_deadline(self):
        clock = [0.0]
        self.fixture.client.clock = lambda: clock[0]
        captured = []
        def late_result(operation_id, **kwargs):
            captured.append(kwargs["deadline"])
            clock[0] += 4
            return {"protocol_version": 1, "device_id": DEVICE, "boot_id": NEXT_BOOT,
                "request_id": "55555555-5555-4555-8555-555555555555",
                "state": "succeeded", "error_code": None, "result": {
                    "operation_id": operation_id, "sha256": hashlib.sha256(self.payload).hexdigest(),
                    "image_size_bytes": len(self.payload), "target": "esp32c3/esp_base",
                    "target_slot": "ota_1"}}
        with patch.object(self.fixture.client, "result", side_effect=late_result):
            with self.assertRaises(frp_ota.UnknownOperation):
                self.upload(result_timeout_seconds=1)
        self.assertEqual(captured, [1.0])
        self.assertEqual(sum(req["command"] == "ota.start"
            for _, req, _ in self.fixture.requests), 1)
        self.assertFalse(any(req["command"] == "firmware.status"
            for _, req, _ in self.fixture.requests))

    def test_expired_read_deadline_never_opens_a_connection(self):
        self.fixture.client.clock = lambda: 5.0
        with patch.object(self.fixture.client, "_open") as opening:
            with self.assertRaises(TimeoutError):
                self.fixture.client.result(OPERATION, deadline=5.0)
            with self.assertRaises(TimeoutError):
                self.fixture.client.firmware_status(deadline=4.0)
        opening.assert_not_called()

    def test_running_firmware_strict_identity_rejects_invalid_types_and_geometry(self):
        for change in ({"image_size_bytes": True}, {"image_size_bytes": 0},
                {"image_size_bytes": 0x1e0001}, {"target": "esp32s3/esp_base"},
                {"ota_slot": "factory"}, {"firmware_sha256": "0" * 64},
                {"firmware_sha256": "A" * 64}, {"extra": 1}):
            with self.subTest(change=change):
                self.fixture.firmware_change = change
                with self.assertRaises(ValueError):
                    self.fixture.client.firmware_status()
        self.fixture.firmware_change = {}
        self.assertEqual(self.fixture.client.firmware_status()["result"]["ota_slot"], "ota_1")

    def test_small_or_oversized_image_never_opens_command(self):
        for size in (1, 0x1e0001):
            self.image.write_bytes(b"X" * size)
            with self.assertRaises(ValueError):
                self.upload()
        self.assertFalse(self.fixture.requests)

    def test_business_and_firmware_command_paths(self):
        self.fixture.client.command("firmware.status")
        self.fixture.client.command("business.status")
        current = self.fixture.client.status()
        for command in ("business.pause", "business.resume"):
            self.fixture.client.command(command, parameters={}, current=current)
        self.assertIsNone(self.fixture.error)
        paths = [path for path, _, _ in self.fixture.requests]
        self.assertIn("/api/v1/commands/business-pause", paths)
        self.assertIn("/api/v1/commands/firmware-status", paths)

    def test_metadata_is_canonical_and_binds_device_boot_and_size(self):
        digest = hashlib.sha256(self.payload).hexdigest()
        expected = f"esp-base-ota-upload-v1\n{OPERATION}\n{DEVICE}\n{BOOT}\n9472\n{digest}\n".encode()
        self.assertEqual(frp_ota.upload_metadata(OPERATION, DEVICE, BOOT, len(self.payload), digest), expected)
        for arguments in ((OPERATION.upper(), DEVICE, BOOT, 9472, digest),
                          (OPERATION, DEVICE, BOOT, 0, digest),
                          (OPERATION, DEVICE, BOOT, 9472, digest.upper())):
            # Digits-only OPERATION.upper() is identical; use an invalid UUID.
            if arguments[0] == OPERATION and arguments[3] == 9472 and arguments[4] == digest:
                arguments = ("bad", *arguments[1:])
            with self.assertRaises(ValueError):
                frp_ota.upload_metadata(*arguments)

    def test_endpoint_and_key_reject_unbound_or_insecure_options(self):
        for endpoint in ("ftp://example.invalid", "http://user@example.invalid", "http://example.invalid/path",
                         "http://example.invalid/?key=secret"):
            with self.assertRaises(ValueError):
                frp_ota.FrpClient(endpoint, DEVICE, KEY)
        with self.assertRaises(ValueError):
            frp_ota.FrpClient("http://example.invalid", DEVICE, bytes(32))


class ByteSocket:
    def __init__(self, payload):
        self.payload = bytearray(payload)
        self.timeouts = []

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def recv(self, size):
        block = bytes(self.payload[:size])
        del self.payload[:size]
        return block


class FramingTests(unittest.TestCase):
    def setUp(self):
        self.client = frp_ota.FrpClient("http://example.invalid", DEVICE, KEY, clock=lambda: 0)

    def response(self, body, *, extra="", length=None, tag=None):
        if tag is None:
            tag = hmac.digest(KEY, body, "sha256").hex()
        return (f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                f"Content-Length: {len(body) if length is None else length}\r\n"
                f"X-ESP-Management-Tag: {tag}\r\n{extra}\r\n").encode() + body

    def test_authenticated_response_exact_bytes_and_budget(self):
        body = b'{"hello":"world"}'
        source = ByteSocket(self.response(body))
        self.assertEqual(self.client._response(source, 2), {"hello": "world"})
        self.assertTrue(all(0 < timeout <= 1 for timeout in source.timeouts))

    def test_duplicate_framing_chunked_missing_tag_and_duplicate_json(self):
        body = b'{"hello":"world"}'
        frames = [self.response(body, extra="Content-Length: 17\r\n"),
                  self.response(body, extra="Transfer-Encoding: chunked\r\n"),
                  self.response(body, length=8193), self.response(body, tag="bad"),
                  self.response(b'{"same":1,"same":2}'), self.response(b'{"value":NaN}'),
                  self.response(body)[:-1]]
        for frame in frames:
            with self.subTest(frame=frame[:60]):
                with self.assertRaises((ValueError, OSError)):
                    self.client._response(ByteSocket(frame), 2)

    def test_send_timeout_does_not_extend_idle_or_total(self):
        class Clock:
            now = 0

            def __call__(self):
                return self.now

        class DelayedSocket:
            def __init__(self, clock, delay, count=1):
                self.clock, self.delay, self.count = clock, delay, count
                self.timeouts = []

            def settimeout(self, timeout):
                self.timeouts.append(timeout)

            def send(self, payload):
                self.clock.now += self.delay
                return min(self.count, len(payload))

        for delay, total, idle in ((1, 300, 30), (30, 300, 30), (300, 300, 30)):
            clock = Clock()
            client = frp_ota.FrpClient("http://example.invalid", DEVICE, KEY, clock=clock)
            source = DelayedSocket(clock, delay)
            with self.assertRaises(TimeoutError):
                client._send(source, b"firmware", total, idle)
            self.assertEqual(len(source.timeouts), 1)
            self.assertLessEqual(source.timeouts[0], 1)
        clock = Clock()
        client = frp_ota.FrpClient("http://example.invalid", DEVICE, KEY, clock=clock)
        source = DelayedSocket(clock, .9)
        with self.assertRaises(TimeoutError):
            client._send(source, b"X" * 400, 300, 30)
        self.assertGreaterEqual(clock.now, 300)
        self.assertLess(source.timeouts[-1], 1)


class ConnectionDeadlineTests(unittest.TestCase):
    """可控 socket 和有限解析子进程；不发 DNS、TCP 或 TLS 流量。"""

    addresses = [(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "",
        ("127.0.0.1", port)) for port in (8001, 8002, 8003)]

    def setUp(self):
        self.now = 0.0
        self.connections = []
        self.client = frp_ota.FrpClient("http://deadline.invalid:8000", DEVICE, KEY,
            clock=lambda: self.now)

    def socket_factory(self, *, delay, failure=False):
        test = self

        class Socket:
            closed = False

            def __init__(self, *_):
                self.timeouts = []
                test.connections.append(self)

            def settimeout(self, value):
                self.timeouts.append(value)

            def connect(self, address):
                self.address = address
                test.now += min(delay, self.timeouts[-1]) if failure else delay
                if failure:
                    raise socket.timeout("有限地址失败")

            def close(self):
                self.closed = True

        return Socket

    def test_failed_addresses_share_one_absolute_five_second_deadline(self):
        with patch("frp_ota.socket.getaddrinfo", return_value=self.addresses), patch(
                "frp_ota.socket.socket", side_effect=self.socket_factory(delay=3, failure=True)):
            with self.assertRaises(TimeoutError):
                self.client._open(300)
        self.assertEqual(self.now, 5)
        self.assertEqual([connection.timeouts for connection in self.connections], [[5], [2]])
        self.assertTrue(all(connection.closed for connection in self.connections))

    def test_late_tcp_success_is_closed_before_use(self):
        with patch("frp_ota.socket.getaddrinfo", return_value=self.addresses), patch(
                "frp_ota.socket.socket", side_effect=self.socket_factory(delay=5)):
            with self.assertRaises(TimeoutError):
                self.client._open(5)
        self.assertEqual(len(self.connections), 1)
        self.assertTrue(self.connections[0].closed)

    def test_resolver_creation_dns_and_tcp_consume_the_same_budget(self):
        test = self
        budgets = []

        class Resolver:
            returncode = 0

            def __init__(self, arguments, **kwargs):
                test.assertEqual(arguments[:3], [frp_ota.sys.executable, "-I", "-c"])
                test.assertEqual(arguments[-2:], ["deadline.invalid", "8000"])
                test.assertEqual(kwargs["stdin"], frp_ota.subprocess.DEVNULL)
                test.now += 2

            def communicate(self, timeout):
                budgets.append(timeout)
                test.now += 1
                return json.dumps(test.addresses[:1]).encode(), None

        with patch("frp_ota.socket.getaddrinfo", side_effect=socket.gaierror), patch(
                "frp_ota.subprocess.Popen", side_effect=Resolver), patch(
                "frp_ota.socket.socket", side_effect=self.socket_factory(delay=1)):
            connection = self.client._open(5)
        self.assertEqual(budgets, [3])
        self.assertEqual(connection.timeouts, [2])
        self.assertEqual(self.now, 4)
        self.assertEqual(connection.address, self.addresses[0][-1])
        connection.close()

    def test_numeric_ipv6_does_not_spawn_resolver_and_keeps_scope(self):
        address = ("fe80::1", 8000, 0, 7)
        self.client = frp_ota.FrpClient("http://[fe80::1%en0]:8000", DEVICE, KEY,
            clock=lambda: self.now)
        with patch("frp_ota.socket.getaddrinfo", return_value=[
                (socket.AF_INET6, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", address)]) as lookup, patch(
                "frp_ota.subprocess.Popen") as process, patch(
                "frp_ota.socket.socket", side_effect=self.socket_factory(delay=1)):
            connection = self.client._open(5)
        self.assertEqual(lookup.call_args.args[-1], socket.AI_NUMERICHOST)
        process.assert_not_called()
        self.assertEqual(connection.address, address)
        connection.close()

    def test_tcp_context_creation_and_tls_share_original_deadline(self):
        self.client = frp_ota.FrpClient("https://deadline.invalid", DEVICE, KEY,
            clock=lambda: self.now)
        test = self

        class Context:
            def __init__(self):
                test.now += .5

            def wrap_socket(self, connection, *, server_hostname):
                test.assertEqual(server_hostname, "deadline.invalid")
                test.assertEqual(connection.timeouts[-1], 1.5)
                test.now += 1.5
                return connection

        with patch("frp_ota.socket.getaddrinfo", return_value=self.addresses), patch(
                "frp_ota.socket.socket", side_effect=self.socket_factory(delay=3)), patch(
                "frp_ota.ssl.create_default_context", side_effect=Context):
            with self.assertRaises(TimeoutError):
                self.client._open(5)
        self.assertEqual(self.now, 5)
        self.assertEqual(len(self.connections), 1)
        self.assertTrue(self.connections[0].closed)

    def test_expired_dns_process_is_killed_and_reaped(self):
        processes = []
        real_popen = frp_ota.subprocess.Popen

        def start_process(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            processes.append(process)
            return process

        client = frp_ota.FrpClient("http://deadline.invalid", DEVICE, KEY)
        started = time.monotonic()
        with patch("frp_ota.socket.getaddrinfo", side_effect=socket.gaierror), patch(
                "frp_ota.DNS_LOOKUP", "import time; time.sleep(60)"), patch(
                "frp_ota.subprocess.Popen", side_effect=start_process), patch(
                "frp_ota.socket.socket") as connections:
            with self.assertRaisesRegex(TimeoutError, "DNS"):
                client._open(started + .2)
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].returncode)
        self.assertTrue(processes[0].stdout.closed)
        with self.assertRaises(ChildProcessError):
            frp_ota.os.waitpid(processes[0].pid, frp_ota.os.WNOHANG)
        connections.assert_not_called()


if __name__ == "__main__":
    unittest.main()
