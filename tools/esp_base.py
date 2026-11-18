#!/usr/bin/env python3
"""ESP Base 固件维护入口：build、sign、flash、control，以及业务应用的 app build、app sign、app inspect。

在已导出官方 ESP-IDF 环境（IDF_PATH、idf.py、esptool、espsecure）的 macOS／Linux 上运行。
私钥、Wi-Fi 与 MQTT 凭据只从仓外文件读取，不打印、不写入仓库。
基于 esp-base 的业务固件用 ESP_BASE_FIRMWARE_ROOT 指向自己的固件根，工程名（决定 target 后半段）取自其 CMakeLists.txt。
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

import esp_base_app

ROOT = Path(__file__).resolve().parents[1]
BASE_FIRMWARE = ROOT / "firmware"
FIRMWARE = Path(os.environ.get("ESP_BASE_FIRMWARE_ROOT") or BASE_FIRMWARE).resolve()
TARGETS = ("esp32c3", "esp32")
NVS_OFFSET, NVS_BYTES = 0x9000, 0x10000
SLOT_BYTES = 0x1E0000
PROVISION_KEYS = ("device_id", "wifi_ssid", "wifi_password", "mqtt_uri", "mqtt_username", "mqtt_password")
BUSINESS_MQTT_KEYS = {"business_mqtt_uri": "biz_mqtt_uri", "business_mqtt_username": "biz_mqtt_user",
                      "business_mqtt_password": "biz_mqtt_pass"}
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
NVS_KEY = re.compile(r"[a-z0-9_]{1,15}\Z")


def app_name():
    """固件根 CMakeLists.txt 里 project() 的名字：构建产物叫 <名字>.bin，target 是 <芯片>/<名字>。"""
    match = re.search(r"^project\((\w+)\)", (FIRMWARE / "CMakeLists.txt").read_text(), re.M)
    if not match:
        fail(f"{FIRMWARE / 'CMakeLists.txt'} 里没有 project(<名字>)")
    return match.group(1)


def fail(message):
    sys.exit("esp_base: " + message)


def idf_path():
    path = os.environ.get("IDF_PATH")
    if not path or not (Path(path) / "tools/idf.py").is_file():
        fail("先导出官方 ESP-IDF 环境：source <esp-idf>/export.sh")
    return Path(path)


BUILD_ROOT = FIRMWARE / "build"  # 正式打包入口会改到仓库之外


def build_dir(target):
    return BUILD_ROOT / target


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


# ---- build ----
def build_firmware(target, build_number, **kwargs):
    directory = build_dir(target)
    run([sys.executable, idf_path() / "tools/idf.py", "-C", FIRMWARE, "-B", directory,
         "-DIDF_TARGET=" + target, "-DSDKCONFIG=" + str(directory / "sdkconfig"),
         "-DESP_BASE_BUILD_NUMBER=" + str(build_number), "build"], **kwargs)
    return directory / f"{app_name()}.bin"


def build(args):
    unsigned = build_firmware(args.target, args.build_number)
    print(json.dumps({"target": args.target, "unsigned": str(unsigned), "bytes": unsigned.stat().st_size,
                      "sha256": sha256(unsigned)}, ensure_ascii=False))


# ---- sign ----
def read_env_value(reference):
    """从 shell env 文件读取一个变量（FILE:VAR），支持带引号的多行 PEM。"""
    file, _, name = reference.rpartition(":")
    if not file or not name:
        fail("--key-env 的格式是 FILE:VAR")
    for token in shlex.split(Path(file).read_text(), comments=True):
        key, separator, value = token.partition("=")
        if separator and key == name:
            return value.replace("\\n", "\n") if "\n" not in value else value
    fail("env 文件里没有变量 " + name)


@contextlib.contextmanager
def signing_key(args):
    if args.key_file:
        yield Path(args.key_file)
        return
    with tempfile.TemporaryDirectory(prefix="esp-base-key-") as directory:
        os.chmod(directory, 0o700)
        path = Path(directory) / "key.pem"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            stream.write(read_env_value(args.key_env).strip() + "\n")
        yield path


def require_rsa3072(key):
    """两目标的 sdkconfig 都是 RSA-3072 签名方案（Secure Boot V2）；其他键签出的镜像设备不会接受。"""
    public = subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout"], check=True, capture_output=True).stdout
    text = subprocess.run(["openssl", "pkey", "-pubin", "-noout", "-text"], input=public,
                          check=True, capture_output=True).stdout.decode()
    if not re.search(r"Public-Key: \(3072 bit\)", text.splitlines()[0]) or "Modulus:" not in text:
        fail("签名键必须是 RSA-3072；两目标共用同一把生产键")


def signed_path(target):
    return build_dir(target) / f"{app_name()}-signed.bin"


def verify_signed(target, key):
    """signed bin = 未签名镜像（已补齐到 4 KiB）+ 一个扇区的 RSA-3072 签名块，并通过官方验签。"""
    unsigned, signed = (build_dir(target) / f"{app_name()}.bin").read_bytes(), signed_path(target).read_bytes()
    padded = unsigned + b"\xff" * (-len(unsigned) % 4096)
    if signed[:len(padded)] != padded or len(signed) != len(padded) + 4096 or len(signed) > SLOT_BYTES:
        fail("签名镜像不是本次构建的字节，或超过 app 槽")
    run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "2", "--keyfile", key,
         signed_path(target)], stdout=subprocess.DEVNULL)


def sign_firmware(target, args):
    unsigned = build_dir(target) / f"{app_name()}.bin"
    if not unsigned.is_file():
        fail("没有构建产物，先运行 build")
    with signing_key(args) as key:
        require_rsa3072(key)
        run([sys.executable, "-m", "espsecure", "sign-data", "--version", "2", "--keyfile", key,
             "--output", signed_path(target), unsigned], stdout=subprocess.DEVNULL)
        verify_signed(target, key)
    return signed_path(target)


def public_key_sha256(args):
    """签名键对应公钥（SPKI DER）的 SHA-256；用来在制品里标明信任根，不暴露私钥。"""
    with signing_key(args) as key:
        der = subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"],
                             check=True, capture_output=True).stdout
    return hashlib.sha256(der).hexdigest()


def sign(args):
    sign_firmware(args.target, args)
    print(json.dumps({"target": args.target, "signed": str(signed_path(args.target)),
                      "bytes": signed_path(args.target).stat().st_size,
                      "sha256": sha256(signed_path(args.target))}, ensure_ascii=False))


# ---- flash ----
def nvs_image(provision, directory):
    """用官方 NVS 分区生成器把出厂资料做成镜像。

    必填设备 UUID、Wi-Fi 和系统 MQTT；可选设备名称 name（1–32 字节 UTF-8，无控制字符，首尾不是空格；
    没给时设备上报「型号-MAC 后 3 字节」）、业务 MQTT（三项同时给出）和 business 对象（业务参数，
    键是最多 15 个字符的小写字母、数字、下划线，值是非空字符串），写进 business 命名空间。
    所属 Group 只在网页里设置，不在出厂资料里。"""
    values = json.loads(Path(provision).read_text())
    business = values.pop("business", {})
    name = values.pop("name", None)
    if name is not None and not (isinstance(name, str) and 0 < len(name.encode()) <= 32 and name == name.strip(" ")
                                 and not any(ord(c) < 0x20 or ord(c) == 0x7f for c in name)):
        fail("name 必须是 1–32 字节、不含控制字符、首尾没有空格的字符串")
    optional = set(values) - set(PROVISION_KEYS)
    if not set(PROVISION_KEYS) <= set(values) or optional not in (set(), set(BUSINESS_MQTT_KEYS)) or \
            not all(isinstance(v, str) and v for v in values.values()):
        fail("出厂资料必须包含：" + "、".join(PROVISION_KEYS) + "；业务 MQTT 的三项要么都给要么都不给")
    if not UUID.fullmatch(values["device_id"]) or not values["mqtt_uri"].startswith("mqtts://") or \
            not values.get("business_mqtt_uri", "mqtts://").startswith("mqtts://"):
        fail("device_id 必须是小写 UUID，mqtt_uri 和 business_mqtt_uri 必须是 mqtts://")
    if not isinstance(business, dict) or not all(NVS_KEY.fullmatch(k) and isinstance(v, str) and v
                                                 for k, v in business.items()):
        fail("business 的键必须是最多 15 个字符的小写字母、数字、下划线，值必须是非空字符串")
    names = {"device_id": "device_id", "wifi_ssid": "wifi_ssid", "wifi_password": "wifi_pass",
             "mqtt_uri": "mqtt_uri", "mqtt_username": "mqtt_user", "mqtt_password": "mqtt_pass", **BUSINESS_MQTT_KEYS}
    rows = ["key,type,encoding,value", "base,namespace,,"]
    entries = [(names[key], value) for key, value in values.items()]
    if name is not None:
        entries.append(("name", name))
    if business:
        entries.append(("business", None))
        entries += sorted(business.items())
    for key, value in entries:
        if value is None:
            rows.append("business,namespace,,")
            continue
        if any(c in value for c in ',"\n\r'):
            fail(key + " 含有 NVS CSV 不支持的字符")
        rows.append(f"{key},data,string,{value}")
    csv, image = Path(directory) / "nvs.csv", Path(directory) / "nvs.bin"
    csv.write_text("\n".join(rows) + "\n")
    generator = idf_path() / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    run([sys.executable, generator, "generate", csv, image, hex(NVS_BYTES)], stdout=subprocess.DEVNULL)
    return image


def build_files(target, app_only):
    """本地构建：偏移取自 flash_args，app 换成签名镜像。"""
    directory = build_dir(target)
    if not signed_path(target).is_file():
        fail("没有签名镜像，先运行 sign")
    files = []
    for line in (directory / "flash_args").read_text().splitlines()[1:]:
        offset, name = line.split()
        if app_only and name != f"{app_name()}.bin":
            continue
        files += [offset, signed_path(target) if name == f"{app_name()}.bin" else directory / name]
    return files, sha256(signed_path(target))


def package_files(path, target, app_only, directory):
    """正式包：偏移取自包内 manifest.json，每个文件先核对摘要再写入。"""
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        if manifest.get("kind") != "esp-base-firmware" or not str(manifest.get("target")).startswith(target + "/"):
            fail("正式包不是 " + target + " 上基于 esp-base 的固件")
        files = []
        for name, offset in sorted(manifest["flash"].items(), key=lambda item: int(item[1], 16)):
            if app_only and name != "firmware.bin":
                continue
            data = archive.read(name)
            if hashlib.sha256(data).hexdigest() != manifest["files"][name]["sha256"]:
                fail("正式包内 " + name + " 的摘要与 manifest 不符")
            (Path(directory) / name).write_bytes(data)
            files += [offset, Path(directory) / name]
    return files, manifest["files"]["firmware.bin"]["sha256"]


def flash(args):
    esptool = [sys.executable, "-m", "esptool", "--chip", args.target, "--port", args.port, "--baud", str(args.baud)]
    with tempfile.TemporaryDirectory(prefix="esp-base-nvs-") as temporary:
        os.chmod(temporary, 0o700)
        if args.package:
            files, app_sha256 = package_files(args.package, args.target, args.app_only, temporary)
        else:
            files, app_sha256 = build_files(args.target, args.app_only)
        if args.provision:
            files += [hex(NVS_OFFSET), nvs_image(args.provision, temporary)]
        if args.erase:
            if not args.provision:
                fail("整片擦除会清掉设备 UUID 和配置，必须同时给出 --provision")
            run([*esptool, "erase-flash"])
        run([*esptool, "write-flash", "--flash-mode", "dio", "--flash-size", "4MB", *files])
    print(json.dumps({"target": args.target, "port": args.port, "package": bool(args.package), "app_sha256": app_sha256,
                      "erased": bool(args.erase), "provisioned": bool(args.provision)}, ensure_ascii=False))


# ---- control ----
def control(args):
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        fail("control 需要 paho-mqtt：python3 -m pip install 'paho-mqtt>=2.1'")
    if not UUID.fullmatch(args.device):
        fail("--device 必须是设备 UUID")
    match = re.fullmatch(r"mqtts://([^:/]+)(?::(\d+))?", args.uri)
    if not match:
        fail("--uri 的格式是 mqtts://host[:port]")
    prefix = f"esp-base/{args.device}/"
    request_id = str(uuid.uuid4())
    seen, done = {}, []

    def on_connect(client, userdata, flags, reason, properties):
        if reason.is_failure:
            fail("Broker 拒绝连接：" + str(reason))
        client.subscribe([(prefix + "status", 1), (prefix + "ota/state", 1), (prefix + "rsp", 1)])

    def on_subscribe(client, userdata, mid, reasons, properties):
        if args.action == "cmd":
            payload = json.loads(args.json) if args.json else {}
            payload["request_id"] = request_id
            client.publish(prefix + "cmd/" + args.name, json.dumps(payload), qos=1)

    def on_message(client, userdata, message):
        name = message.topic[len(prefix):]
        try:
            payload = json.loads(message.payload)
        except ValueError:
            return
        print(json.dumps({"topic": name, "retained": bool(message.retain), "payload": payload}, ensure_ascii=False),
              flush=True)
        seen[name] = payload
        if args.action == "status" and {"status", "ota/state"} <= set(seen):
            done.append(0)
        if args.action == "cmd" and name == "rsp" and payload.get("request_id") == request_id:
            done.append(0 if payload.get("ok") else 1)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="esp-base-cli-" + request_id[:8])
    client.username_pw_set(args.username, Path(args.password_file).read_text().strip())
    client.tls_set_context(ssl.create_default_context())
    client.on_connect, client.on_subscribe, client.on_message = on_connect, on_subscribe, on_message
    client.connect(match.group(1), int(match.group(2) or 8883), keepalive=30)
    client.loop_start()
    deadline = time.monotonic() + args.timeout
    while not done and (args.action == "watch" or time.monotonic() < deadline):
        time.sleep(0.1)
    client.loop_stop()
    client.disconnect()
    if not done:
        fail("等待设备应答超时")
    sys.exit(done[0])


# ---- app：业务应用（WASM） ----
def cargo():
    path = shutil.which("cargo") or str(Path.home() / ".cargo/bin/cargo")
    if not Path(path).is_file():
        fail("没有找到 cargo：先安装 rustup（https://rustup.rs）")
    return path


def app_build(args):
    """构建应用并封装成未签名的应用包：<cargo 的 target 目录>/esp-base/<应用名>.ebapp。"""
    project = Path(args.project).resolve()
    if not (project / "Cargo.toml").is_file():
        fail(f"{project} 里没有 Cargo.toml")
    run([cargo(), "build", "--release"], cwd=project, stdout=sys.stderr)
    metadata = json.loads(subprocess.run([cargo(), "metadata", "--format-version", "1", "--no-deps"], cwd=project,
                                         check=True, capture_output=True).stdout)
    try:
        crate = json.loads(subprocess.run([cargo(), "read-manifest"], cwd=project, check=True, capture_output=True).stdout)
        wasm_path = Path(metadata["target_directory"]) / "wasm32v1-none/release" / (crate["name"].replace("-", "_") + ".wasm")
        if not wasm_path.is_file():
            fail(f"没有找到 {wasm_path}：应用要用 SDK 的 .cargo/config.toml 编译到 wasm32v1-none，crate-type 是 cdylib")
        wasm = wasm_path.read_bytes()
        checked = esp_base_app.inspect(wasm)
        fields = esp_base_app.manifest(project, args.build_number, checked["abi"])
        package = esp_base_app.pack(wasm, fields)
    except esp_base_app.AppError as error:
        fail(str(error))
    output = Path(metadata["target_directory"]) / "esp-base" / (fields["name"] + ".ebapp")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(package)
    print(json.dumps({**fields, "package": str(output), "package_bytes": len(package), "wasm_bytes": len(wasm),
                      "linear_bytes": checked["linear_bytes"], "imports": checked["imports"]}, ensure_ascii=False))


def app_signed_path(package):
    return package.with_name(package.stem.removesuffix("-signed") + "-signed.ebapp")


def app_sign(args):
    """用固件的同一把 RSA-3072 键、同一个官方入口签名：补齐到 4 KiB，再附一个扇区的 Secure Boot V2 签名块。"""
    package = Path(args.package).resolve()
    try:
        described = esp_base_app.describe(package.read_bytes())
    except (OSError, ValueError, esp_base_app.AppError) as error:
        fail(str(error))
    if described["signed"]:
        fail("这个应用包已经签过名")
    signed = app_signed_path(package)
    with signing_key(args) as key:
        require_rsa3072(key)
        run([sys.executable, "-m", "espsecure", "sign-data", "--version", "2", "--keyfile", key, "--output", signed,
             package], stdout=subprocess.DEVNULL)
        run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "2", "--keyfile", key, signed],
            stdout=subprocess.DEVNULL)
    data, unsigned = signed.read_bytes(), package.read_bytes()
    padded = unsigned + b"\xff" * (-len(unsigned) % esp_base_app.SECTOR_BYTES)
    if data[:len(padded)] != padded or len(data) != len(padded) + esp_base_app.SECTOR_BYTES or len(data) > esp_base_app.SLOT_BYTES:
        fail("签名后的应用包不是原包的字节，或超过应用槽")
    print(json.dumps({"name": described["manifest"]["name"], "version": described["manifest"]["version"],
                      "signed": str(signed), "bytes": len(data), "sha256": sha256(signed)}, ensure_ascii=False))


def app_inspect(args):
    """查看应用包或 .wasm：清单、体积、用到的宿主接口和线性内存。"""
    data = Path(args.file).read_bytes()
    try:
        if data[:4] == esp_base_app.MAGIC:
            described = esp_base_app.describe(data)
            wasm = described.pop("wasm")
            result = {**described, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), **esp_base_app.inspect(wasm)}
        else:
            result = {"wasm_bytes": len(data), **esp_base_app.inspect(data)}
    except (ValueError, esp_base_app.AppError) as error:
        fail(str(error))
    print(json.dumps(result, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    command = commands.add_parser("build", help="用官方 ESP-IDF 构建未签名固件")
    command.add_argument("target", choices=TARGETS)
    command.add_argument("--build-number", type=int, default=0, help="正式构建号；本地构建为 0")
    command.set_defaults(run=build)

    command = commands.add_parser("sign", help="用 RSA-3072 键签名并用官方 espsecure 验签")
    command.add_argument("target", choices=TARGETS)
    key = command.add_mutually_exclusive_group(required=True)
    key.add_argument("--key-file", help="仓外的 PEM 私钥文件")
    key.add_argument("--key-env", help="FILE:VAR，从 shell env 文件读取 PEM 私钥")
    command.set_defaults(run=sign)

    command = commands.add_parser("flash", help="经 USB 刷写签名固件，可同时写入出厂资料")
    command.add_argument("target", choices=TARGETS)
    command.add_argument("--port", required=True)
    command.add_argument("--package", help="正式构建的 ZIP；缺省刷本地 build 与 sign 的结果")
    command.add_argument("--provision", help="仓外的出厂资料 JSON（设备 UUID、Wi-Fi、系统 MQTT，可选业务 MQTT 与业务参数），写入 NVS 分区")
    command.add_argument("--erase", action="store_true", help="先整片擦除；必须同时给出 --provision")
    command.add_argument("--app-only", action="store_true", help="只写 app，不动 bootloader、分区表和 otadata")
    command.add_argument("--baud", type=int, default=460800, help="刷写波特率；USB 转串口不稳时降到 230400 或 115200")
    command.set_defaults(run=flash)

    command = commands.add_parser("control", help="经 MQTT 查看状态或下发命令")
    command.add_argument("action", choices=("status", "cmd", "watch"))
    command.add_argument("name", nargs="?", help="cmd 的命令名：status、restart、config")
    command.add_argument("--json", help="cmd 的参数（JSON 对象，不含 request_id）")
    command.add_argument("--device", required=True)
    command.add_argument("--uri", required=True, help="mqtts://host[:port]")
    command.add_argument("--username", required=True)
    command.add_argument("--password-file", required=True)
    command.add_argument("--timeout", type=float, default=30)
    command.set_defaults(run=control)

    command = commands.add_parser("app", help="业务应用（WASM）：构建、签名、查看")
    actions = command.add_subparsers(dest="action", required=True)
    action = actions.add_parser("build", help="用 cargo 构建应用，检查模块并封装成未签名的应用包")
    action.add_argument("project", help="应用工程目录（含 Cargo.toml）")
    action.add_argument("--build-number", type=int, default=0, help="正式构建号；本地构建为 0")
    action.set_defaults(run=app_build)
    action = actions.add_parser("sign", help="用 RSA-3072 键给应用包签名并用官方 espsecure 验签")
    action.add_argument("package", help="app build 产出的 .ebapp")
    key = action.add_mutually_exclusive_group(required=True)
    key.add_argument("--key-file", help="仓外的 PEM 私钥文件")
    key.add_argument("--key-env", help="FILE:VAR，从 shell env 文件读取 PEM 私钥")
    action.set_defaults(run=app_sign)
    action = actions.add_parser("inspect", help="查看应用包或 .wasm 的清单、体积、宿主接口和线性内存")
    action.add_argument("file")
    action.set_defaults(run=app_inspect)

    args = parser.parse_args()
    if args.command == "control" and args.action == "cmd" and not args.name:
        fail("cmd 需要命令名")
    try:
        args.run(args)
    except subprocess.CalledProcessError as error:
        fail(f"{Path(str(error.cmd[0])).name} 失败，退出码 {error.returncode}")


if __name__ == "__main__":
    main()
