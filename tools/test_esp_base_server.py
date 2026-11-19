#!/usr/bin/env python3
"""app push 与 app deploy 的测试：对着本机的一个假 ESP Tool Server 跑客户端和命令行。

假 Server 只照 OpenAPI 应答，不验签、不连设备；真实的登记、部署和设备行为由 esp-tool 的测试和实板验收覆盖。
    python3 tools/test_esp_base_server.py
"""
import base64
import hashlib
import http.server
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esp_base_app  # noqa: E402
import esp_base_server  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "tools/esp_base.py"
PACKAGE = ROOT / "firmware/tests/apps/hello.ebapp"  # 测试键签名的最小应用包
DEVICE = "0f8fad5b-d9cb-469f-a165-70867728950e"
DEPLOYMENT = "77777777-0000-4000-8000-000000000006"
USER, PASSWORD = "operator", "fixture-password"


class FakeServer:
    """记录收到的请求；部署任务每被读一次就走到 script 里的下一个状态。"""

    def __init__(self):
        self.requests, self.apps, self.script, self.refuse = [], {}, [], None
        fake = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def reply(self, status, code, message, data):
                body = json.dumps({"code": code, "timestamp": 1_794_585_000_000, "msg": message, "data": data}).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def handle_any(self):
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                fake.requests.append((self.command, self.path, dict(self.headers), body))
                expected = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()
                if self.headers.get("Authorization") != expected:
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", 'Basic realm="ESP Tool"')
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path == "/api/v1/not-routed":   # 入口没有放行的路径：Nginx 自己的 404，不是 envelope
                    self.send_response(404)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", "9")
                    self.end_headers()
                    self.wfile.write(b"<html/>\r\n")
                    return
                if fake.refuse and (self.command, self.path) == fake.refuse[0]:
                    return self.reply(*fake.refuse[1], None)
                if (self.command, self.path) == ("POST", "/api/v1/apps"):
                    digest = hashlib.sha256(body).hexdigest()
                    manifest = esp_base_app.describe(body)["manifest"]
                    record = fake.apps.setdefault(digest, {"sha256": digest, "name": manifest["name"], "version": manifest["version"],
                                                           "abi": manifest["abi"], "memory": manifest["memory"], "stack": manifest["stack"],
                                                           "chips": [], "pins": {}, "size": len(body), "created_at": "2026-11-18T10:00:00Z"})
                    return self.reply(200, 200, "ok", record)
                if (self.command, self.path) == ("POST", "/api/v1/app-deployments"):
                    request = json.loads(body)
                    if request["app_sha256"] not in fake.apps:
                        return self.reply(404, 40401, "not found", None)
                    return self.reply(200, 200, "ok", fake.deployment("QUEUED", 0, "", request))
                if (self.command, self.path) == ("GET", "/api/v1/app-deployments/" + DEPLOYMENT):
                    step = fake.script.pop(0) if len(fake.script) > 1 else fake.script[0]
                    return self.reply(200, 200, "ok", fake.deployment(*step))
                self.reply(404, 40401, "not found", None)

            do_GET = do_POST = do_DELETE = handle_any

            def log_message(self, *_):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.httpd.server_port}"

    def deployment(self, status, attempt, error, request=None):
        return {"id": DEPLOYMENT, "device_id": (request or {}).get("device_id", DEVICE),
                "app_sha256": (request or {}).get("app_sha256", "0" * 64), "status": status, "error": error, "attempt": attempt,
                "created_at": "2026-11-18T10:05:00Z", "deadline_at": "2026-11-25T10:05:00Z", "updated_at": "2026-11-18T10:05:00Z",
                "finished_at": None if status in esp_base_server.ACTIVE else "2026-11-18T10:05:20Z"}

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeServer()
        self.addCleanup(self.fake.close)
        self.server = esp_base_server.Server(self.fake.url, USER, PASSWORD)

    def test_base_address_and_credentials_are_checked_before_any_request(self):
        for base in ("http://esp.example.test", "ftp://127.0.0.1", "https://esp.example.test/api", "https://esp.example.test/?x=1",
                     "https://user@esp.example.test", "esp.example.test", ""):
            with self.assertRaises(esp_base_server.ServerError, msg=base):
                esp_base_server.Server(base, USER, PASSWORD)
        for username, password in (("", PASSWORD), (USER, ""), ("a:b", PASSWORD)):
            with self.assertRaises(esp_base_server.ServerError):
                esp_base_server.Server("https://esp.example.test", username, password)
        self.assertEqual(esp_base_server.Server("https://esp.example.test/", USER, PASSWORD).base, "https://esp.example.test")
        self.assertEqual(self.fake.requests, [])

    def test_register_sends_the_package_bytes_with_basic_auth(self):
        package = PACKAGE.read_bytes()
        record = self.server.register_app(package)
        self.assertEqual(record["sha256"], hashlib.sha256(package).hexdigest())
        self.assertEqual(record["name"], "hello")
        method, path, headers, body = self.fake.requests[0]
        self.assertEqual((method, path, body), ("POST", "/api/v1/apps", package))
        self.assertEqual(headers["Content-Type"], "application/octet-stream")
        self.assertEqual(headers["User-Agent"], "esp-base-cli")
        self.assertTrue(headers["Authorization"].startswith("Basic "))
        # 同一份字节再登记一次：返回已有的记录。
        self.assertEqual(self.server.register_app(package), record)

    def test_failures_carry_the_status_the_business_code_and_a_readable_reason(self):
        wrong = esp_base_server.Server(self.fake.url, USER, "wrong-password")
        with self.assertRaises(esp_base_server.ServerError) as caught:
            wrong.register_app(b"x")
        self.assertEqual((caught.exception.status, caught.exception.code), (401, None))
        self.assertNotIn("wrong-password", str(caught.exception))
        with self.assertRaises(esp_base_server.ServerError) as caught:
            self.server.call("GET", "/api/v1/not-routed")
        self.assertEqual(caught.exception.status, 404)
        self.assertIn("入口是否放行了这条路径", str(caught.exception))
        for reply, text in (((409, 40903, "app does not allow this chip"), "应用包的清单不允许这种芯片"),
                            ((409, 40903, "device has not reported an app host"), "没有应用宿主"),
                            ((409, 40901, "device already has an active app deployment"), "已有进行中的部署任务"),
                            ((400, 40003, "app rejected: invalid manifest"), "app rejected: invalid manifest"),
                            ((503, 50301, "broker connection unavailable"), "控制连接不可用")):
            self.fake.refuse = (("POST", "/api/v1/app-deployments"), reply)
            with self.assertRaises(esp_base_server.ServerError) as caught:
                self.server.create_deployment(DEVICE, "a" * 64)
            self.assertEqual((caught.exception.status, caught.exception.code), reply[:2])
            self.assertIn(text, str(caught.exception))
        closed = esp_base_server.Server("http://127.0.0.1:9", USER, PASSWORD, timeout=2)
        with self.assertRaises(esp_base_server.ServerError) as caught:
            closed.deployment(DEPLOYMENT)
        self.assertIn("连不上", str(caught.exception))

    def test_deployment_arguments_are_checked_locally(self):
        for device, digest in (("lab", "a" * 64), (DEVICE.upper(), "a" * 64), (DEVICE, "a" * 63), (DEVICE, "A" * 64)):
            with self.assertRaises(esp_base_server.ServerError):
                self.server.create_deployment(device, digest)
        self.assertEqual(self.fake.requests, [])

    def test_waiting_follows_the_deployment_until_it_ends(self):
        digest = self.server.register_app(PACKAGE.read_bytes())["sha256"]
        self.fake.script = [("QUEUED", 0, ""), ("IN_PROGRESS", 1, ""), ("IN_PROGRESS", 1, "transport"), ("IN_PROGRESS", 2, ""),
                            ("IN_PROGRESS", 2, ""), ("SUCCEEDED", 2, "")]
        created = self.server.create_deployment(DEVICE, digest, 600)
        self.assertEqual(json.loads(self.fake.requests[-1][3]), {"device_id": DEVICE, "app_sha256": digest, "ttl_seconds": 600})
        now, changes = [0.0], []
        final = self.server.wait_deployment(created, 60, lambda item, elapsed: changes.append((item["status"], item["attempt"], item["error"], elapsed)),
                                            clock=lambda: now[0], sleep=lambda seconds: now.__setitem__(0, now[0] + seconds))
        self.assertEqual(final["status"], "SUCCEEDED")
        # 只在状态、次数或原因变了的时候报告。
        self.assertEqual(changes, [("IN_PROGRESS", 1, "", 2.0), ("IN_PROGRESS", 1, "transport", 3.0), ("IN_PROGRESS", 2, "", 4.0),
                                   ("SUCCEEDED", 2, "", 6.0)])

    def test_waiting_gives_up_after_the_timeout_and_leaves_the_deployment_alone(self):
        digest = self.server.register_app(PACKAGE.read_bytes())["sha256"]
        self.fake.script = [("QUEUED", 0, "")]
        created = self.server.create_deployment(DEVICE, digest)
        self.assertNotIn("ttl_seconds", json.loads(self.fake.requests[-1][3]))
        now = [0.0]
        final = self.server.wait_deployment(created, 5, clock=lambda: now[0], sleep=lambda seconds: now.__setitem__(0, now[0] + seconds))
        self.assertEqual(final["status"], "QUEUED")
        self.assertEqual([request[0] for request in self.fake.requests[2:]], ["GET"] * 5)   # 只读，没有取消


class CommandLineTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeServer()
        self.addCleanup(self.fake.close)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.password = Path(self.directory.name) / "password"
        self.password.write_text(PASSWORD + "\n")
        self.digest = hashlib.sha256(PACKAGE.read_bytes()).hexdigest()

    def run_cli(self, *arguments, password=None):
        return subprocess.run([sys.executable, str(CLI), "app", *arguments, "--server", self.fake.url, "--username", USER,
                               "--password-file", str(password or self.password)], capture_output=True, text=True, timeout=60)

    def test_push_registers_a_signed_package_and_refuses_an_unsigned_one(self):
        result = self.run_cli("push", str(PACKAGE))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["sha256"], self.digest)
        unsigned = Path(self.directory.name) / "hello-unsigned.ebapp"
        unsigned.write_bytes(PACKAGE.read_bytes()[:esp_base_app.describe(PACKAGE.read_bytes())["content_bytes"]])
        before = len(self.fake.requests)
        result = self.run_cli("push", str(unsigned))
        self.assertEqual(result.returncode, 1)
        self.assertIn("还没有签名", result.stderr)
        self.assertEqual(len(self.fake.requests), before)   # 没签名的包不发给 Server

    def test_the_password_never_appears_in_the_output(self):
        wrong = Path(self.directory.name) / "wrong"
        wrong.write_text("another-secret\n")
        result = self.run_cli("push", str(PACKAGE), password=wrong)
        self.assertEqual(result.returncode, 1)
        self.assertIn("账户或密码不对", result.stderr)
        self.assertNotIn("another-secret", result.stdout + result.stderr)
        result = self.run_cli("push", str(PACKAGE), password=Path(self.directory.name) / "missing")
        self.assertEqual(result.returncode, 1)
        self.assertIn("读不到密码文件", result.stderr)

    def test_deploy_registers_the_file_then_waits_for_the_device(self):
        self.fake.script = [("IN_PROGRESS", 1, ""), ("SUCCEEDED", 1, "")]
        result = self.run_cli("deploy", str(PACKAGE), "--device", DEVICE)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([line["status"] for line in lines], ["QUEUED", "IN_PROGRESS", "SUCCEEDED", "SUCCEEDED"])
        self.assertEqual(lines[-1]["app_sha256"], self.digest)
        self.assertEqual([(request[0], request[1]) for request in self.fake.requests[:2]],
                         [("POST", "/api/v1/apps"), ("POST", "/api/v1/app-deployments")])
        self.assertEqual(json.loads(self.fake.requests[1][3]), {"device_id": DEVICE, "app_sha256": self.digest})

    def test_deploy_by_digest_reports_a_rejection_with_the_device_reason(self):
        self.run_cli("push", str(PACKAGE))
        self.fake.script = [("REJECTED", 1, "pin_denied")]
        before = len(self.fake.requests)
        result = self.run_cli("deploy", self.digest, "--device", DEVICE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("REJECTED（pin_denied）", result.stderr)
        self.assertEqual(self.fake.requests[before][:2], ("POST", "/api/v1/app-deployments"))   # 给的是 SHA-256：不再登记

    def test_deploy_timeout_exit_code_and_no_wait(self):
        self.run_cli("push", str(PACKAGE))
        self.fake.script = [("QUEUED", 0, "")]
        result = self.run_cli("deploy", self.digest, "--device", DEVICE, "--timeout", "1.5")
        self.assertEqual(result.returncode, 2)
        self.assertIn("任务仍在进行", result.stderr)
        before = len(self.fake.requests)
        result = self.run_cli("deploy", self.digest, "--device", DEVICE, "--no-wait")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "QUEUED")
        self.assertEqual(len(self.fake.requests), before + 1)

    def test_deploy_refuses_what_is_neither_a_file_nor_a_digest(self):
        result = self.run_cli("deploy", "hxh_lift", "--device", DEVICE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("SHA-256", result.stderr)
        self.fake.refuse = (("POST", "/api/v1/app-deployments"), (409, 40903, "app needs a newer host interface than the device provides"))
        result = self.run_cli("deploy", str(PACKAGE), "--device", DEVICE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("宿主接口比设备固件提供的新", result.stderr)


if __name__ == "__main__":
    unittest.main()
