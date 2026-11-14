#!/usr/bin/env python3
"""正式构建入口，由 Fast Deploy 的 package Job 调用：构建、签名、验签并封装成 ZIP。

环境变量：
  ESP_BASE_SIGNING_KEY_FILE 或 ESP_BASE_SIGNING_KEY_ENV（FILE:VAR）  仓外的 RSA-3072 签名键
  ESP_BASE_SIGNING_TRUST    release 或 software-test；后者不得用于正式设备
版本和构建号由 Fast Deploy 分配并写进 FAST_DEPLOY_IDENTITY_FILE，本入口不分配编号，也不连接设备。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import esp_base

ARCHITECTURES = {"esp32c3": "riscv32", "esp32": "xtensa"}
# 与 dependencies.lock 对应的官方 ESP-IDF 发行版；构建宿主上的位置可用 ESP_BASE_IDF_PATH 覆盖。
IDF_DEFAULT = Path.home() / ".espressif/frameworks/esp-idf-v6.1"
SOURCES = ("sdkconfig.defaults", "sdkconfig.defaults.{target}", "dependencies.lock.{target}",
           "partitions/partitions.csv")


def encoded(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n").encode()


def signing_arguments():
    file, env = os.environ.get("ESP_BASE_SIGNING_KEY_FILE"), os.environ.get("ESP_BASE_SIGNING_KEY_ENV")
    trust = os.environ.get("ESP_BASE_SIGNING_TRUST")
    if bool(file) == bool(env) or trust not in ("release", "software-test"):
        esp_base.fail("需要 ESP_BASE_SIGNING_KEY_FILE 或 ESP_BASE_SIGNING_KEY_ENV 其一，以及 ESP_BASE_SIGNING_TRUST")
    return argparse.Namespace(key_file=file, key_env=env), trust


def tool_version(name):
    path = shutil.which(name)
    if not path:
        esp_base.fail("缺少工具 " + name + "，先导出官方 ESP-IDF 环境")
    return subprocess.check_output([path, "--version"], text=True).splitlines()[0]


def inputs(target):
    """冻结进 Fast Deploy 输入摘要的内容：官方 SDK 提交、工具链、锁与配置的摘要、签名信任根。"""
    idf = esp_base.idf_path()
    key, trust = signing_arguments()
    compiler = ("riscv32" if target == "esp32c3" else "xtensa") + "-esp-elf-gcc"
    return {
        "schema_version": 1, "target": target,
        "idf": {"commit": subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip(),
                "describe": subprocess.check_output(["git", "-C", str(idf), "describe", "--tags", "--dirty"],
                                                    text=True).strip()},
        "tools": {name: tool_version(name) for name in ("cmake", "ninja", compiler)},
        "sources": {name.format(target=target): esp_base.sha256(esp_base.FIRMWARE / name.format(target=target))
                    for name in SOURCES},
        "signing": {"scheme": "rsa-3072-v2", "trust": trust, "public_key_sha256": esp_base.public_key_sha256(key)},
    }


def formal_identity():
    """读取 Fast Deploy 写出的本次身份，核对平台、应用身份与目标架构。"""
    path = os.environ.get("FAST_DEPLOY_IDENTITY_FILE", "")
    if not path or not Path(path).is_absolute():
        esp_base.fail("本入口只能由 Fast Deploy 的 package Job 调用（缺少 FAST_DEPLOY_IDENTITY_FILE）")
    expected = json.loads(Path(path).read_bytes())
    target = expected.get("variant")
    if (target not in ARCHITECTURES or expected.get("platform") != "firmware" or expected.get("application_id") != "esp-base"
            or expected.get("architecture") != ARCHITECTURES[target] or expected.get("local_validation") is not False
            or type(expected.get("build_number")) is not int or expected["build_number"] < 1):
        esp_base.fail("本次身份不是 esp-base 固件的正式身份")
    return target, expected


def directory_files(directory):
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            esp_base.fail("固件包目录只能包含普通文件")
        if path.is_file():
            files[path.relative_to(directory).as_posix()] = path.read_bytes()
    return files


def package_directory(stage, expected, artifact):
    """把已签名并核对的固件目录封装成正式 ZIP；根 build.json 换成正式身份，不覆盖已有不同字节。"""
    import tempfile
    import zipfile
    files = directory_files(stage)
    files["build.json"] = encoded(expected)
    with tempfile.TemporaryDirectory(prefix=".package-", dir=artifact.parent) as temporary:
        candidate = Path(temporary) / "package.zip"
        with zipfile.ZipFile(candidate, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(files.items()):
                item = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                item.create_system = 3
                item.external_attr = 0o100644 << 16
                item.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(item, data)
        with zipfile.ZipFile(candidate) as archive:
            if archive.namelist() != sorted(files) or any(archive.read(name) != data for name, data in files.items()):
                esp_base.fail("最终固件包与封装输入不一致")
        try:
            os.link(candidate, artifact)
        except FileExistsError:
            if artifact.is_symlink() or artifact.read_bytes() != candidate.read_bytes():
                esp_base.fail("同版本构建号已有不同固件包，拒绝覆盖")
    return {"schema_version": 1, "build": expected, "artifact_sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            "artifact_bytes": artifact.stat().st_size}


def package(artifact):
    run_directory = artifact.parent
    target, expected = formal_identity()
    key, trust = signing_arguments()
    frozen = inputs(target)

    # 构建目录放在本次 run 的收据目录里，不改动源码树。
    esp_base.BUILD_ROOT = run_directory / "firmware-build"
    esp_base.build_firmware(target, expected["build_number"], stdout=sys.stderr)
    signed = esp_base.sign_firmware(target, key)
    directory = esp_base.build_dir(target)
    if "CONFIG_ESP_BASE_TEST_NO_BROKER=y" in (directory / "sdkconfig").read_text():
        esp_base.fail("测试开关 ESP_BASE_TEST_NO_BROKER 不能进入正式包")
    version = f'{expected["version"]}+{expected["build_number"]}'
    if version.encode() + b"\0" not in signed.read_bytes()[:1024]:
        esp_base.fail("固件内的版本与 Fast Deploy 分配的不一致")
    if inputs(target) != frozen:
        esp_base.fail("构建期间输入发生变化")

    stage = run_directory / "firmware-package"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    files = {"firmware.bin": signed, "bootloader.bin": directory / "bootloader/bootloader.bin",
             "partition-table.bin": directory / "partition_table/partition-table.bin",
             "ota_data_initial.bin": directory / "ota_data_initial.bin", "esp_base.elf": directory / "esp_base.elf",
             "sdkconfig": directory / "sdkconfig", "LICENSE": esp_base.ROOT / "LICENSE",
             "dependencies.lock": esp_base.FIRMWARE / f"dependencies.lock.{target}"}
    for name, source in files.items():
        shutil.copyfile(source, stage / name)
    flash = {"bootloader/bootloader.bin": "bootloader.bin", "partition_table/partition-table.bin": "partition-table.bin",
             "ota_data_initial.bin": "ota_data_initial.bin", "esp_base.bin": "firmware.bin"}
    offsets = dict(reversed(line.split()) for line in (directory / "flash_args").read_text().splitlines()[1:])
    manifest = {"schema_version": 3, "kind": "esp-base-firmware", "target": f"{target}/esp_base", "version": version,
                "inputs": frozen,
                "flash": {flash[name]: offset for name, offset in offsets.items()},
                "files": {name: {"bytes": (stage / name).stat().st_size, "sha256": esp_base.sha256(stage / name)}
                          for name in sorted(files)}}
    (stage / "manifest.json").write_bytes(encoded(manifest))
    local = {k: v for k, v in expected.items() if k not in ("build_number", "source_commit_sha", "input_sha256")}
    local.update(local_validation=True, run_id="")
    (stage / "build.json").write_bytes(encoded(local))
    for path in stage.iterdir():
        path.chmod(0o644)
    if formal_identity() != (target, expected):
        esp_base.fail("构建期间本次身份发生变化")
    report = package_directory(stage, expected, artifact)
    return {"artifact": report, "firmware_sha256": manifest["files"]["firmware.bin"]["sha256"],
            "version": version, "target": manifest["target"], "signing_trust": trust}


def ensure_idf_environment():
    """Fast Deploy 不导出 IDF：用官方 export.sh 取得环境，再以 IDF 的 Python 重新执行本入口。

    idf.py 与 espsecure 依赖 IDF 自己的 Python 环境，所以不能只补 PATH。
    """
    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    if os.environ.get("IDF_PATH") and venv and Path(sys.prefix).resolve() == Path(venv).resolve():
        return
    idf = Path(os.environ.get("ESP_BASE_IDF_PATH") or IDF_DEFAULT)
    if not (idf / "export.sh").is_file():
        esp_base.fail(f"构建宿主缺少官方 ESP-IDF：{idf}")
    dump = subprocess.run(["bash", "-c", 'source "$1/export.sh" >/dev/null 2>&1 && env -0', "bash", str(idf)],
                          check=True, capture_output=True).stdout.decode()
    environment = dict(item.split("=", 1) for item in dump.split("\0") if "=" in item)
    python = Path(environment["IDF_PYTHON_ENV_PATH"]) / "bin" / "python"
    os.execve(python, [str(python), str(Path(__file__).resolve()), *sys.argv[1:]], environment)


def main():
    ensure_idf_environment()
    if len(sys.argv) == 3 and sys.argv[1] == "inputs":
        sys.stdout.buffer.write(encoded(inputs(sys.argv[2])))
    elif len(sys.argv) == 2:
        sys.stdout.buffer.write(encoded(package(Path(sys.argv[1]))))
    else:
        esp_base.fail("用法：package_entry.py inputs <target>，或由 Fast Deploy 传入 ZIP 路径")


if __name__ == "__main__":
    main()
