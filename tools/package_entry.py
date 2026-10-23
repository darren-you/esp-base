#!/usr/bin/env python3
"""Core 独立打包入口；冻结完整输入后复用原构建及签名，不分配编号。"""
import os
from pathlib import Path
import sys

from build_package import (TARGETS, build, current_inputs, encoded, package, portable_input,
                           read_json, run_inputs, write_once)


def inputs(target):
    if target not in TARGETS:
        raise ValueError("未知固件目标")
    trust = os.environ.get("ESP_BASE_SIGNING_TRUST")
    if trust not in ("software-test", "release"):
        raise ValueError("必须显式声明 ESP_BASE_SIGNING_TRUST")
    paths = []
    for name in ("ESP_BASE_SDKCONFIG", "ESP_BASE_VERIFICATION_KEY"):
        path = Path(os.environ.get(name, ""))
        if not path.is_absolute():
            raise ValueError(name + " 必须为仓外绝对普通文件")
        paths.append(path)
    return current_inputs(target, *paths, trust)


def run(artifact):
    snapshot = Path(os.environ.get("FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE", ""))
    if not snapshot.is_absolute():
        raise ValueError("入口只能由 Core package_run.py 调用")
    directory = snapshot.parent
    request = read_json(directory / "package-run.json")
    value = inputs(request["manifest"]["variant"])
    run_inputs(value)
    if artifact != directory / request["manifest"]["artifact"]:
        raise ValueError("交付路径必须为同次 Core run 指定的原 ZIP")
    frozen = directory / "firmware-input.json"
    write_once(frozen, encoded(value))
    build_directory = directory / "firmware-build"
    build(frozen, build_directory)
    signed = {}
    for name in ("image", "partition", "manifest"):
        variable = "ESP_BASE_SIGNED_" + name.upper()
        if os.environ.get(variable):
            path = Path(os.environ[variable])
            if not path.is_absolute():
                raise ValueError(variable + " 必须为绝对普通文件")
            signed["signed_" + name] = path
    if not value["automatic_signing"] and "signed_manifest" not in signed:
        package(frozen, build_directory, None, signed.get("signed_image"),
                manifest_only=True, signed_partition=signed.get("signed_partition"))
        raise ValueError("外签清单位于原 run 的 firmware-build；签署后设置 ESP_BASE_SIGNED_MANIFEST 并重试同 run")
    return package(frozen, build_directory, artifact, signed.pop("signed_image", None), **signed)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "inputs":
        sys.stdout.buffer.write(encoded(portable_input(inputs(sys.argv[2]))))
    elif len(sys.argv) == 2:
        sys.stdout.buffer.write(encoded(run(Path(sys.argv[1]))))
    else:
        raise ValueError("用法：package_entry.py inputs <target> 或由 Core 传入原 ZIP 路径")


if __name__ == "__main__":
    main()
