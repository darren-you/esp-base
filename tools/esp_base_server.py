#!/usr/bin/env python3
"""ESP Tool Server 管理接口的客户端：登记应用包、创建部署任务并等它结束。

只用标准库。账户密码由调用方从仓外的文件读取后传进来，这里不打印、不落盘。除了回环地址，只接受 https://：
Basic Auth 的凭据不走明文连接。接口的字段见 esp-tool 的 OpenAPI（api/server/esp-tool-openapi.yaml）。
"""
import base64
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request

SHA256 = re.compile(r"[0-9a-f]{64}\Z")
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
ACTIVE = ("QUEUED", "IN_PROGRESS")
LOOPBACK = ("127.0.0.1", "localhost", "::1")
# Server 给装不了、冲突这几种情况的消息；其余的原样显示。
MESSAGES = {
    "device has not reported an app host": "设备的固件没有应用宿主（它从没上报过 app/state）",
    "app does not allow this chip": "应用包的清单不允许这种芯片",
    "app needs a newer host interface than the device provides": "应用需要的宿主接口比设备固件提供的新",
    "device already has an active app deployment": "这台设备已有进行中的部署任务",
    "app is referenced by a deployment": "应用包被部署任务引用",
    "not found": "设备或应用包没有登记",
    "broker connection unavailable": "Server 到 Broker 的控制连接不可用",
}


class ServerError(Exception):
    """请求没有成功。status 是 HTTP 状态，code 是 Server 的业务码；连不上时两者都是 None。"""

    def __init__(self, message, status=None, code=None):
        super().__init__(message)
        self.status, self.code = status, code


class Server:
    def __init__(self, base, username, password, timeout=30):
        url = urllib.parse.urlsplit(base)
        if url.scheme not in ("https", "http") or not url.hostname or url.path not in ("", "/") or url.query or url.fragment or url.username:
            raise ServerError("--server 是管理入口的根地址，形如 https://<主机>，不带路径和账户")
        if url.scheme == "http" and url.hostname not in LOOPBACK:
            raise ServerError("--server 必须是 https://：账户密码不走明文连接（回环地址除外）")
        if not username or not password or ":" in username:
            raise ServerError("需要账户和密码")
        self.base = f"{url.scheme}://{url.netloc}"
        self.timeout = timeout
        self._authorization = "Basic " + base64.b64encode(f"{username}:{password}".encode()).decode()

    def call(self, method, path, body=None, content_type=None):
        """调用一个接口，返回 envelope 里的 data。"""
        headers = {"Accept": "application/json", "Authorization": self._authorization, "User-Agent": "esp-base-cli"}
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(self.base + path, data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status, raw = response.status, response.read()
        except urllib.error.HTTPError as error:
            with error:
                status, raw = error.code, error.read()
        except (urllib.error.URLError, OSError) as error:
            raise ServerError(f"连不上 {self.base}：{getattr(error, 'reason', error)}") from None
        if status == 401:
            raise ServerError("账户或密码不对（HTTP 401）", status)
        try:
            envelope = json.loads(raw)
            code, message, data = envelope["code"], envelope["msg"], envelope["data"]
        except (ValueError, KeyError, TypeError):
            raise ServerError(f"{method} {path} 返回 HTTP {status}，不是 Server 的应答（入口是否放行了这条路径？）", status) from None
        if status != 200 or code != 200:
            raise ServerError(f"{MESSAGES.get(message, message)}（HTTP {status}，code {code}）", status, code)
        return data

    def register_app(self, package):
        """登记一个签名的应用包；同一份字节登记过就返回已有的记录。"""
        return self.call("POST", "/api/v1/apps", package, "application/octet-stream")

    def create_deployment(self, device_id, sha256, ttl_seconds=None):
        if not UUID.fullmatch(device_id):
            raise ServerError("设备 ID 必须是小写 UUID")
        if not SHA256.fullmatch(sha256):
            raise ServerError("应用包的 SHA-256 必须是 64 位小写十六进制")
        body = {"device_id": device_id, "app_sha256": sha256}
        if ttl_seconds:
            body["ttl_seconds"] = ttl_seconds
        return self.call("POST", "/api/v1/app-deployments", json.dumps(body).encode(), "application/json")

    def deployment(self, deployment_id):
        return self.call("GET", "/api/v1/app-deployments/" + deployment_id)

    def wait_deployment(self, deployment, timeout, changed=None, interval=1.0, clock=time.monotonic, sleep=time.sleep):
        """每隔 interval 秒读一次任务，直到它结束或等满 timeout 秒；状态、次数或原因变了就调用 changed(任务, 已过秒数)。

        返回最后读到的任务；还在进行中说明等满了时间（任务本身不受影响，Server 和设备会继续）。
        """
        started = clock()
        seen = (deployment["status"], deployment["attempt"], deployment["error"])
        while deployment["status"] in ACTIVE and clock() - started < timeout:
            sleep(interval)
            deployment = self.deployment(deployment["id"])
            now = (deployment["status"], deployment["attempt"], deployment["error"])
            if now != seen and changed:
                changed(deployment, clock() - started)
            seen = now
        return deployment
