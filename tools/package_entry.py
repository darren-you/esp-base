#!/usr/bin/env python3
"""正式构建入口，由 Fast Deploy Core 的 shared/package_run.py 调用：构建、签名、验签并封装成 ZIP。

环境变量：
  ESP_BASE_SIGNING_KEY_FILE 或 ESP_BASE_SIGNING_KEY_ENV（FILE:VAR）  仓外的 RSA-3072 签名键
  ESP_BASE_SIGNING_TRUST    release 或 software-test；后者不得用于正式设备
版本和构建号由 Core 分配，本入口不分配编号，也不连接设备。
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
    """冻结进 Core 收据的输入：官方 SDK 提交、工具链、锁与配置的摘要、签名信任根。"""
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


def package(artifact):
    core = Path(os.environ.get("FAST_DEPLOY_PACKAGE_CORE_ROOT", ""))
    run_directory = artifact.parent
    request = json.loads((run_directory / "package-run.json").read_text())
    target = request["manifest"]["variant"]
    if not core.is_absolute() or target not in ARCHITECTURES:
        esp_base.fail("本入口只能由 Core 的 package_run.py 调用")
    identity = [sys.executable, core / "shared/package_identity.py", "--project-root", esp_base.ROOT,
                "--platform", "firmware", "--application-id", "esp-base",
                "--architecture", ARCHITECTURES[target], "--variant", target]
    expected = json.loads(subprocess.check_output([str(part) for part in [*identity[:2], "resolve", *identity[2:]]]))
    key, trust = signing_arguments()
    frozen = inputs(target)

    # 构建目录放在本次 run 的收据目录里，不改动源码树。
    esp_base.BUILD_ROOT = run_directory / "firmware-build"
    esp_base.build_firmware(target, expected["build_number"], stdout=sys.stderr)
    signed = esp_base.sign_firmware(target, key)
    directory = esp_base.build_dir(target)
    version = f'{expected["version"]}+{expected["build_number"]}'
    if version.encode() + b"\0" not in signed.read_bytes()[:1024]:
        esp_base.fail("固件内的版本与 Core 分配的不一致")
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
    report = subprocess.check_output([str(part) for part in [
        *identity[:2], "package-directory", *identity[2:], "--directory", stage, "--artifact", artifact]])
    return {"artifact": json.loads(report), "firmware_sha256": manifest["files"]["firmware.bin"]["sha256"],
            "version": version, "target": manifest["target"], "signing_trust": trust}


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "inputs":
        sys.stdout.buffer.write(encoded(inputs(sys.argv[2])))
    elif len(sys.argv) == 2:
        sys.stdout.buffer.write(encoded(package(Path(sys.argv[1]))))
    else:
        esp_base.fail("用法：package_entry.py inputs <target>，或由 Core 传入 ZIP 路径")


if __name__ == "__main__":
    main()
