#!/usr/bin/env python3
"""在独立锁定 SDK 装配本仓精确容量补丁；完整原件和派生检查不访问设备。"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

from check_sdk import (apply_git_command, check, git_environment, patch_inputs, read_recipe,
                       read_source_file, reject_builtin_content_conversions, reject_external_content_filters, repository_paths)


def prepare(path: Path) -> None:
    try:
        check(path)
    except (ValueError, subprocess.CalledProcessError):
        check(path, patched=False)
    else:
        return
    recipe, raw, identity = read_recipe()
    repositories = repository_paths(path)
    for declaration in recipe["managed_patches"]:
        reject_external_content_filters(repositories[declaration["repository"]],
                                        [item["path"] for item in declaration["files"]])
    patches = patch_inputs(recipe)
    # Git reads the two frozen verified inputs, not paths that could change after hashing.
    with tempfile.TemporaryDirectory(prefix="esp-base-capacity-patches-") as directory:
        frozen = []
        for declaration, file in patches:
            content, _ = read_source_file(file)
            if hashlib.sha256(content).hexdigest() != declaration["sha256"]:
                raise ValueError("SDK 容量补丁在冻结前改变")
            patch = Path(directory) / file.name
            patch.write_bytes(content)
            patch.chmod(0o400)
            frozen.append((declaration, patch))
        if read_recipe()[1:] != (raw, identity):
            raise ValueError("SDK 配方在装配前改变")
        # Both real Git apply checks complete before the first SDK mutation.
        for declaration, file in frozen:
            repository = repositories[declaration["repository"]]
            reject_builtin_content_conversions(repository, [item["path"] for item in declaration["files"]], file.read_bytes())
            subprocess.run(apply_git_command(repository, "apply",
                                       "--check", "--whitespace=nowarn", str(file)), check=True, env=git_environment())
        for declaration, file in frozen:
            subprocess.run(apply_git_command(repositories[declaration["repository"]], "apply",
                                       "--whitespace=nowarn", str(file)), check=True, env=git_environment())
    stamp = repositories["idf"] / recipe["derivation_stamp"]
    fd = os.open(stamp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(raw)
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
    print("ESP Base 完整来源及精确容量派生已装配并核对")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
