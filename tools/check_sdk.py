#!/usr/bin/env python3
"""Check the exact public ESP-IDF and lwIP commits selected by ESP Base."""

import argparse
import json
from pathlib import Path
import subprocess


LOCK = json.loads((Path(__file__).resolve().parent.parent / "sdk-lock.json").read_text())


def git(path: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(path), *args], check=True, capture_output=True, text=True
    ).stdout.rstrip("\n")


def check(path: Path) -> None:
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
    if git(idf, "diff", "--cached", "--name-only"):
        raise ValueError("ESP-IDF index contains uncommitted changes")
    status = git(idf, "status", "--porcelain", "--untracked-files=normal",
                 "--ignore-submodules=none")
    if status != f" M {LOCK['lwip']['path']}":
        raise ValueError("ESP-IDF checkout differs beyond the locked lwIP gitlink")
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
    print("ESP Base SDK 与 sdk-lock.json 一致")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
