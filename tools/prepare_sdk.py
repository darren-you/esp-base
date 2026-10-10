#!/usr/bin/env python3
"""在独立、锁定的 SDK checkout 应用本仓确定性容量统计补丁；不访问设备。"""

import argparse
import os
from pathlib import Path
import subprocess

from check_sdk import LOCK, ROOT, check, patch_inputs, repository_paths


def prepare(path: Path) -> None:
    try:
        check(path)
    except (ValueError, subprocess.CalledProcessError):
        check(path, patched=False)
    else:
        return
    repositories = repository_paths(path)
    patches = patch_inputs()
    # All repositories are checked before the first mutation. Partial/unknown states reject.
    for patch, file in patches:
        subprocess.run(["git", "-C", str(repositories[patch["repository"]]), "apply",
                        "--check", "--whitespace=nowarn", str(file)], check=True)
    for patch, file in patches:
        subprocess.run(["git", "-C", str(repositories[patch["repository"]]), "apply",
                        "--whitespace=nowarn", str(file)], check=True)
    stamp = repositories["idf"] / LOCK["derivation_stamp"]
    fd = os.open(stamp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write((ROOT / "sdk-lock.json").read_bytes())
        stream.flush()
        os.fsync(stream.fileno())
    stamp.chmod(0o400)
    check(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    try:
        prepare(args.path)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ESP Base SDK 装配失败：{error}\n")
    print("ESP Base 受管容量统计 SDK 已装配并核对；官方基线及修改摘要均保留")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
