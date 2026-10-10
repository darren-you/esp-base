#!/usr/bin/env python3
"""核对官方基线、锁定子模块和 ESP Base 唯一受管容量统计补丁。"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / "sdk-lock.json").read_text())


def git(path: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(path), *args], check=True, capture_output=True, text=True
    ).stdout.rstrip("\n")


def patch_inputs() -> list[tuple[dict, Path]]:
    if LOCK["schema_version"] != 2:
        raise ValueError("SDK lock does not declare the managed capacity contract")
    result = []
    for patch in LOCK["managed_patches"]:
        path = ROOT / patch["path"]
        if path.is_symlink() or not path.is_file():
            raise ValueError("Managed SDK patch must be a regular source file")
        if hashlib.sha256(path.read_bytes()).hexdigest() != patch["sha256"]:
            raise ValueError(f"Managed SDK patch digest differs: {patch['path']}")
        result.append((patch, path))
    if {patch["repository"] for patch, _ in result} != {"idf", "tlsf"} or len(result) != 2:
        raise ValueError("Managed SDK patch repositories differ")
    return result


def repository_paths(path: Path) -> dict[str, Path]:
    idf = path.resolve(strict=True)
    return {"idf": idf, "tlsf": idf / LOCK["tlsf"]["path"]}


def check(path: Path, *, patched: bool = True) -> None:
    idf = path.resolve(strict=True)
    lwip = idf / LOCK["lwip"]["path"]
    if git(idf, "rev-parse", "HEAD") != LOCK["idf"]["revision"]:
        raise ValueError("ESP-IDF commit differs from sdk-lock.json")
    if (not lwip.is_dir()
            or git(lwip, "rev-parse", "--show-toplevel") != str(lwip.resolve())
            or git(lwip, "rev-parse", "HEAD") != LOCK["lwip"]["revision"]):
        raise ValueError("ESP lwIP commit differs from sdk-lock.json")
    if git(lwip, "status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("ESP lwIP checkout contains uncommitted changes")
    repositories = repository_paths(idf)
    stamp = idf / LOCK["derivation_stamp"]
    if patched and (stamp.is_symlink() or not stamp.is_file()
                    or stamp.read_bytes() != (ROOT / "sdk-lock.json").read_bytes()):
        raise ValueError("SDK derivation stamp differs from the canonical SDK recipe")
    tlsf = repositories["tlsf"]
    if (git(tlsf, "rev-parse", "--show-toplevel") != str(tlsf.resolve())
            or git(tlsf, "rev-parse", "HEAD") != LOCK["tlsf"]["revision"]):
        raise ValueError("TLSF differs from the official locked IDF gitlink")
    for patch, _ in patch_inputs():
        repo = repositories[patch["repository"]]
        if git(repo, "diff", "--cached", "--name-only"):
            raise ValueError(f"SDK index contains changes: {patch['repository']}")
        allowed = {item["path"] for item in patch["files"]} if patched else set()
        if patch["repository"] == "idf":
            allowed.add(LOCK["lwip"]["path"])
            if patched:
                allowed.add(LOCK["tlsf"]["path"])
        lines = git(repo, "status", "--porcelain", "--untracked-files=normal",
                    "--ignore-submodules=none").splitlines()
        expected_status = {" M " + name for name in allowed}
        if patched and patch["repository"] == "idf":
            expected_status.add("?? " + LOCK["derivation_stamp"])
        if set(lines) != expected_status:
            raise ValueError(f"SDK differs beyond its exact managed inputs: {patch['repository']}")
        for item in patch["files"]:
            original = subprocess.run(["git", "-C", str(repo), "show", f"HEAD:{item['path']}"],
                                      check=True, capture_output=True).stdout
            file = repo / item["path"]
            if file.is_symlink() or not file.is_file():
                raise ValueError(f"SDK input is not a regular file: {item['path']}")
            if hashlib.sha256(original).hexdigest() != item["before_sha256"]:
                raise ValueError(f"Official SDK source digest differs: {item['path']}")
            expected = item["after_sha256"] if patched else item["before_sha256"]
            if hashlib.sha256(file.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Managed SDK source digest differs: {item['path']}")
    for line in git(idf, "submodule", "status", "--recursive").splitlines():
        parts = line.strip().split()
        if len(parts) < 2:
            raise ValueError("ESP-IDF submodule status is invalid")
        revision, submodule_path = parts[:2]
        if revision.startswith(("-", "U")):
            raise ValueError(f"ESP-IDF submodule is unavailable: {submodule_path}")
        if revision.startswith("+") and (submodule_path != LOCK["lwip"]["path"]
                                         or revision[1:] != LOCK["lwip"]["revision"]):
            raise ValueError(f"ESP-IDF submodule differs from its gitlink: {submodule_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    try:
        check(args.path)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"ESP Base SDK 检查失败：{error}\n")
    print("ESP Base SDK 官方基线、子模块和受管容量统计补丁与 sdk-lock.json 一致")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
