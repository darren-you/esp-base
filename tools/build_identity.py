#!/usr/bin/env python3
"""将同次 Core 身份编译进固件；普通 IDF 构建明确标记为本地验证。"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {"esp32c3": ("riscv32", 5), "esp32": ("xtensa", 0)}
MAGIC = 0x31424245
DESCRIPTOR = struct.Struct("<IIQII32s41s65s128s16s14s")
MAX_NUMBER = 9007199254740991


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def read_json(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("身份输入必须为普通文件")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("身份输入有重复字段")
            result[key] = value
        return result
    return json.loads(path.read_bytes(), object_pairs_hook=pairs)


def resolve(target, suffix="", root=ROOT):
    release = read_json(root / "release.json")
    if (set(release) != {"version"} or type(release["version"]) is not str or
            not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", release["version"])):
        raise ValueError("release.json 必须声明唯一产品版本")
    if suffix not in ("", "-capacity-lab"):
        raise ValueError("未知本地观察版本后缀")
    value = dict(version=release["version"], platform="firmware", application_id="esp-base",
                 architecture=TARGETS[target][0], variant=target, local_validation=True, run_id="")
    if any(os.environ.get(key) for key in (
            "FAST_DEPLOY_RUN_ID", "FAST_DEPLOY_CLIENT_VERSION_SNAPSHOT_FILE",
            "FAST_DEPLOY_PACKAGE_CORE_ROOT", "FAST_DEPLOY_SOURCE_REF",
            "FAST_DEPLOY_RESOLVED_COMMIT_SHA", "FAST_DEPLOY_CORE_COMMIT_SHA")):
        core = Path(os.environ.get("FAST_DEPLOY_PACKAGE_CORE_ROOT", ""))
        if not core.is_absolute():
            raise ValueError("正式输入必须显式指定冻结 FAST_DEPLOY_PACKAGE_CORE_ROOT")
        command = [sys.executable, str(core / "shared/package_identity.py"), "resolve",
                   "--project-root", str(root), "--platform", "firmware", "--application-id", "esp-base",
                   "--architecture", value["architecture"], "--variant", target]
        value = json.loads(subprocess.check_output(command))
        if value.get("local_validation") is not False or value.get("version") != release["version"] or suffix:
            raise ValueError("正式固件不得回落本地身份或消费 LAB 版本")
        # IDF discovers component sources and CMake input from these directories.
        # Core already checks tracked content; ignored/untracked files must not
        # silently become additional first-party compiler inputs.
        files = []
        for directory in ("firmware/apps", "firmware/components", "firmware/partitions", "tools"):
            for path in (root / directory).rglob("*"):
                if path.is_symlink():
                    raise ValueError("正式固件第一方输入不能通过链接读取")
                if path.is_file() and (path.suffix in (".c", ".cpp", ".h", ".hpp", ".S", ".s",
                                                      ".cmake", ".py", ".csv", ".yml") or
                                       path.name == "CMakeLists.txt" or path.name.startswith("Kconfig")):
                    files.append(path.relative_to(root).as_posix())
        if subprocess.run(["git", "-C", str(root), "ls-files", "--error-unmatch", "--", *files],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            raise ValueError("正式固件第一方编译输入必须全部属于冻结 Git")
    else:
        value["version"] += suffix
    descriptor_bytes(value, target)
    return value


def fixed(text, size):
    raw = text.encode("ascii")
    if len(raw) >= size or b"\0" in raw:
        raise ValueError("固件身份字段超过原生容量或含 NUL")
    return raw.ljust(size, b"\0")


def descriptor_bytes(value, target):
    local = value.get("local_validation")
    keys = {"version", "platform", "application_id", "architecture", "variant", "local_validation", "run_id"}
    if local is False:
        keys |= {"build_number", "source_commit_sha", "input_sha256"}
    if (type(local) is not bool or set(value) != keys or value["platform"] != "firmware" or
            value["application_id"] != "esp-base" or value["variant"] != target or
            value["architecture"] != TARGETS[target][0] or
            type(value["version"]) is not str or
            not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-capacity-lab)?", value["version"])):
        raise ValueError("固件身份、版本或实际目标不一致")
    number = value.get("build_number", 0)
    if local:
        if value["run_id"] != "":
            raise ValueError("本地身份不能携带正式 run")
    elif (type(number) is not int or not 1 <= number <= MAX_NUMBER or
          "-" in value["version"] or
          not re.fullmatch(r"[a-z0-9_][a-z0-9_.-]*", value["run_id"], re.ASCII) or
          not re.fullmatch(r"[a-f0-9]{40}", value["source_commit_sha"]) or
          not re.fullmatch(r"[a-f0-9]{64}", value["input_sha256"])):
        raise ValueError("正式固件编号或冻结来源无效")
    return DESCRIPTOR.pack(MAGIC, DESCRIPTOR.size, number, 1, int(local),
                           fixed(value["version"], 32), fixed(value.get("source_commit_sha", ""), 41),
                           fixed(value.get("input_sha256", ""), 65), fixed(value["run_id"], 128),
                           fixed(target, 16), b"\0" * 14)


def generated(value, target):
    # Values are ASCII, bounded and validated before becoming C literals.
    descriptor_bytes(value, target)
    quote = json.dumps
    source = ('// Generated from the current build identity; do not edit.\n'
              '#include "esp_base_build.h"\n'
              '#ifdef __APPLE__\n'
              '#define BUILD_SECTION "__DATA_CONST,__esp_base_build"\n'
              '#else\n#define BUILD_SECTION ".rodata_custom_desc"\n#endif\n'
              'const __attribute__((section(BUILD_SECTION), used)) esp_base_build_t esp_base_build = {\n'
              f'    .magic = {MAGIC}U, .size = 320U, .schema_version = 1U,\n'
              f'    .build_number = UINT64_C({value.get("build_number", 0)}),\n'
              f'    .local_validation = {int(value["local_validation"])}U,\n')
    for field in ("version", "source_commit_sha", "input_sha256", "run_id"):
        source += f'    .{field} = {quote(value.get(field, ""))},\n'
    source += f'    .target = {quote(target)},\n}};\n'
    return {"build.json": encoded(value), "esp_base_build.c": source.encode(),
            "identity.cmake": f'set(PROJECT_VER "{value["version"]}")\n'.encode()}


def prepare(directory, value, target, verify=False):
    if directory.is_symlink():
        raise ValueError("生成目录不能为链接")
    if not verify:
        directory.mkdir(parents=True, exist_ok=True)
    for name, data in generated(value, target).items():
        path = directory / name
        if path.is_symlink():
            raise ValueError("生成文件不能为链接")
        if path.exists() and path.read_bytes() == data:
            continue
        if verify:
            raise ValueError("本次构建身份改变或生成字节被修改，请重新 configure")
        path.write_bytes(data)


def inspect_image(path, value, target):
    """Read the descriptor at the SDK's fixed first-DROM offset, never scan for a marker.

    This checks identity and image geometry, not cryptographic signature validity.
    The caller must separately run the official verifier on complete signed bytes.
    """
    data = path.read_bytes()
    if len(data) < 32 + 256 + DESCRIPTOR.size or data[0] != 0xE9 or not 1 <= data[1] <= 16:
        raise ValueError("固件镜像头或长度无效")
    if int.from_bytes(data[12:14], "little") != TARGETS[target][1]:
        raise ValueError("固件真实芯片与目标不同")
    first_size = int.from_bytes(data[28:32], "little")
    if first_size < 256 + DESCRIPTOR.size:
        raise ValueError("固件首段没有完整身份")
    offset = 24
    for _ in range(data[1]):
        if offset + 8 > len(data):
            raise ValueError("固件段头截断")
        length = int.from_bytes(data[offset + 4:offset + 8], "little")
        offset += 8 + length
        if offset > len(data):
            raise ValueError("固件段正文截断")
    if int.from_bytes(data[32:36], "little") != 0xABCD5432:
        raise ValueError("固件原生 app 描述无效")
    if data[48:80] != fixed(value["version"], 32) or data[80:112] != fixed("esp_base", 32):
        raise ValueError("固件原生产品或版本与本次输入不同")
    if data[288:288 + DESCRIPTOR.size] != descriptor_bytes(value, target):
        raise ValueError("固件内嵌身份与本次输入不同")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=TARGETS)
    parser.add_argument("--suffix", default="")
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    try:
        value = resolve(args.target, args.suffix)
        prepare(args.directory, value, args.target, args.verify or args.image is not None)
        if args.image:
            inspect_image(args.image, value, args.target)
        print(json.dumps(value))
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, "固件身份失败：" + str(error) + "\n")


if __name__ == "__main__":
    main()
