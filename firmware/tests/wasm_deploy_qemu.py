#!/usr/bin/env python3
"""设备级测试：在 QEMU 里按脚本部署一组应用包，核对应用槽、验签、试运行、提交、回退和重启后的恢复。

测试固件（CONFIG_ESP_BASE_TEST_WASM_DEPLOY）不启动系统层，也没有网络：待部署的包由本脚本放进 Flash 镜像里
ota_1 的位置，固件按固定的顺序把它们当作期望应用下发，「下载」是从 Flash 里拷贝，其余走的都是正式的代码。
固件和应用包用临时生成、用完即弃的键签名；其中一个包故意用另一把键签名。

需要已导出的官方 ESP-IDF 环境（idf.py、esptool、espsecure 和 Espressif 的 QEMU）。在 firmware 目录的上一级或任意
位置执行：

    python3 firmware/tests/wasm_deploy_qemu.py [esp32c3|esp32] [--no-build]
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

FIRMWARE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(FIRMWARE.parent / "tools"))
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


def build(target, directory):
    idf = Path(os.environ["IDF_PATH"]) / "tools/idf.py"
    run([sys.executable, idf, "-C", FIRMWARE, "-B", directory, "-DIDF_TARGET=" + target,
         "-DSDKCONFIG=" + str(directory / "sdkconfig"),
         "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;tests/sdkconfig.wasm_deploy;tests/sdkconfig.qemu", "build"],
        stdout=subprocess.DEVNULL)


def flash_image(target, build_dir, work):
    """合成 4 MiB 的 Flash 镜像：签名的测试固件，加上放在 ota_1 位置的八个待部署的包。"""
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
    with open(image, "r+b") as stream:
        for index, data in enumerate(staged):
            assert len(data) <= SLOT_BYTES
            stream.seek(STAGED_OFFSET + index * SLOT_BYTES)
            stream.write(data)
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


def check(log):
    """核对每一步之后的 app/state。返回失败的条目。"""
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
    expect(log.count("ESP_BASE_APP_STARTED hello 0.1.0+2 slot=1") == 3, "首次启动加两次回退，都在槽 1 运行 hello 0.1.0+2")
    # 操作引脚的应用：装进槽 0；重启后在联网之前引脚就回到安全状态，应用再跑一遍。
    after(7, "succeeded", None, "0.1.0+1", "pins")
    expect(log.count("ESP_BASE_APP_STARTED pins 0.1.0+1 slot=0") == 2, "步骤 8：重启后从槽 0 恢复运行 pins 0.1.0+1")
    runs = [dict(pair.split("=") for pair in line.split()) for line in re.findall(r"app: pins:((?: \w+=-?\d+)+)", log)]
    for index, run in enumerate(runs):
        print(f"  pins 第 {index + 1} 次启动：{run}")
    # 返回值两种芯片、模拟器和实板都一样；读回的电平和占空比取决于硬件（模拟器不模拟引脚电平），只打印。
    expect(len(runs) == 2 and all(run.get("out") == "0" and run.get("config") == "0" and run.get("write") == "0" and
                                  run.get("again") == "0" and run.get("second") == "0" and run.get("denied") == "-2"
                                  for run in runs), "pins：写输出、配置和改配 PWM 都成功，没有声明的引脚被拒绝（两次启动）")
    after(9, "succeeded", None, "0.1.0+5")
    expect("ESP_BASE_APP_STARTED hello 0.1.0+5 slot=1" in log, "步骤 9：换回不用引脚的应用，装进槽 1")
    doc = json.loads(final.group(1)) if final else {}
    expect(bool(final) and doc.get("state") == "succeeded" and (doc.get("running") or {}).get("version") == "0.1.0+5" and
           (doc.get("app") or {}).get("state") == "running", "步骤 10：最后一次重启后仍运行 hello 0.1.0+5，任务记录保留")
    expect(not re.search(r"E \(\d+\) (gpio|ledc):", log), "GPIO 和 LEDC 驱动没有报错")
    expect("Guru Meditation" not in log and "abort()" not in log and "assert failed" not in log, "全程没有崩溃")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("target", nargs="?", default="esp32c3", choices=sorted(QEMU))
    parser.add_argument("--no-build", action="store_true", help="用已有的构建目录，不重新构建")
    parser.add_argument("--log", help="把模拟器的完整输出另存一份")
    args = parser.parse_args()
    build_dir = FIRMWARE / "build" / ("deploy-" + args.target)
    if not args.no_build:
        build(args.target, build_dir)
    with tempfile.TemporaryDirectory(prefix="wasm-deploy-") as directory:
        image = flash_image(args.target, build_dir, Path(directory))
        log = emulate(args.target, image, 240)
    if args.log:
        Path(args.log).write_text(log)
    for line in log.splitlines():
        if "WASM_DEPLOY_TEST phase=" in line or "WASM_DEPLOY_TEST begin" in line:
            print(line.strip())
    failures = check(log)
    print(f"wasm deploy test ({args.target}): {'passed' if not failures else str(len(failures)) + ' failure(s)'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
