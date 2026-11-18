#!/usr/bin/env python3
"""本机运行业务应用：驱动 sdk/sim 的模拟器。

模拟器是设备上的同一份宿主核心（firmware/components/base_wasm/core）加 WAMR 的本机构建，平台服务（定时器、业务
MQTT、引脚、PWM、业务参数）换成模拟实现。应用包按设备的规则解析和验收，所以清单写错、引脚在这个芯片上不能用、
用了没有声明的引脚、死循环、陷阱，在本机就能看到，不用等部署到设备上。

时间是虚拟的：定时器只在 advance() 推进时钟时到期，应用睡眠直接把时钟拨快，运行结果是确定的。

    from esp_base_sim import Simulator

    with Simulator("target/esp-base/hxh_lift.ebapp", params={"hxh_site": "lab"}) as sim:
        sim.start()
        sim.mqtt_up()
        sim.message("hxh-studio/lab/lift/cmd/move", {"direction": "up"})
        assert sim.gpio[21] == 1
        sim.advance(2000)            # 没有续租：推杆自己停下
        assert sim.gpio[21] == 0

与设备的差别：
- 不验签。
- 本机的指针是 64 位，运行时自己的结构比设备上大，所以内存池按清单 memory 的 1.5 倍给；stats() 报的是本机的用量，
  比设备上高三成左右。设备上的实际用量看 app/state 里的 pool_used、pool_peak。
- PWM 只核对这个频率配不配得出这样的分辨率，不模拟波形；设备上所有 LEDC 定时器要共用一个时钟源这一条也不模拟。

第一次使用时用 cmake 构建模拟器（要有 cmake 和 C 编译器），WAMR 和 cJSON 取自组件管理器给固件取回的那一份
（firmware/managed_components），没有时先构建一次固件。
"""
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sdk/sim"
MANAGED = ROOT / "firmware/managed_components"
_binary = None


class SimulatorError(Exception):
    pass


def build():
    """构建（或增量更新）模拟器，返回可执行文件的路径。一个进程里只做一次。"""
    global _binary
    if _binary:
        return _binary
    wamr, cjson = MANAGED / "wasm-micro-runtime", MANAGED / "espressif__cjson/cJSON"
    if not (wamr / "build-scripts/runtime_lib.cmake").is_file() or not (cjson / "cJSON.c").is_file():
        raise SimulatorError(f"{MANAGED} 里没有 WAMR 和 cJSON：先在已导出 ESP-IDF 环境的终端里运行一次 "
                             f"python3 {ROOT / 'tools/esp_base.py'} build esp32c3，让组件管理器把它们取回来")
    out = SOURCE / "build"
    try:
        if not (out / "CMakeCache.txt").is_file():
            subprocess.run(["cmake", "-S", str(SOURCE), "-B", str(out), f"-DWAMR_ROOT_DIR={wamr}", f"-DCJSON_DIR={cjson}"],
                           check=True, capture_output=True, text=True)
        subprocess.run(["cmake", "--build", str(out), "-j", "8"], check=True, capture_output=True, text=True)
    except FileNotFoundError:
        raise SimulatorError("没有找到 cmake：本机运行需要 cmake 和 C 编译器") from None
    except subprocess.CalledProcessError as error:
        raise SimulatorError("模拟器构建失败：\n" + (error.stdout or "")[-2000:] + (error.stderr or "")[-2000:]) from None
    _binary = out / "app_sim"
    return _binary


@dataclass
class Publish:
    """应用发布的一条消息；will 为真表示这是宿主在应用停止时替它发出的遗嘱。"""
    topic: str
    payload: bytes
    qos: int
    retain: bool
    will: bool = False

    @property
    def text(self):
        return self.payload.decode("utf-8", "replace")

    def json(self):
        return json.loads(self.payload)


class Simulator:
    """一个在本机运行的应用。app 是应用包（.ebapp，签名与否均可）或裸的 .wasm。

    裸的 .wasm 没有清单，用 memory、stack、pins（{引脚号: 用途}）给出；应用包以包里的清单为准。
    清单或引脚不符合设备的规则时抛 SimulatorError，原因与设备上报的相同（manifest_invalid、pin_denied 等）。
    """

    def __init__(self, app, *, chip="esp32c3", device_id=None, params=None, memory=None, stack=None, pins=None):
        command = [str(build()), str(app), "--chip", chip]
        if device_id:
            command += ["--device-id", device_id]
        for key, value in (params or {}).items():
            command += ["--param", f"{key}={value}"]
        if memory:
            command += ["--memory", str(memory)]
        if stack:
            command += ["--stack", str(stack)]
        for pin, mode in (pins or {}).items():
            command += ["--pin", f"{pin}={mode}"]
        self._process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         text=True, encoding="utf-8")
        self.events = []        # 全部事件，按发生的先后
        self.logs = []          # 应用的日志文本
        self.published = []     # 应用发布的消息（Publish）
        self.subscriptions = []  # 应用订阅的过滤器
        self.gpio = {}          # 引脚号 -> 电平：输出脚的当前电平（含安全电平）
        self.pwm = {}           # 引脚号 -> {"hz", "bits", "duty"}；没在输出 PWM 的引脚不在里面
        self.timers = {}        # 定时器编号 -> {"delay_ms", "period_ms"}：应用最近一次的设置
        self.will = None        # 应用设的遗嘱（Publish），没有则为 None
        self.connect_wanted = False
        self.running = False
        self.fault = None       # 最近一次故障的原因
        self.start_error = None  # 最近一次启动失败的原因
        self.uptime_ms = 0
        first = self._read()
        if first.get("event") != "ready":
            self.close()
            raise SimulatorError(first.get("error") or first.get("text") or "模拟器没有启动")
        self.info = first       # 清单：name、version、abi、memory、stack、wasm_bytes、chip

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self._process.poll() is None:
            self._process.stdin.close()
            try:
                self._process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._process.kill()
        for stream in (self._process.stdout, self._process.stderr):
            if stream and not stream.closed:
                stream.close()

    # ---- 命令 ----
    def start(self):
        """把引脚置到安全状态，加载应用并调用它的 init。失败的原因在 start_error 里。"""
        return self._command(cmd="start")

    def stop(self):
        """停止应用：引脚回到安全状态，宿主替它发出遗嘱并断开连接。"""
        return self._command(cmd="stop")

    def mqtt_up(self):
        """业务 MQTT 连上了。应用要先要求过连接。"""
        return self._command(cmd="mqtt_up")

    def mqtt_down(self):
        return self._command(cmd="mqtt_down")

    def message(self, topic, payload=b""):
        """送一条消息给应用。payload 可以是 bytes、str，或会被编码成 JSON 的 dict／list。

        没连上、没有订阅匹配这个主题、主题或载荷超长时，消息到不了应用，返回的事件里是 dropped。
        """
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        if isinstance(payload, str):
            payload = payload.encode()
        return self._command(cmd="message", topic=topic, payload=payload.hex())

    def advance(self, ms):
        """把时钟推进 ms 毫秒，期间到期的定时器按先后交给应用。"""
        return self._command(cmd="advance", ms=ms)

    def input(self, pin, level):
        """给一个输入脚加电平。"""
        return self._command(cmd="input", pin=pin, level=int(bool(level)))

    def clock(self, unix_ms):
        """对时：之后应用读到的 UTC 时间是这个时刻加上流逝的时间。没对时之前应用读到的是「还没对时」。"""
        return self._command(cmd="clock", unix_ms=unix_ms)

    def event(self, kind, a=0, b=0):
        """原样交给应用一个事件（kind 是事件种类的编号，1 是定时器）：测应用怎样对待没设过的定时器、不认识的事件。"""
        return self._command(cmd="event", kind=kind, a=a, b=b)

    def stats(self):
        """运行时内存池和线性内存的用量（本机的数字，见模块说明）。"""
        return next(event for event in self._command(cmd="stats") if event["event"] == "stats")

    # ---- 内部 ----
    def _read(self):
        line = self._process.stdout.readline()
        if not line:
            detail = self._process.stderr.read().strip()
            raise SimulatorError("模拟器意外退出" + (f"：{detail[-2000:]}" if detail else ""))
        return json.loads(line)

    def _command(self, **fields):
        if self._process.poll() is not None:
            raise SimulatorError("模拟器已经退出")
        self._process.stdin.write(json.dumps(fields) + "\n")
        self._process.stdin.flush()
        events, error = [], None
        while True:
            event = self._read()
            if event["event"] == "idle":
                self.running, self.uptime_ms = event["running"], event["uptime_ms"]
                if error:
                    raise SimulatorError(error)   # 命令本身不成立：没连接就 mqtt_up、给输出脚加电平等
                return events
            if event["event"] == "error":
                error = event["text"]
                continue
            self._apply(event)
            events.append(event)
            self.events.append(event)

    def _apply(self, event):
        kind = event["event"]
        if kind == "log":
            self.logs.append(event["text"])
        elif kind == "publish":
            event["payload"] = bytes.fromhex(event["payload"])
            self.published.append(Publish(event["topic"], event["payload"], event["qos"], event["retain"], event["will"]))
        elif kind == "connect":
            will = event["will"]
            if will:
                will["payload"] = bytes.fromhex(will["payload"])
                self.will = Publish(will["topic"], will["payload"], will["qos"], will["retain"], True)
            else:
                self.will = None
            self.connect_wanted = True
        elif kind == "disconnect":
            self.connect_wanted = False
        elif kind == "subscribe":
            self.subscriptions.append(event["filter"])
        elif kind == "gpio":
            self.gpio[event["pin"]] = event["level"]
        elif kind == "safe":
            self.pwm.pop(event["pin"], None)
            if event["mode"] in ("out0", "out1", "pwm"):
                self.gpio[event["pin"]] = event["level"]
        elif kind == "pwm_config":
            self.pwm[event["pin"]] = {"hz": event["hz"], "bits": event["bits"], "duty": 0}
        elif kind == "pwm":
            self.pwm[event["pin"]]["duty"] = event["duty"]
        elif kind == "timer":
            self.timers[event["id"]] = {"delay_ms": event["delay_ms"], "period_ms": event["period_ms"]}
        elif kind == "timer_cancel":
            self.timers.pop(event["id"], None)
        elif kind == "started":
            self.start_error = self.fault = None
        elif kind == "start_failed":
            self.start_error = event["error"]
        elif kind == "fault":
            self.fault = event["reason"]
        elif kind == "stopped":
            self.subscriptions, self.timers = [], {}


def describe(event):
    """一个事件的单行文本，给 app run 显示用。"""
    shown = dict(event)
    kind = shown.pop("event")
    for holder in (shown, shown.get("will") or {}):
        payload = holder.get("payload")
        if isinstance(payload, bytes):
            try:
                holder["payload"] = payload.decode("utf-8")
            except UnicodeDecodeError:
                holder["payload"] = "hex:" + payload.hex()
    if kind == "log":
        return f"log {('?', 'error', 'warn', 'info')[min(max(shown['level'], 0), 3)]}: {shown['text']}"
    return (kind + " " + json.dumps(shown, ensure_ascii=False, separators=(", ", "="))[1:-1].replace('"', "")).rstrip()
