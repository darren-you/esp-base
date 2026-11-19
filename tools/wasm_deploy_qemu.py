#!/usr/bin/env python3
"""设备级测试：在 QEMU 里按脚本部署一组应用包，核对应用槽、验签、试运行、提交、回退和重启后的恢复。

测试固件（CONFIG_ESP_BASE_TEST_WASM_DEPLOY）不启动系统层，也没有网络：待部署的包由本脚本放进 Flash 镜像里
ota_1 的位置，固件按固定的顺序把它们当作期望应用下发，「下载」是从 Flash 里拷贝，其余走的都是正式的代码。
固件和应用包用临时生成、用完即弃的键签名；其中一个包故意用另一把键签名。

需要已导出的官方 ESP-IDF 环境（idf.py、esptool、espsecure 和 Espressif 的 QEMU）。在任意位置执行：

    python3 tools/wasm_deploy_qemu.py [esp32c3|esp32] [--no-build]

主机侧的脚本不放在固件根里（工程标准），所以在 tools/；测试固件的 sdkconfig 片段和测试用的应用仍在 firmware/tests。
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import esp_base_app  # noqa: E402

APPS = FIRMWARE / "tests/apps"
STAGED_OFFSET, SLOT_BYTES = 0x200000, 0x8000
QEMU = {"esp32c3": ("qemu-system-riscv32", "esp32c3"), "esp32": ("qemu-system-xtensa", "esp32")}
DONE = "WASM_DEPLOY_TEST done"


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def sign(data, key, directory, name):
    unsigned, signed = directory / (name + ".bin"), directory / (name + "-signed.bin")
    unsigned.write_bytes(data)
    run([sys.executable, "-m", "espsecure", "sign-data", "--version", "2", "--keyfile", key, "--output", signed, unsigned],
        stdout=subprocess.DEVNULL)
    return signed.read_bytes()


# pins 夹具声明的引脚：输出（安全电平低、高各一个）、带上拉的输入、两个 PWM。与 sdk/rust/fixtures/examples/pins.rs 对应。
PINS = {"esp32c3": {"4": "out0", "5": "out1", "6": "in_pullup", "7": "pwm", "10": "pwm"},
        "esp32": {"4": "out0", "5": "out1", "13": "in_pullup", "18": "pwm", "19": "pwm"}}


def package(wasm_name, version, memory=10240, pins=None):
    wasm = (APPS / (wasm_name + ".wasm")).read_bytes()
    esp_base_app.inspect(wasm)
    fields = {"name": wasm_name, "version": version, "abi": 1, "memory": memory, "stack": 2048}
    if pins:
        fields["pins"] = pins
    return esp_base_app.pack(wasm, fields)


def build(target, directory, qemu=True):
    """构建测试固件。qemu 为真时日志走 UART（模拟器只有它）；实板用固件自己的控制台。"""
    idf = Path(os.environ["IDF_PATH"]) / "tools/idf.py"
    defaults = "sdkconfig.defaults;tests/sdkconfig.wasm_deploy" + (";tests/sdkconfig.qemu" if qemu else "")
    run([sys.executable, idf, "-C", FIRMWARE, "-B", directory, "-DIDF_TARGET=" + target,
         "-DSDKCONFIG=" + str(directory / "sdkconfig"), "-DSDKCONFIG_DEFAULTS=" + defaults, "build"],
        stdout=subprocess.DEVNULL)


def flash_image(target, build_dir, work, extra=None):
    """合成 4 MiB 的 Flash 镜像：签名的测试固件，加上放在 ota_1 位置的八个待部署的包。

    extra 是另外一个未签名的应用包（例如真实业务的应用）：放在第九个位置，脚本最后部署它，用来在实板上量它的内存。
    返回镜像的路径；work 目录里还有单独的 app-signed.bin（写到 0x20000）和 staged.bin（写到 0x200000），实板上只写
    这两块和 ota_data_initial.bin，不动 bootloader 和分区表。
    """
    firmware_key, other_key = work / "firmware-key.pem", work / "other-key.pem"
    for key in (firmware_key, other_key):
        run([sys.executable, "-m", "espsecure", "generate-signing-key", "--version", "2", "--scheme", "rsa3072", key],
            stdout=subprocess.DEVNULL)
    lines = (build_dir / "flash_args").read_text().split("\n")
    parts = [line.split() for line in lines[1:] if line.strip()]
    app = next(name for _, name in parts if not name.startswith(("bootloader", "partition_table", "ota_data")))
    signed_app = work / "app-signed.bin"
    signed_app.write_bytes(sign((build_dir / app).read_bytes(), firmware_key, work, "app"))
    image = work / "flash.bin"
    merge = [sys.executable, "-m", "esptool", "--chip", target, "merge-bin", "-o", image, "--pad-to-size", "4MB"] + lines[0].split()
    for offset, name in parts:
        merge += [offset, signed_app if name == app else build_dir / name]
    run(merge, stdout=subprocess.DEVNULL)

    tampered = bytearray(sign(package("hello", "0.1.0+3"), firmware_key, work, "tampered"))
    tampered[200] ^= 0x01  # 签名之后改掉 wasm 里的一个比特
    staged = [
        sign(package("hello", "0.1.0+1"), firmware_key, work, "hello-1"),      # 0：首次部署
        sign(package("hello", "0.1.0+2"), firmware_key, work, "hello-2"),      # 1：替换成新版本
        sign(package("init_fail", "0.1.0+1"), firmware_key, work, "init-fail"),  # 2：启动失败，回退
        bytes(tampered),                                                        # 3：签名后被改动，拒绝
        sign(package("hello", "0.1.0+4"), other_key, work, "other-key"),       # 4：别的键签名，拒绝
        sign(package("trial_fault", "0.1.0+1"), firmware_key, work, "trial-fault"),  # 5：试运行期间出故障，回退
        sign(package("pins", "0.1.0+1", 12288, PINS[target]), firmware_key, work, "pins"),  # 6：操作引脚的应用
        sign(package("hello", "0.1.0+5"), firmware_key, work, "hello-5"),      # 7：换回不用引脚的应用
    ]
    if extra:
        staged.append(sign(Path(extra).read_bytes(), firmware_key, work, "extra"))  # 8：可选，真实业务的应用
    blob = bytearray(b"\xff" * (len(staged) * SLOT_BYTES))
    for index, data in enumerate(staged):
        assert len(data) <= SLOT_BYTES
        blob[index * SLOT_BYTES:index * SLOT_BYTES + len(data)] = data
    (work / "staged.bin").write_bytes(blob)
    with open(image, "r+b") as stream:
        stream.seek(STAGED_OFFSET)
        stream.write(blob)
    return image


def emulate(target, image, timeout):
    program, machine = QEMU[target]
    command = [program, "-nographic", "-machine", machine, "-drive", f"file={image},if=mtd,format=raw", "-serial", "mon:stdio"]
    if target == "esp32":
        # 固件要求芯片版本不低于 v3.0（ESP32-D0WD-V3），模拟器默认是 v0.0：给它一份烧了 CHIP_VER_REV1、CHIP_VER_REV2 的 eFuse。
        efuse = bytearray(124)
        efuse[0x0D], efuse[0x16] = 0x80, 0x10
        path = Path(image).with_name("efuse.bin")
        path.write_bytes(efuse)
        command += ["-drive", f"file={path},if=none,format=raw,id=efuse",
                    "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse"]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               start_new_session=True)
    os.set_blocking(process.stdout.fileno(), False)
    deadline, output = time.time() + timeout, b""
    while time.time() < deadline and process.poll() is None and DONE.encode() not in output:
        chunk = process.stdout.read()
        if chunk:
            output += chunk
        else:
            time.sleep(0.05)
    time.sleep(0.3)
    output += process.stdout.read() or b""
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        pass
    return re.sub(r"\x1b\[[0-9;]*m", "", output.decode("utf-8", "replace"))


def check(log, board=False, extra=None):
    """核对每一步之后的 app/state。返回失败的条目。

    board 为真表示日志来自实板：读回的引脚电平和 PWM 占空比也要对。extra 是可选的第九个包的应用名。
    """
    failures = []

    def expect(condition, what):
        print(("  ok   " if condition else "  FAIL ") + what)
        if not condition:
            failures.append(what)

    states = {int(step): json.loads(text) for step, text in re.findall(r"WASM_DEPLOY_TEST step=(\d+) after=(\{.*\})", log)}
    final = re.search(r"WASM_DEPLOY_TEST done final=(\{.*\})", log)

    def after(step, state, error, version, name="hello"):
        doc = states.get(step) or {}
        running = doc.get("running") or {}
        expect(doc.get("state") == state and doc.get("error") == error and running.get("version") == version and
               running.get("name") == name and (doc.get("app") or {}).get("state") == "running",
               f"步骤 {step}：任务 {state}／{error}，运行 {name} {version}（实际 {doc.get('state')}／{doc.get('error')}，"
               f"运行 {running.get('name')} {running.get('version')}，应用 {(doc.get('app') or {}).get('state')}）")

    after(0, "succeeded", None, "0.1.0+1")
    expect("ESP_BASE_APP_SLOT 0 hello 0.1.0+1" in log and "ESP_BASE_APP_STARTED hello 0.1.0+1 slot=0" in log,
           "步骤 1：重启后从槽 0 恢复运行 hello 0.1.0+1")
    after(2, "succeeded", None, "0.1.0+2")
    expect("ESP_BASE_APP_STARTED hello 0.1.0+2 slot=1" in log, "步骤 2：新版本装进槽 1")
    after(3, "failed", "start_failed", "0.1.0+2")
    after(4, "rejected", "signature_invalid", "0.1.0+2")
    after(5, "rejected", "signature_invalid", "0.1.0+2")
    after(6, "failed", "fault", "0.1.0+2")
    expect(((states.get(6) or {}).get("app") or {}).get("restarts") == 0,
           "步骤 6：试运行的应用出故障是部署失败，不计入故障重启次数（实际 "
           f"{((states.get(6) or {}).get('app') or {}).get('restarts')}）")
    expect(log.count("ESP_BASE_APP_STARTED hello 0.1.0+2 slot=1") == 3, "首次启动加两次回退，都在槽 1 运行 hello 0.1.0+2")
    # 操作引脚的应用：装进槽 0；重启后在联网之前引脚就回到安全状态，应用再跑一遍。
    after(7, "succeeded", None, "0.1.0+1", "pins")
    expect(log.count("ESP_BASE_APP_STARTED pins 0.1.0+1 slot=0") == 2, "步骤 8：重启后从槽 0 恢复运行 pins 0.1.0+1")
    lines = [dict(pair.split("=") for pair in line.split()) for line in re.findall(r"app: pins:((?: \w+=-?\d+)+)", log)]
    runs, expired = [line for line in lines if "out" in line], [line for line in lines if "expired" in line]
    for index, run in enumerate(runs):
        print(f"  pins 第 {index + 1} 次启动：{run}")
    # 返回值两种芯片、模拟器和实板都一样；读回的电平和占空比取决于硬件（模拟器不模拟引脚电平），只打印。
    expect(len(runs) == 2 and all(run.get("out") == "0" and run.get("config") == "0" and run.get("write") == "0" and
                                  run.get("again") == "0" and run.get("second") == "0" and run.get("denied") == "-2" and
                                  run.get("pulse") == "0" for run in runs),
           "pins：写输出、配置和改配 PWM、有时限的输出都成功，没有声明的引脚被拒绝（两次启动）")
    expect(len(expired) == 2 and all(line.get("expired") == "4" for line in expired),
           "pins：有时限的输出到期后应用收到事件（两次启动）")
    if board:
        # 实板：安全电平、写出去的电平、带上拉的输入、占空比都读得回来；时限到了引脚回到低电平。
        expect(all(run.get("safe0") == "0" and run.get("safe1") == "1" and run.get("high") == "1" and run.get("low") == "0" and
                   run.get("in") == "1" and run.get("duty") == "1228" for run in runs),
               "pins（实板）：安全电平 0／1、写高读高、写低读低、上拉输入为高、占空比读回 1228")
        expect(all(line.get("level") == "0" for line in expired), "pins（实板）：时限到期时引脚已经回到低电平")
        stamps = [(int(a), int(b)) for a, b in re.findall(r"\((\d+)\) app: pins: safe0=.*\n(?:.*\n)*?.*\((\d+)\) app: pins: expired", log)]
        for begin, end in stamps:
            print(f"  时限 300 毫秒，实际 {end - begin} 毫秒后应用收到到期事件")
        expect(len(stamps) == 2 and all(295 <= end - begin <= 330 for begin, end in stamps), "pins（实板）：300 毫秒的时限准时到期")
    after(9, "succeeded", None, "0.1.0+5")
    expect("ESP_BASE_APP_STARTED hello 0.1.0+5 slot=1" in log, "步骤 9：换回不用引脚的应用，装进槽 1")
    expect(states.get(10) is None and "step=10 restart" in log, "步骤 10：再重启一次")
    restored = re.findall(r"WASM_DEPLOY_TEST step=11 before=(\{.*\})", log)
    doc = json.loads(restored[0]) if restored else {}
    expect(doc.get("state") == "succeeded" and (doc.get("running") or {}).get("version") == "0.1.0+5" and
           (doc.get("app") or {}).get("state") == "running", "重启后仍运行 hello 0.1.0+5，任务记录保留")
    doc = json.loads(final.group(1)) if final else {}
    if extra:
        after(11, "succeeded", None, doc.get("running", {}).get("version"), extra)
        app = (states.get(11) or {}).get("app") or {}
        print(f"  {extra}：内存池用掉 {app.get('pool_used')} 字节，峰值 {app.get('pool_peak')}，线性内存 {app.get('linear')}")
    else:
        expect("step=11 skipped" in log and bool(final) and (doc.get("running") or {}).get("version") == "0.1.0+5",
               "没有第九个包：最后一步跳过")
    for value in re.findall(r"WASM_DEPLOY_TEST prepare_ms=(\d+)", log):
        print(f"  宿主启动（登记应用槽、逐槽验签、读部署记录）用了 {value} 毫秒")
    expect(not re.search(r"E \(\d+\) (gpio|ledc):", log), "GPIO 和 LEDC 驱动没有报错")
    expect("Guru Meditation" not in log and "abort()" not in log and "assert failed" not in log, "全程没有崩溃")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("target", nargs="?", default="esp32c3", choices=sorted(QEMU))
    parser.add_argument("--no-build", action="store_true", help="用已有的构建目录，不重新构建")
    parser.add_argument("--log", help="把模拟器的完整输出另存一份")
    parser.add_argument("--extra", help="另外一个未签名的应用包：最后部署它（实板上量真实应用的内存）")
    parser.add_argument("--check-log", help="不运行模拟器，只核对一份实板的串口日志")
    parser.add_argument("--image-dir", help="不运行模拟器，只把要写进实板的文件放到这个目录：app-signed.bin、staged.bin")
    args = parser.parse_args()
    # 模拟器和实板的测试固件控制台不同，各用各的构建目录。
    build_dir = FIRMWARE / "build" / (("board-" if args.image_dir else "deploy-") + args.target)
    extra = esp_base_app.describe(Path(args.extra).read_bytes())["manifest"]["name"] if args.extra else None
    if args.check_log:
        log = re.sub(r"\x1b\[[0-9;]*m", "", Path(args.check_log).read_text(encoding="utf-8", errors="replace"))
        failures = check(log, board=True, extra=extra)
        print(f"wasm deploy test ({args.target}, board): {'passed' if not failures else str(len(failures)) + ' failure(s)'}")
        sys.exit(1 if failures else 0)
    if not args.no_build:
        build(args.target, build_dir, qemu=not args.image_dir)
    if args.image_dir:
        directory = Path(args.image_dir)
        directory.mkdir(parents=True, exist_ok=True)
        flash_image(args.target, build_dir, directory, args.extra)
        print(json.dumps({"app": str(directory / "app-signed.bin"), "staged": str(directory / "staged.bin"),
                          "otadata": str(build_dir / "ota_data_initial.bin"),
                          "partition_table": str(build_dir / "partition_table/partition-table.bin")}))
        return
    with tempfile.TemporaryDirectory(prefix="wasm-deploy-") as directory:
        image = flash_image(args.target, build_dir, Path(directory), args.extra)
        log = emulate(args.target, image, 240)
    if args.log:
        Path(args.log).write_text(log)
    for line in log.splitlines():
        if "WASM_DEPLOY_TEST phase=" in line or "WASM_DEPLOY_TEST begin" in line:
            print(line.strip())
    failures = check(log, extra=extra)
    print(f"wasm deploy test ({args.target}): {'passed' if not failures else str(len(failures)) + ' failure(s)'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
