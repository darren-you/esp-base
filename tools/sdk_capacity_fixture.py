"""仅测试：真实小 Git 图、两份实际 patch 与 Base 本地配方，不触达仓外 SDK。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


TOOLS = Path(__file__).resolve().parent


class CapacityFixture:
    def __init__(self, root: Path, sdk: Path | None = None):
        self.root = root
        self.product = root / "base-source"
        self.product.mkdir()
        self.sdk = sdk or root / "sdk"
        self.environment = {**os.environ, "GIT_OPTIONAL_LOCKS": "0", "PYTHONDONTWRITEBYTECODE": "1"}
        self.recipe = json.loads((TOOLS.parent / "sdk-lock.json").read_bytes())
        self.original = {}
        self.after = {}
        tlsf_upstream = root / "tlsf-upstream"
        self.init(tlsf_upstream)
        if sdk is None:
            self.init(self.sdk)
            lwip_upstream = root / "lwip-upstream"
            self.init(lwip_upstream)
            (lwip_upstream / "tcp.c").write_text("int original_lwip;\n")
            self.commit(lwip_upstream)
            self.git(self.sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                     str(lwip_upstream), self.recipe["lwip"]["path"])
            extra_upstream = root / "extra-upstream"
            self.init(extra_upstream)
            (extra_upstream / "extra.c").write_text("int extra_source;\n")
            self.commit(extra_upstream)
            self.git(self.sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                     str(extra_upstream), "framework")
            (self.sdk / "sdk.c").write_text("int sdk_good;\n")
        for declaration in self.recipe["managed_patches"]:
            repo = self.sdk if declaration["repository"] == "idf" else tlsf_upstream
            for index, item in enumerate(declaration["files"]):
                file = repo / item["path"]
                file.parent.mkdir(parents=True, exist_ok=True)
                before = f"int capacity_{index} = 1;\n".encode()
                after = f"int capacity_{index} = 2;\n".encode()
                file.write_bytes(before)
                key = declaration["repository"], item["path"]
                self.original[key] = before
                self.after[key] = after
                item["before_sha256"] = hashlib.sha256(before).hexdigest()
                item["after_sha256"] = hashlib.sha256(after).hexdigest()
        self.commit(tlsf_upstream)
        self.git(self.sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(tlsf_upstream), self.recipe["tlsf"]["path"])
        # Preserve an existing graph's intentional lwIP checkout override in its HEAD/index.
        staged = [".gitmodules", self.recipe["tlsf"]["path"]]
        if sdk is None:
            staged += ["sdk.c", self.recipe["lwip"]["path"], "framework"]
        staged += [item["path"] for declaration in self.recipe["managed_patches"]
                   if declaration["repository"] == "idf" for item in declaration["files"]]
        self.git(self.sdk, "add", "--", *staged)
        self.git(self.sdk, "commit", "-qm", "测试SDK容量原件图")
        self.lwip = self.sdk / self.recipe["lwip"]["path"]
        self.tlsf = self.sdk / self.recipe["tlsf"]["path"]
        self.extra = self.sdk / "framework"
        if sdk is None:
            (lwip_upstream / "tcp.c").write_text("int corrected_lwip;\n")
            corrected = self.commit(lwip_upstream)
            self.git(self.lwip, "fetch", "-q", "origin")
            self.git(self.lwip, "checkout", "-q", "--detach", corrected)
        self.recipe["idf"]["revision"] = self.git(self.sdk, "rev-parse", "HEAD")
        self.recipe["lwip"]["revision"] = self.git(self.lwip, "rev-parse", "HEAD")
        self.recipe["tlsf"]["revision"] = self.git(self.tlsf, "rev-parse", "HEAD")
        for declaration in self.recipe["managed_patches"]:
            repo = self.sdk if declaration["repository"] == "idf" else self.tlsf
            for item in declaration["files"]:
                (repo / item["path"]).write_bytes(self.after[declaration["repository"], item["path"]])
            patch = self.run_git(repo, "diff", "--no-ext-diff", "--binary", "HEAD", "--",
                                 *[item["path"] for item in declaration["files"]]).stdout
            file = self.product / declaration["path"]
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(patch)
            declaration["sha256"] = hashlib.sha256(patch).hexdigest()
            for item in declaration["files"]:
                (repo / item["path"]).write_bytes(self.original[declaration["repository"], item["path"]])
        for name in ("check_sdk.py", "prepare_sdk.py"):
            shutil.copyfile(TOOLS / name, self.product / "tools" / name)
        self.git(self.sdk, "remote", "add", "origin", self.recipe["idf"]["repository"])
        self.git(self.lwip, "remote", "set-url", "origin", self.recipe["lwip"]["repository"])
        self.write_recipe()
        self.raw_status = self.git(self.sdk, "status", "--porcelain", "--ignore-submodules=none")

    def run_git(self, path: Path, *args) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "--no-optional-locks", "-C", str(path), *args],
                              check=True, capture_output=True, env=self.environment)

    def git(self, path: Path, *args) -> str:
        return self.run_git(path, *args).stdout.decode().strip()

    def init(self, path: Path) -> None:
        path.mkdir(parents=True)
        self.git(path, "init", "-q", "-b", "master")
        self.git(path, "config", "user.name", "容量来源fixture")
        self.git(path, "config", "user.email", "sdk-capacity@example.invalid")

    def commit(self, path: Path) -> str:
        self.git(path, "add", ".")
        self.git(path, "commit", "-qm", "真实容量来源测试原件")
        return self.git(path, "rev-parse", "HEAD")

    def write_recipe(self, *, stamp: bool = False) -> None:
        raw = (json.dumps(self.recipe, ensure_ascii=False, indent=2) + "\n").encode()
        (self.product / "sdk-lock.json").write_bytes(raw)
        if stamp:
            self.stamp.chmod(0o600)
            self.stamp.write_bytes(raw)
            self.stamp.chmod(0o400)

    @property
    def stamp(self) -> Path:
        return self.sdk / self.recipe["derivation_stamp"]

    def command(self, name: str, *args, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(self.product / "tools" / name), *args],
                              capture_output=True, text=True, env=self.environment, timeout=timeout)

    def prepare(self) -> subprocess.CompletedProcess:
        return self.command("prepare_sdk.py", "--path", str(self.sdk))

    def check(self) -> subprocess.CompletedProcess:
        return self.command("check_sdk.py", "--path", str(self.sdk))

    def apply(self) -> None:
        result = self.prepare()
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
