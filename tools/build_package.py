#!/usr/bin/env python3
"""用锁定 IDF 构建并封装实际签名固件；编号只消费 Core，不操作设备。"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.metadata
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

from build_identity import (ROOT, TARGETS, decode_json, encoded, inspect_image, read_json,
                            resolve, validate_source_inputs)
from check_sdk import check as check_sdk
from prepare_native_layout import MigrationError, parse_table, table_from_csv, signed_image as native_image

SLOT_BYTES = 0x1E0000
BUILD_FILES = ("esp_base.elf", "bootloader/bootloader.bin",
               "partition_table/partition-table.bin", "ota_data_initial.bin")
PACKAGE_FILES = {"firmware.bin", "verification-key.bin", "sdk-lock.json", "LICENSE", "inputs.json",
                 "context.json", "version-snapshot.json", "build.json", "dependencies.lock",
                 *(Path(name).name for name in BUILD_FILES)}
RUN_FILES = {"package-inputs.json", "package-manifest.json"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def regular(path):
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("输入或制品必须为普通文件")
    return path.read_bytes()


def fact(path):
    data = regular(path)
    return {"sha256": digest(data), "bytes": len(data)}


def write_once(path, data):
    if path.is_symlink():
        raise ValueError("收据或输入不能为链接")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if regular(path) != data:
            raise ValueError("既有冻结输入或构建收据不同，拒绝覆盖")
        return
    with tempfile.TemporaryDirectory(prefix=".firmware-write-", dir=path.parent) as temporary:
        candidate = Path(temporary) / "input"
        candidate.write_bytes(data)
        candidate.chmod(0o600)
        try:
            os.link(candidate, path)
        except FileExistsError:
            if regular(path) != data:
                raise ValueError("并发写入的冻结事实不同")


def config_values(path):
    values = {}
    for line in regular(path).decode().splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
        disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        if not match and not disabled:
            if line and not line.startswith("#"):
                raise ValueError("sdkconfig 含未知配置行")
            continue
        key, value = (match[1], match[2]) if match else (disabled[1], "n")
        # 锁定 IDF 的 sdkconfig.rename 兼容段会重复输出同值旧名称。
        if key in values and values[key] != value:
            raise ValueError("sdkconfig 同名字段值冲突")
        values[key] = value
    return values


def public_key(path, target):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
    data = regular(path)
    if b"PRIVATE KEY" in data:
        raise ValueError("验证输入必须是公钥，不能归档私钥")
    if target == "esp32":
        if len(data) != 64:
            raise ValueError("ESP32 验证公钥必须为原生 64 字节 P-256 格式")
        ec.EllipticCurvePublicNumbers(int.from_bytes(data[:32], "big"),
                                     int.from_bytes(data[32:], "big"), ec.SECP256R1()).public_key()
    else:
        key = serialization.load_pem_public_key(data)
        if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
            raise ValueError("C3 验证公钥必须为 RSA-3072")
    return data


def check_config(config, target, verification_key):
    required = ("CONFIG_APP_REPRODUCIBLE_BUILD", "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT",
                "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT", "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
                "CONFIG_PARTITION_TABLE_CUSTOM", "CONFIG_ESPTOOLPY_FLASHSIZE_4MB")
    if any(config.get(key) != "y" for key in required):
        raise ValueError("固件交付需要可复现构建、签名更新、回滚和固定 4 MiB 分区")
    if config.get("CONFIG_SECURE_BOOT", "n") != "n" or config.get("CONFIG_IDF_TARGET") != json.dumps(target):
        raise ValueError("目标或硬件安全配置不符合当前 Base 合同")
    scheme = "RSA" if target == "esp32c3" else "ECDSA"
    partition = "c3-partition-table.csv" if target == "esp32c3" else "esp32-partition-table.csv"
    if (config.get(f"CONFIG_SECURE_SIGNED_APPS_{scheme}_SCHEME") != "y" or
            config.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME") != json.dumps("partitions/" + partition) or
            config.get("CONFIG_PARTITION_TABLE_OFFSET") != "0x8000" or
            config.get("CONFIG_PARTITION_TABLE_MD5") != ("y" if target == "esp32c3" else "n")):
        raise ValueError("签名方案或实际分区文件与 target 不符")
    if target == "esp32" and config.get("CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT") != "y":
        raise ValueError("ESP32 需要签名启动校验")
    automatic = config.get("CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES") == "y"
    if not automatic and target == "esp32c3":
        # 官方 RSA v2 外部签名不编译 ECDSA 专用 verification-key Kconfig。
        # 公钥随完整签名块验证；发布仍需显式的授权键及运行设备信任验收。
        return False
    key = Path(json.loads(config.get("CONFIG_SECURE_BOOT_SIGNING_KEY" if automatic
                                     else "CONFIG_SECURE_BOOT_VERIFICATION_KEY", '""')))
    if (not key.is_absolute() or key.is_symlink() or not key.is_file() or
            key.resolve().is_relative_to(ROOT)):
        raise ValueError("签名或验证键必须为仓外绝对普通文件")
    if automatic:
        # Extract only public bytes using the installed official signing tool.
        # Private bytes and private-key digests never enter a receipt.
        with tempfile.TemporaryDirectory(prefix="esp-base-public-") as temporary:
            output = Path(temporary) / "public.bin"
            subprocess.run([sys.executable, "-m", "espsecure", "extract-public-key",
                            "--version", "2" if target == "esp32c3" else "1",
                            "--keyfile", str(key), str(output)], check=True, stdout=subprocess.DEVNULL)
            if public_key(output, target) != verification_key:
                raise ValueError("自动签名键与冻结验证公钥不同")
    elif regular(key) != verification_key:
        raise ValueError("SDK 内嵌验证键与冻结签名公钥不同")
    return automatic


def tools_fact(target):
    names = ("cmake", "ninja", ("riscv32" if target == "esp32c3" else "xtensa") + "-esp-elf-gcc")
    result = {}
    for name in names:
        located = shutil.which(name)
        if not located:
            raise ValueError("请先导出锁定 SDK 环境；缺少 " + name)
        path = Path(located).resolve()
        result[name] = {"path": str(path), **fact(path),
                        "version": subprocess.check_output([str(path), "--version"], text=True).splitlines()[0]}
    result["python"] = {"path": str(Path(sys.executable).resolve()), "version": sys.version,
                        "packages": {name: importlib.metadata.version(name)
                                     for name in ("esptool", "idf-component-manager", "esp-idf-kconfig")}}
    return result


def current_inputs(target, sdkconfig, key, trust):
    idf = Path(os.environ.get("IDF_PATH", ""))
    if not idf.is_absolute():
        raise ValueError("必须显式导出锁定 IDF_PATH")
    idf = idf.resolve(strict=True)
    check_sdk(idf)
    sdkconfig, key = sdkconfig.absolute(), key.absolute()
    verification = public_key(key, target)
    config = config_values(sdkconfig)
    automatic = check_config(config, target, verification)
    sha = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    validate_source_inputs()
    selected_lock = "dependencies.lock.esp32" if target == "esp32" else "dependencies.lock"
    return {"schema_version": 1, "target": target, "source_commit_sha": sha,
            "sdk_root": str(idf), "sdk_lock": fact(ROOT / "sdk-lock.json"),
            "dependency_lock": fact(ROOT / "firmware" / selected_lock),
            "components": component_facts(target),
            "sdkconfig": {"path": str(sdkconfig), **fact(sdkconfig),
                          "values_sha256": digest(encoded(config))},
            "verification_key": {"path": str(key), **fact(key)},
            "automatic_signing": automatic, "signing_trust": trust, "tools": tools_fact(target)}


def portable_input(value):
    """冻结字节与工具身份进入包；宿主绝对路径只留在本地输入文件。"""
    result = {k: v for k, v in value.items() if k != "sdk_root"}
    for name in ("sdkconfig", "verification_key"):
        result[name] = {k: v for k, v in value[name].items() if k != "path"}
    result["tools"] = {name: {k: v for k, v in tool.items() if k != "path"}
                       for name, tool in value["tools"].items()}
    return result


def validate_run_inputs(snapshot, manifest_raw, inputs, context):
    """验证 Core 实际采集的完整输入；不把产品摘要冒充整个 Profile 快照。"""
    target = inputs["target"]
    manifest = decode_json(manifest_raw)
    argv = ["python3", "tools/package_entry.py", "inputs", target]
    expected = {"schema_version": 1, "repo_name": "esp-base", "config_path": "release.json",
                "platform": "firmware", "application_id": "esp-base", "variant": target,
                "architectures": [TARGETS[target][0]], "entry": "tools/package_entry.py",
                "artifact": "esp-base-" + target + ".zip", "identity_entry": "build.json",
                "input_commands": [argv]}
    if (type(manifest) is not dict or type(manifest.get("schema_version")) is not int or
            manifest != expected or digest(manifest_raw) != context["job_manifest_sha256"]):
        raise ValueError("独立打包声明未绑定当前目标及 Core context")
    if (type(snapshot) is not dict or set(snapshot) != {"schema_version", "kind", "commands"} or
            type(snapshot["schema_version"]) is not int or snapshot["schema_version"] != 1 or
            snapshot["kind"] != "standalone-package-inputs" or
            type(snapshot["commands"]) is not list or len(snapshot["commands"]) != 1 or
            digest(core_encoded(snapshot)) != context["profile_snapshot_id"]):
        raise ValueError("固件输入未绑定 Core 采集的完整输入快照")
    command = snapshot["commands"][0]
    if (type(command) is not dict or
            set(command) != {"argv", "executable", "executable_sha256", "stdout"} or
            command["argv"] != argv or type(command["executable"]) is not str or
            not Path(command["executable"]).is_absolute() or
            type(command["executable_sha256"]) is not str or
            not re.fullmatch(r"[0-9a-f]{64}", command["executable_sha256"]) or
            command["stdout"] != encoded(inputs).decode()):
        raise ValueError("Core 冻结的工具输出与实际固件输入不同")


def core_encoded(value):
    # 与 Core 原始记录相同的 UTF-8 编码；不能改变非 ASCII 工具路径的摘要。
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode()


def run_inputs(value):
    snapshot_path = Path(os.environ.get("FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE", ""))
    if not snapshot_path.is_absolute():
        raise ValueError("正式构建必须由 Core package_run.py 提供原始收据")
    directory = snapshot_path.parent
    request = read_json(directory / "package-run.json")
    original = read_json(directory / "client-version-result.json")
    context = original["context"]
    manifest_raw = regular(ROOT / "tools" / ("package-" + value["target"] + ".json"))
    snapshot = read_json(directory / "package-inputs.json")
    if (set(request) != {"schema_version", "kind", "context", "manifest"} or
            type(request["schema_version"]) is not int or request["schema_version"] != 1 or
            request["kind"] != "standalone-package-run" or request["context"] != context or
            request["manifest"] != decode_json(manifest_raw) or
            context["profile_snapshot_id"] != os.environ.get("FAST_DEPLOY_PROFILE_SNAPSHOT_ID")):
        raise ValueError("独立打包原始请求与同次 Core 收据不同")
    validate_run_inputs(snapshot, manifest_raw, portable_input(value), context)
    command = snapshot["commands"][0]
    interpreter = Path(sys.executable).absolute()
    if command["executable"] != str(interpreter) or command["executable_sha256"] != fact(interpreter)["sha256"]:
        raise ValueError("打包解释器已经偏离 Core 采集的工具")
    return {"package-inputs.json": regular(directory / "package-inputs.json"),
            "package-manifest.json": manifest_raw}


def validate_input(path):
    value = read_json(path)
    if value.get("signing_trust") not in ("software-test", "release"):
        raise ValueError("必须冻结签名信任用途")
    current = current_inputs(value["target"], Path(value["sdkconfig"]["path"]),
                             Path(value["verification_key"]["path"]), value["signing_trust"])
    if encoded(current) != regular(path) or value != current:
        raise ValueError("SDK、配置、签名、工具链或源码已经偏离冻结输入")
    run_inputs(value)
    identity = resolve(value["target"])
    if identity["local_validation"] or identity["source_commit_sha"] != value["source_commit_sha"]:
        raise ValueError("正式固件构建必须使用同一冻结源码及 Core 编号")
    return value, identity


@contextmanager
def locked_build(directory):
    if not directory.is_absolute() or directory.is_symlink() or directory.resolve().is_relative_to(ROOT):
        raise ValueError("构建目录必须为仓外绝对目录")
    directory.mkdir(parents=True, exist_ok=True)
    fd = os.open(directory / ".firmware.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def component_facts(target):
    import yaml
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
    name = "dependencies.lock.esp32" if target == "esp32" else "dependencies.lock"
    values = yaml.safe_load(regular(ROOT / "firmware" / name))
    result = {}
    for name, entry in values["dependencies"].items():
        if name == "idf":
            continue
        path = ROOT / "firmware/managed_components" / name.replace("/", "__")
        validate_hash_eq_hashdir(path, entry["component_hash"])
        # Hash all actual files too: ignored additions must not become CMake inputs.
        members = {}
        for member in sorted(path.rglob("*")):
            if member.is_symlink():
                raise ValueError("固件公开组件不得通过链接读取额外输入")
            if member.is_file():
                members[member.relative_to(path).as_posix()] = fact(member)
        result[name] = {"locked_hash": entry["component_hash"], "files_sha256": digest(encoded(members))}
    return result


def environment():
    rejected = ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "EXTRA_COMPONENT_DIRS",
                "CMAKE_TOOLCHAIN_FILE", "CMAKE_PROJECT_INCLUDE", "IDF_COMPONENT_MANAGER")
    if any(os.environ.get(key) for key in rejected):
        raise ValueError("不能用环境覆盖冻结的原生构建输入")
    env = dict(os.environ, IDF_CCACHE_ENABLE="0", CCACHE_DISABLE="1")
    for key in ("SDKCONFIG", "SDKCONFIG_DEFAULTS", "IDF_TARGET", "PROJECT_VER"):
        env.pop(key, None)
    return env


def unsigned_path(directory, value):
    return directory / ("esp_base-unsigned.bin" if value["automatic_signing"] else "esp_base.bin")


def built_facts(directory, value, identity):
    if digest(encoded(config_values(directory / "sdkconfig"))) != value["sdkconfig"]["values_sha256"]:
        raise ValueError("实际 SDK 配置与冻结配置不同")
    unsigned = unsigned_path(directory, value)
    inspect_image(unsigned, identity, value["target"])
    raw = regular(unsigned)
    body = native_image(raw, value["target"], signed=False)
    if raw != body + b"\xff" * (len(raw) - len(body)):
        raise ValueError("未签固件存在非规范尾部")
    elf = regular(directory / "esp_base.elf")
    if (elf[:6] != b"\x7fELF\x01\x01" or
            int.from_bytes(elf[18:20], "little") != (243 if value["target"] == "esp32c3" else 94)):
        raise ValueError("实际 ELF 架构不符")
    bootstrap_facts(directory, value)
    return {"files": {name: fact(directory / name) for name in BUILD_FILES},
            "unsigned": fact(unsigned), "components": component_facts(value["target"])}


def bootstrap_facts(directory, value):
    target = value["target"]
    bootloader = regular(directory / "bootloader/bootloader.bin")
    if (native_image(bootloader, target, app=False) != bootloader or
            len(bootloader) > 0x8000 - (0x1000 if target == "esp32" else 0)):
        raise ValueError("bootloader 镜像或几何非法")
    components = Path(value["sdk_root"]) / "components"
    csv = ROOT / "firmware/partitions" / ("esp32-partition-table.csv" if target == "esp32"
                                         else "c3-partition-table.csv")
    expected = table_from_csv(csv, components, disable_md5=target == "esp32")
    table = regular(directory / "partition_table/partition-table.bin")
    trailer = 68 if target == "esp32" and value["automatic_signing"] else 0
    if len(table) != len(expected) + trailer or table[:len(expected)] != expected:
        raise ValueError("实际分区表不等于冻结 CSV 的官方生成字节")
    entries = parse_table(table, components)
    if sorted((v[2], v[3]) for v in entries.values() if v[0] == 0) != [(0x20000, SLOT_BYTES), (0x200000, SLOT_BYTES)]:
        raise ValueError("实际双 app 槽几何不符")
    if regular(directory / "ota_data_initial.bin") != b"\xff" * 8192:
        raise ValueError("初始 OTA 数据不是空白双扇区")
    if target == "esp32":
        key = Path(value["verification_key"]["path"])
        for name in ("esp_base.elf", "bootloader/bootloader.elf"):
            if embedded_public_key(directory / name) != regular(key):
                raise ValueError("app 或 bootloader 的实际内嵌验证键不同")
        if trailer:
            with tempfile.TemporaryDirectory(prefix="esp-base-table-") as temporary:
                original = Path(temporary) / "partition.bin"
                original.write_bytes(expected)
                verify_signed(directory / "partition_table/partition-table.bin", original, key, target)


def embedded_public_key(path):
    """两条官方签名路径都从最终链接 ELF 回读同一原生符号。"""
    from elftools.elf.elffile import ELFFile
    elf = ELFFile(io.BytesIO(regular(path)))
    symbols = elf.get_section_by_name(".symtab")
    values = [symbols.get_symbol_by_name("_binary_signature_verification_key_bin_" + suffix)
              for suffix in ("start", "end")] if symbols is not None else []
    if len(values) != 2 or any(not group or len(group) != 1 for group in values):
        raise ValueError("实际 ELF 缺少唯一验证键符号")
    start, end = (group[0] for group in values)
    if (not isinstance(start["st_shndx"], int) or start["st_shndx"] != end["st_shndx"] or
            end["st_value"] - start["st_value"] != 64):
        raise ValueError("ELF 验证键符号范围非法")
    section = elf.get_section(start["st_shndx"])
    offset = start["st_value"] - section["sh_addr"]
    data = section.data()
    if not 0 <= offset <= len(data) - 64:
        raise ValueError("ELF 验证键超出实际节")
    return data[offset:offset + 64]


def build(input_path, directory):
    value, identity = validate_input(input_path)
    binding = {"input": value, "build": identity}
    with locked_build(directory):
        receipt = directory / "firmware-build.json"
        if receipt.exists():
            expected = {"binding": binding, "native": built_facts(directory, value, identity)}
            if read_json(receipt) != expected:
                raise ValueError("已完成构建的原件或身份改变")
            return expected
        frozen = directory / "firmware-input.json"
        if not frozen.exists() and any(p.name != ".firmware.lock" for p in directory.iterdir()):
            raise ValueError("不能采用已有的不明构建目录")
        write_once(frozen, encoded(binding))
        config = directory / "sdkconfig"
        if not config.exists():
            write_once(config, regular(Path(value["sdkconfig"]["path"])))
        idf = Path(value["sdk_root"])
        command = [sys.executable, str(idf / "tools/idf.py"), "-C", str(ROOT / "firmware"),
                   "-B", str(directory), "-DIDF_TARGET=" + value["target"], "-DESP_BASE_APP=esp_base",
                   "-DSDKCONFIG=" + str(config), "-DSDKCONFIG_DEFAULTS=", "-DCCACHE_ENABLE=OFF"]
        subprocess.run([*command, "reconfigure"], env=environment(), check=True)
        before = component_facts(value["target"])
        if before != value["components"]:
            raise ValueError("组件实际文件已偏离分配前冻结输入")
        subprocess.run([*command, "build"], env=environment(), check=True)
        native = built_facts(directory, value, identity)
        if native["components"] != before or validate_input(input_path) != (value, identity):
            raise ValueError("构建期间冻结输入或公开组件改变")
        result = {"binding": binding, "native": native}
        write_once(receipt, encoded(result))
        return result


def verify_signed(signed, unsigned, key, target):
    data, original = regular(signed), regular(unsigned)
    padding = (-len(original)) % 4096 if target == "esp32c3" else 0
    trailer = 4096 if target == "esp32c3" else 68
    if (len(data) != len(original) + padding + trailer or len(data) > SLOT_BYTES or
            data[:len(original) + padding] != original + b"\xff" * padding):
        raise ValueError("签名镜像不是本次实际编译字节或超过 app 槽")
    subprocess.run([sys.executable, "-m", "espsecure", "verify-signature",
                    "--version", "2" if target == "esp32c3" else "1",
                    "--keyfile", str(key), str(signed)], check=True, stdout=subprocess.DEVNULL)


def recorded_identity(context, snapshot, target):
    core = Path(os.environ.get("FAST_DEPLOY_PACKAGE_CORE_ROOT", ""))
    if not core.is_absolute():
        raise ValueError("包记录回读需要显式的可信 Core")
    with tempfile.TemporaryDirectory(prefix="esp-base-records-") as temporary:
        context_file, snapshot_file = Path(temporary) / "context.json", Path(temporary) / "snapshot.json"
        context_file.write_bytes(encoded(context))
        snapshot_file.write_bytes(encoded(snapshot))
        return json.loads(subprocess.check_output([
            sys.executable, str(core / "shared/package_identity.py"), "verify-recorded-identity",
            "--context-file", str(context_file), "--snapshot-file", str(snapshot_file),
            "--platform", "firmware", "--application-id", "esp-base",
            "--architecture", TARGETS[target][0], "--variant", target]))


def package(input_path, directory, artifact, signed_image, signed_manifest=None,
            manifest_only=False, signed_partition=None):
    value, identity = validate_input(input_path)
    with locked_build(directory):
        native = built_facts(directory, value, identity)
        expected = {"binding": {"input": value, "build": identity}, "native": native}
        if read_json(directory / "firmware-build.json") != expected:
            raise ValueError("交付前构建收据或实际字节不同")
        signed = signed_image or directory / "esp_base.bin"
        key = Path(value["verification_key"]["path"])
        verify_signed(signed, unsigned_path(directory, value), key, value["target"])
        inspect_image(signed, identity, value["target"])
        original = read_json(Path(os.environ["FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE"]).with_name("client-version-result.json"))
        context, snapshot = original["context"], original["version_snapshot"]
        if recorded_identity(context, snapshot, value["target"]) != identity:
            raise ValueError("Core 原始记录与实际固件身份不同")
        with tempfile.TemporaryDirectory(prefix="esp-base-package-") as temporary:
            stage = Path(temporary) / "package"
            stage.mkdir()
            files = {"firmware.bin": regular(signed), "verification-key.bin": regular(key),
                     "sdk-lock.json": regular(ROOT / "sdk-lock.json"), "LICENSE": regular(ROOT / "LICENSE"),
                     "inputs.json": encoded(portable_input(value)), "context.json": encoded(context),
                     "version-snapshot.json": encoded(snapshot), "build.json": encoded(identity)}
            files.update(run_inputs(value))
            selected = "dependencies.lock.esp32" if value["target"] == "esp32" else "dependencies.lock"
            files["dependencies.lock"] = regular(ROOT / "firmware" / selected)
            for name in BUILD_FILES:
                files[Path(name).name] = regular(directory / name)
            if value["target"] == "esp32" and not value["automatic_signing"]:
                if signed_partition is None:
                    raise ValueError("ESP32 外部签名包还必须提供 --signed-partition")
                verify_signed(signed_partition, directory / "partition_table/partition-table.bin", key, "esp32")
                files["partition-table.bin"] = regular(signed_partition)
            elif signed_partition is not None:
                raise ValueError("只有 ESP32 外部签名路径接受独立签名分区表")
            manifest = {"schema_version": 2, "kind": "esp-base-firmware", "build": identity,
                        "target": value["target"], "slot_capacity_bytes": SLOT_BYTES,
                        "signing_trust": value["signing_trust"],
                        "signature_scheme": "rsa-3072-v2" if value["target"] == "esp32c3" else "ecdsa-p256-v1",
                        "input_manifest_sha256": digest(encoded(portable_input(value))),
                        "sdkconfig_values_sha256": value["sdkconfig"]["values_sha256"],
                        "components": native["components"],
                        "files": {name: {"sha256": digest(data), "bytes": len(data)} for name, data in files.items()}}
            files["manifest.json"] = encoded(manifest)
            manifest_path = directory / "firmware-package-manifest.json"
            write_once(manifest_path, files["manifest.json"])
            if manifest_only:
                return {"manifest": str(manifest_path), "sha256": digest(files["manifest.json"])}
            signed_manifest_path = directory / "firmware-package-manifest.signed.bin"
            if signed_manifest is not None:
                verify_signed(signed_manifest, manifest_path, key, value["target"])
                write_once(signed_manifest_path, regular(signed_manifest))
            elif not signed_manifest_path.exists():
                if not value["automatic_signing"]:
                    raise ValueError("外部签名还需签署已生成的 firmware-package-manifest.json")
                signing_key = json.loads(config_values(Path(value["sdkconfig"]["path"]))["CONFIG_SECURE_BOOT_SIGNING_KEY"])
                candidate = Path(temporary) / "manifest-signed.bin"
                subprocess.run([sys.executable, "-m", "espsecure", "sign-data",
                                "--version", "2" if value["target"] == "esp32c3" else "1",
                                "--keyfile", signing_key, "--output", str(candidate), str(manifest_path)],
                               check=True, stdout=subprocess.DEVNULL)
                write_once(signed_manifest_path, regular(candidate))
            verify_signed(signed_manifest_path, manifest_path, key, value["target"])
            files["manifest.signed.bin"] = regular(signed_manifest_path)
            local = {k: v for k, v in identity.items() if k not in ("build_number", "source_commit_sha", "input_sha256")}
            local.update(local_validation=True, run_id="")
            files["build.json"] = encoded(local)
            for name, data in files.items():
                path = stage / name
                path.write_bytes(data)
                path.chmod(0o644)
            if validate_input(input_path) != (value, identity) or built_facts(directory, value, identity) != native:
                raise ValueError("封装期间输入或实际构建改变")
            core = Path(os.environ["FAST_DEPLOY_PACKAGE_CORE_ROOT"])
            command = [sys.executable, str(core / "shared/package_identity.py"), "package-directory",
                       "--project-root", str(ROOT), "--platform", "firmware", "--application-id", "esp-base",
                       "--architecture", identity["architecture"], "--variant", value["target"],
                       "--directory", str(stage), "--artifact", str(artifact)]
            report = json.loads(subprocess.check_output(command))
            verify_package(artifact)
            return report


def verify_package(artifact, require_release=False, trusted_key=None):
    # 整次回读只使用同一份字节，避免路径在多次读取间被换成另一包。
    with zipfile.ZipFile(io.BytesIO(regular(artifact))) as archive:
        names = archive.namelist()
        if (len(set(names)) != len(names) or any("/" in name or "\\" in name for name in names) or
                any(item.file_size > 64 * 1024 * 1024 or not stat.S_ISREG(item.external_attr >> 16)
                    for item in archive.infolist())):
            raise ValueError("固件包条目重复或越界")
        manifest = decode_json(archive.read("manifest.json"))
        identity = decode_json(archive.read("build.json"))
        members = PACKAGE_FILES | (RUN_FILES if manifest["schema_version"] == 2 else set())
        if (type(manifest["schema_version"]) is not int or manifest["schema_version"] not in (1, 2) or
                manifest["kind"] != "esp-base-firmware" or
                manifest["target"] not in TARGETS or
                manifest["signature_scheme"] != ("rsa-3072-v2" if manifest["target"] == "esp32c3" else "ecdsa-p256-v1") or
                manifest["build"] != identity or identity["local_validation"] is not False or
                manifest["slot_capacity_bytes"] != SLOT_BYTES or
                manifest["signing_trust"] not in ("software-test", "release") or
                set(manifest["files"]) != members or
                set(names) != members | {"manifest.json", "manifest.signed.bin"}):
            raise ValueError("固件包身份或文件集合非法")
        if require_release and (manifest["signing_trust"] != "release" or trusted_key is None):
            raise ValueError("正式发布拒绝软件测试签名链，且必须显式提供授权验证公钥")
        for name, expected in manifest["files"].items():
            data = archive.read(name)
            if {"sha256": digest(data), "bytes": len(data)} != expected:
                raise ValueError("最终包文件摘要或尺寸不同")
        with tempfile.TemporaryDirectory(prefix="esp-base-readback-") as temporary:
            directory = Path(temporary)
            image, key = directory / "firmware.bin", directory / "verification-key.bin"
            image.write_bytes(archive.read("firmware.bin"))
            key.write_bytes(archive.read("verification-key.bin"))
            public_key(key, manifest["target"])
            if trusted_key is not None and public_key(trusted_key, manifest["target"]) != regular(key):
                raise ValueError("包内签名键不同于本轮授权公钥")
            manifest_path, signed_manifest = directory / "manifest.json", directory / "manifest.signed.bin"
            manifest_path.write_bytes(archive.read("manifest.json"))
            signed_manifest.write_bytes(archive.read("manifest.signed.bin"))
            verify_signed(signed_manifest, manifest_path, key, manifest["target"])
            inputs = decode_json(archive.read("inputs.json"))
            context = decode_json(archive.read("context.json"))
            snapshot = decode_json(archive.read("version-snapshot.json"))
            if manifest["schema_version"] == 2:
                validate_run_inputs(decode_json(archive.read("package-inputs.json")),
                                    archive.read("package-manifest.json"), inputs, context)
            elif digest(encoded(inputs)) != context["profile_snapshot_id"]:
                raise ValueError("历史固件包输入不同于原 Core 快照")
            if (inputs["target"] != manifest["target"] or inputs["signing_trust"] != manifest["signing_trust"] or
                    inputs["source_commit_sha"] != identity["source_commit_sha"] or
                    inputs["components"] != manifest["components"] or
                    inputs["sdkconfig"]["values_sha256"] != manifest["sdkconfig_values_sha256"] or
                    digest(encoded(inputs)) != manifest["input_manifest_sha256"] or
                    inputs["verification_key"] != manifest["files"]["verification-key.bin"] or
                    inputs["sdk_lock"] != manifest["files"]["sdk-lock.json"] or
                    inputs["dependency_lock"] != manifest["files"]["dependencies.lock"] or
                    recorded_identity(context, snapshot, manifest["target"]) != identity):
                raise ValueError("签名清单未绑定完整原始 Core 与固件输入")
            inspect_image(image, identity, manifest["target"])
            if native_image(regular(image), manifest["target"]) != regular(image) or image.stat().st_size > SLOT_BYTES:
                raise ValueError("固件超过目标 app 槽")
            subprocess.run([sys.executable, "-m", "espsecure", "verify-signature",
                            "--version", "2" if manifest["target"] == "esp32c3" else "1",
                            "--keyfile", str(key), str(image)], check=True, stdout=subprocess.DEVNULL)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    freeze = commands.add_parser("freeze")
    freeze.add_argument("--target", choices=TARGETS, required=True)
    freeze.add_argument("--sdkconfig", type=Path, required=True)
    freeze.add_argument("--public-key", type=Path, required=True)
    freeze.add_argument("--signing-trust", choices=("software-test", "release"), required=True)
    freeze.add_argument("--input", type=Path, required=True)
    for name in ("build", "package", "manifest"):
        command = commands.add_parser(name)
        command.add_argument("--input", type=Path, required=True)
        command.add_argument("--build-dir", type=Path, required=True)
        if name in ("package", "manifest"):
            command.add_argument("--signed-image", type=Path)
            command.add_argument("--signed-partition", type=Path)
        if name == "package":
            command.add_argument("--artifact", type=Path, required=True)
            command.add_argument("--signed-manifest", type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("--artifact", type=Path, required=True)
    verify.add_argument("--require-release", action="store_true")
    verify.add_argument("--public-key", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "freeze":
            value = current_inputs(args.target, args.sdkconfig, args.public_key, args.signing_trust)
            write_once(args.input, encoded(value))
            print(json.dumps({"input_manifest_sha256": digest(encoded(portable_input(value)))}))
        elif args.action == "build":
            print(json.dumps(build(args.input, args.build_dir)))
        elif args.action == "package":
            print(json.dumps(package(args.input, args.build_dir, args.artifact, args.signed_image,
                                     args.signed_manifest, signed_partition=args.signed_partition)))
        elif args.action == "manifest":
            print(json.dumps(package(args.input, args.build_dir, None, args.signed_image,
                                     manifest_only=True, signed_partition=args.signed_partition)))
        else:
            print(json.dumps(verify_package(args.artifact, args.require_release, args.public_key)))
    except (ValueError, OSError, KeyError, TypeError, MigrationError, subprocess.SubprocessError) as error:
        parser.exit(1, "固件打包失败：" + str(error) + "\n")


if __name__ == "__main__":
    main()
