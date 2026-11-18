#!/usr/bin/env python3
"""本机运行的测试：模拟器（sdk/sim）、它的 Python 封装（tools/esp_base_sim.py），以及 app new／build／test／run。

用的应用是 host 测试的那一批（firmware/tests/apps，源码在 sdk/rust）。需要 cmake、C 编译器和 rustup；WAMR 与 cJSON
取自 firmware/managed_components（先构建过一次固件）。在仓库的任意位置执行：

    python3 sdk/sim/test_sim.py
"""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import esp_base_app  # noqa: E402
from esp_base_sim import Simulator, SimulatorError  # noqa: E402

APPS = ROOT / "firmware/tests/apps"
TOOL = ROOT / "tools/esp_base.py"
IO_PINS = {4: "out0", 5: "out1", 6: "in_pullup", 7: "pwm", 8: "pwm"}
DENIED, NOT_READY, INVALID, NO_MEMORY = -2, -3, -1, -4


def package(name, directory, **manifest):
    """用 host 测试的应用现封一个未签名的应用包。"""
    fields = {"name": name, "version": "0.1.0+1", "abi": 1, "memory": 12288, "stack": 2048, **manifest}
    path = Path(directory) / (name + ".ebapp")
    path.write_bytes(esp_base_app.pack((APPS / (name + ".wasm")).read_bytes(), fields))
    return path


def io(sim, operation, a=0, b=0, c=0):
    """给 io 夹具发一条命令，返回它报告的返回值（io/result 上的十进制数）或读到的文本（io/text）。"""
    before = len(sim.published)
    sim.message("io/cmd", struct.pack("<BIII", operation, a, b, c))
    assert len(sim.published) == before + 1, sim.events[-3:]
    reply = sim.published[-1]
    return reply.text if reply.topic == "io/text" else int(reply.text)


class HelloTest(unittest.TestCase):
    def test_lifecycle(self):
        with Simulator(APPS / "hello.ebapp") as sim:
            self.assertEqual((sim.info["name"], sim.info["version"], sim.info["wasm_bytes"]), ("hello", "0.1.0+0", (APPS / "hello.wasm").stat().st_size))
            sim.start()
            self.assertTrue(sim.running)
            self.assertEqual(sim.logs, ["hello: init"])
            self.assertEqual((sim.will.topic, sim.will.payload, sim.will.qos, sim.will.retain),
                             ("hello/state", b"offline", 1, True))
            self.assertEqual(sim.subscriptions, ["hello/in"])
            self.assertEqual(sim.timers, {0: {"delay_ms": 1000, "period_ms": 1000}})

            # 没连上：定时器照常到期，应用只计数。
            sim.advance(2500)
            self.assertEqual((sim.uptime_ms, sim.published), (2500, []))
            sim.mqtt_up()
            self.assertEqual((sim.published[-1].topic, sim.published[-1].text), ("hello/state", "online"))
            # 再过 1.6 秒：3000、4000 两次到期，计数接着之前的 2。
            events = sim.advance(1600)
            self.assertEqual([event["payload"] for event in events if event["event"] == "publish"], [b"3", b"4"])

            # 订阅匹配的消息送到应用；不匹配的到不了。
            sim.message("hello/in", {"ping": 1})
            self.assertEqual((sim.published[-1].topic, sim.published[-1].json()), ("hello/echo", {"ping": 1}))
            self.assertEqual(sim.message("hello/other", b"x"), [{"event": "dropped", "reason": "not_subscribed"}])
            self.assertEqual(sim.message("hello/in", b"x" * 1025), [{"event": "dropped", "reason": "too_large"}])

            # 断开后发布不出去；没连上的消息到不了应用。
            sim.mqtt_down()
            count = len(sim.published)
            sim.advance(1000)
            self.assertEqual(len(sim.published), count)
            self.assertEqual(sim.message("hello/in", b"x"), [{"event": "dropped", "reason": "not_connected"}])

            # 停止：宿主替应用发出遗嘱（连着的时候）。
            sim.mqtt_up()
            sim.stop()
            self.assertFalse(sim.running)
            self.assertEqual((sim.published[-1].topic, sim.published[-1].text, sim.published[-1].will),
                             ("hello/state", "offline", True))
            # 停止后可以再启动，从头开始。
            sim.start()
            self.assertTrue(sim.running)
            self.assertEqual(sim.logs, ["hello: init"] * 2)

    def test_stats_and_bare_wasm(self):
        with Simulator(APPS / "hello.wasm", memory=10240, stack=2048) as sim:
            sim.start()
            stats = sim.stats()
            self.assertEqual(stats["linear"], 4176)
            self.assertTrue(0 < stats["pool_used"] <= stats["pool_peak"] <= 10240 * 3 // 2)


class IoTest(unittest.TestCase):
    """引脚、PWM、业务参数、设备信息：io 夹具把宿主接口的返回值原样报出来。"""

    def setUp(self):
        self.sim = Simulator(APPS / "io.wasm", memory=16384, pins=IO_PINS, params={"hxh_site": "lab"},
                             device_id="11111111-2222-4333-8444-555555555555")
        self.addCleanup(self.sim.close)
        self.sim.start()
        self.sim.mqtt_up()

    def test_safe_levels_before_the_app_runs(self):
        safe = {event["pin"]: (event["mode"], event["level"]) for event in self.sim.events if event["event"] == "safe"}
        self.assertEqual(safe, {4: ("out0", 0), 5: ("out1", 1), 6: ("in_pullup", 1), 7: ("pwm", 0), 8: ("pwm", 0)})
        self.assertEqual(self.sim.gpio, {4: 0, 5: 1, 7: 0, 8: 0})

    def test_gpio(self):
        sim = self.sim
        self.assertEqual((io(sim, 1, 4, 1), sim.gpio[4], io(sim, 2, 4)), (0, 1, 1))
        self.assertEqual(io(sim, 2, 5), 1)   # out1 的安全电平能读回
        self.assertEqual(io(sim, 2, 6), 1)   # 带上拉的输入脚没人驱动时是高
        sim.input(6, 0)
        self.assertEqual(io(sim, 2, 6), 0)
        for pin in (6, 7, 9, 12):            # 输入脚、PWM 脚、没声明的、Flash 的
            self.assertEqual(io(sim, 1, pin, 1), DENIED)
        with self.assertRaises(SimulatorError):
            sim.input(4, 1)                  # 输出脚的电平不能从外面给

    def test_pwm(self):
        sim = self.sim
        self.assertEqual(io(sim, 4, 7, 100), NOT_READY)
        self.assertEqual((io(sim, 3, 7, 50, 14), sim.pwm[7]), (0, {"hz": 50, "bits": 14, "duty": 0}))
        self.assertEqual((io(sim, 4, 7, 1228), sim.pwm[7]["duty"], io(sim, 5, 7)), (0, 1228, 1228))
        self.assertEqual(io(sim, 4, 7, 16384), INVALID)
        # 这个频率配不出这样的分辨率：40 kHz 的 14 位要 655 MHz 的时钟。
        self.assertEqual(io(sim, 3, 8, 40000, 14), INVALID)
        self.assertEqual((io(sim, 3, 8, 40000, 10), io(sim, 4, 8, 1023)), (0, 0))

    def test_stop_returns_outputs_to_safe_levels(self):
        sim = self.sim
        io(sim, 1, 4, 1)
        io(sim, 1, 5, 0)
        io(sim, 3, 7, 50, 14)
        io(sim, 4, 7, 800)
        self.assertEqual((sim.gpio[4], sim.gpio[5], sim.pwm[7]["duty"]), (1, 0, 800))
        sim.stop()
        self.assertEqual((sim.gpio, sim.pwm), ({4: 0, 5: 1, 7: 0, 8: 0}, {}))

    def test_pulse_returns_to_the_safe_level_without_the_app(self):
        sim = self.sim
        self.assertEqual((io(sim, 11, 4, 1, 2000), sim.gpio[4]), (0, 1))
        self.assertEqual((sim.advance(1999), sim.gpio[4]), ([], 1))
        # 到期：引脚先回到安全电平，应用随后才收到事件（夹具把引脚号发布到 io/expired）。
        events = sim.advance(1)
        self.assertEqual([event["event"] for event in events], ["pulse_expired", "publish"])
        self.assertEqual((events[0]["pin"], events[0]["level"], events[0]["at_ms"], sim.gpio[4]), (4, 0, 2000, 0))
        self.assertEqual((sim.published[-1].topic, sim.published[-1].text), ("io/expired", "4"))
        # 续期：时限从最后一次算起，中间电平不变。
        io(sim, 11, 4, 1, 2000)
        sim.advance(1500)
        io(sim, 11, 4, 1, 2000)
        self.assertEqual((sim.advance(1999), sim.gpio[4]), ([], 1))
        sim.advance(1)
        self.assertEqual(sim.gpio[4], 0)
        # 普通的写取消时限。
        io(sim, 11, 4, 1, 1000)
        io(sim, 1, 4, 1)
        self.assertEqual((sim.advance(5000), sim.gpio[4]), ([], 1))
        # 安全电平是高的输出脚：到期回到高。
        self.assertEqual((io(sim, 11, 5, 0, 300), sim.gpio[5]), (0, 0))
        sim.advance(300)
        self.assertEqual(sim.gpio[5], 1)
        # 只有输出脚可以，时限 1 毫秒到 10 分钟。
        self.assertEqual([io(sim, 11, 6, 1, 100), io(sim, 11, 7, 1, 100), io(sim, 11, 9, 1, 100)], [DENIED] * 3)
        self.assertEqual([io(sim, 11, 5, 1, 0), io(sim, 11, 5, 1, 600001), io(sim, 11, 5, 1, 600000)], [INVALID, INVALID, 0])

    def test_pulse_expires_while_the_app_sleeps(self):
        sim = self.sim
        io(sim, 11, 4, 1, 200)
        # 应用在一次调用里睡 250 毫秒：时限在它睡着的时候到期，引脚照样按时回到安全电平。
        events = sim.message("io/cmd", struct.pack("<BIII", 9, 250, 1, 0))
        kinds = [(event["event"], event.get("topic")) for event in events]
        self.assertEqual(kinds, [("sleep", None), ("pulse_expired", None), ("publish", "io/result"), ("publish", "io/expired")])
        self.assertEqual((events[1]["at_ms"], sim.gpio[4], sim.uptime_ms), (200, 0, 250))

    def test_param_and_device(self):
        sim = self.sim
        self.assertEqual(io(sim, 7, 0), "11111111-2222-4333-8444-555555555555")
        self.assertEqual(io(sim, 7, 1), "esp32c3")
        before = len(sim.published)
        sim.message("io/cmd", b"\x06hxh_site")
        self.assertEqual((len(sim.published), sim.published[-1].text), (before + 1, "lab"))
        sim.message("io/cmd", b"\x06missing")
        self.assertEqual(sim.published[-1].text, "-6")

    def test_sleep_moves_the_clock(self):
        sim = self.sim
        self.assertEqual((io(sim, 9, 250, 3), sim.uptime_ms), (0, 750))
        self.assertEqual((io(sim, 9, 300, 5), sim.uptime_ms), (-5, 750 + 900))   # 一次调用里累计最多睡 1000 毫秒

    def test_too_many_pwm_combinations(self):
        pins = {0: "pwm", 1: "pwm", 2: "pwm", 3: "pwm", 4: "pwm", 5: "pwm"}
        with Simulator(APPS / "io.wasm", memory=16384, pins=pins) as sim:
            sim.start()
            sim.mqtt_up()
            for pin, hz in enumerate((50, 100, 200, 400)):
                self.assertEqual(io(sim, 3, pin, hz, 10), 0)
            self.assertEqual(io(sim, 3, 4, 800, 10), NO_MEMORY)   # 第五种组合：定时器用完了
            self.assertEqual(io(sim, 3, 4, 400, 10), 0)           # 与已有的相同：共用
            self.assertEqual(io(sim, 3, 0, 800, 10), 0)           # 换掉一个独占的组合：腾出来的定时器接着用


class FaultTest(unittest.TestCase):
    def fault(self, name, reason, trigger):
        with Simulator(APPS / (name + ".wasm"), pins={4: "out1"}) as sim:
            sim.start()
            self.assertTrue(sim.running, sim.start_error)
            events = trigger(sim)
            self.assertIn(reason, sim.fault)
            self.assertFalse(sim.running)
            # 出故障后应用被停掉，输出回到安全电平。
            self.assertEqual([event["event"] for event in events][-2:], ["safe", "stopped"])
            self.assertEqual(sim.gpio, {4: 1})
            self.assertEqual(sim.advance(5000) + sim.event(1), [])

    def test_faults_only_stop_the_app(self):
        timer = lambda sim: sim.event(1)   # noqa: E731  这些夹具不设定时器：直接交给它们一个定时器事件
        self.fault("trial_fault", "instruction limit exceeded", lambda sim: sim.advance(2000))
        self.fault("spin", "instruction limit exceeded", timer)
        self.fault("trap", "unreachable", timer)
        self.fault("bad_pointer", "out of bounds memory access", timer)
        self.fault("deep", "wasm operand stack overflow", timer)

    def test_unknown_events_are_ignored(self):
        with Simulator(APPS / "hello.ebapp") as sim:
            sim.start()
            self.assertEqual(sim.event(99, 1, 2), [])
            self.assertTrue(sim.running)

    def test_start_failure(self):
        with Simulator(APPS / "init_fail.wasm") as sim:
            sim.start()
            self.assertFalse(sim.running)
            self.assertIn("app_init: returned -1", sim.start_error)   # init 返回的错误码：Error::Invalid
        with Simulator(APPS / "hello.wasm", memory=4096) as sim:
            sim.start()
            self.assertFalse(sim.running)
            self.assertTrue(sim.start_error)

    def test_mqtt_up_needs_a_connect_request(self):
        with Simulator(APPS / "spin.wasm") as sim:
            sim.start()
            with self.assertRaises(SimulatorError):
                sim.mqtt_up()


class PackageTest(unittest.TestCase):
    """应用包按设备的规则验收。"""

    def test_manifest_rules(self):
        with tempfile.TemporaryDirectory() as directory:
            pins = package("pins", directory, pins={"4": "out0", "5": "out1", "6": "in_pullup", "7": "pwm", "10": "pwm"})
            with Simulator(pins) as sim:
                sim.start()
                self.assertTrue(sim.running, sim.start_error)
                # 夹具把每一步的返回值写进一行日志：本机上读回的电平和占空比是真的。
                line = dict(pair.split("=") for pair in sim.logs[-1].removeprefix("pins: ").split())
                self.assertEqual(line, {"safe0": "0", "safe1": "1", "out": "0", "high": "1", "low": "0", "in": "1",
                                        "config": "0", "write": "0", "duty": "1228", "again": "0", "second": "0",
                                        "denied": "-2", "pulse": "0"})
                # 300 毫秒的时限到期：引脚回到低电平，应用收到事件后读回来的也是低。
                self.assertEqual(sim.gpio[4], 1)
                sim.advance(300)
                self.assertEqual((sim.gpio[4], sim.logs[-1]), (0, "pins: expired=4 level=0"))
            cases = [
                ("pin_denied", {"pins": {"12": "out0"}}, "esp32c3"),      # Flash 的引脚
                ("pin_denied", {"pins": {"18": "in"}}, "esp32c3"),        # USB
                ("pin_denied", {"pins": {"34": "out0"}}, "esp32"),        # 只能输入
                ("chip_mismatch", {"chips": ["esp32"]}, "esp32c3"),
                ("memory_too_large", {"memory": 65536}, "esp32c3"),
            ]
            for expected, manifest, chip in cases:
                with self.assertRaises(SimulatorError) as raised:
                    Simulator(package("hello", directory, **manifest), chip=chip)
                self.assertEqual(str(raised.exception), expected)
            Simulator(package("hello", directory, pins={"34": "in"}), chip="esp32").close()
            broken = Path(directory) / "broken.ebapp"
            broken.write_bytes(b"EBAP" + bytes(40))
            with self.assertRaises(SimulatorError) as raised:
                Simulator(broken)
            self.assertEqual(str(raised.exception), "package_invalid")


class ToolTest(unittest.TestCase):
    """app new → build → test → run：新建的工程开箱能用，最小应用不超过 4 KB。"""

    def tool(self, *arguments, **kwargs):
        return subprocess.run([sys.executable, str(TOOL), "app", *arguments], capture_output=True, text=True, **kwargs)

    def test_new_project(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "door-bell"
            created = self.tool("new", str(project))
            self.assertEqual(created.returncode, 0, created.stderr)
            self.assertNotEqual(self.tool("new", str(project)).returncode, 0)   # 不覆盖已有的工程
            self.assertNotEqual(self.tool("new", str(Path(directory) / "Bad Name")).returncode, 0)

            built = self.tool("build", str(project))
            self.assertEqual(built.returncode, 0, built.stderr)
            result = json.loads(built.stdout)
            self.assertEqual((result["name"], result["version"], result["abi"]), ("door_bell", "0.1.0+0", 1))
            self.assertLessEqual(result["wasm_bytes"], 4096)
            self.assertLessEqual(result["memory_estimate"], result["memory"])

            tested = self.tool("test", str(project))
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn("1 passed", tested.stdout)

            script = Path(directory) / "script.txt"
            script.write_text("# 连上之后等一秒\nup\nadv 1000\nstop\n", encoding="utf-8")
            ran = self.tool("run", result["package"], "--script", str(script))
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("log info: door-bell: init", ran.stdout)
            self.assertIn("publish topic=door-bell/tick", ran.stdout)

            with Simulator(result["package"]) as sim:
                sim.start()
                sim.mqtt_up()
                sim.advance(3000)
                self.assertEqual([item.topic for item in sim.published], ["door-bell/tick"] * 3)

    def test_run_shows_events(self):
        script = "up\nmsg hello/in ping\nmsg hello/in \\x00\nstop\n"
        ran = self.tool("run", str(APPS / "hello.ebapp"), input=script)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        for line in ("connect keepalive_s=30, will={topic=hello/state, payload=offline, qos=1, retain=true}",
                     "publish topic=hello/state, payload=online, qos=1, retain=true, will=false",
                     "publish topic=hello/echo, payload=ping, qos=0, retain=false, will=false",
                     "publish topic=hello/state, payload=offline, qos=1, retain=true, will=true",   # 停止时宿主代发的遗嘱
                     "stopped"):
            self.assertIn("] " + line + "\n", ran.stdout)

    def test_run_reports_faults(self):
        ran = self.tool("run", str(APPS / "trial_fault.wasm"), input="adv 2000\n")
        self.assertEqual(ran.returncode, 1, ran.stdout + ran.stderr)
        self.assertIn("fault reason=instruction limit exceeded", ran.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
