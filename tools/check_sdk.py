#!/usr/bin/env python3
"""Check the exact public ESP-IDF and lwIP commits selected by ESP Base."""

import argparse
import hashlib
import stat
import json
import os
from pathlib import Path
import subprocess
import tempfile
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
# Official SDK Python imports and their child processes keep sources free of caches.
sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"


def read_source_file(path: Path, *, mode: int | None = None) -> tuple[bytes, tuple]:
    """稳定读取小型配方输入，不跟随链接，不在 FIFO 的 open 上等待。"""
    try:
        before = path.lstat()
    except FileNotFoundError as error:
        raise ValueError(f"容量配方输入缺失：{path}") from error
    if not stat.S_ISREG(before.st_mode) or (mode is not None and stat.S_IMODE(before.st_mode) != mode):
        raise ValueError(f"容量配方输入必须为既定权限的普通文件：{path}")
    if before.st_size > 65536:
        raise ValueError(f"容量配方输入大小越界：{path}")
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(descriptor, "rb") as stream:
        opened = os.fstat(stream.fileno())
        identity = lambda info: (info.st_dev, info.st_ino, info.st_mode, info.st_size,
                                 info.st_mtime_ns, info.st_ctime_ns)
        if identity(opened) != identity(before):
            raise ValueError(f"容量配方输入在打开时改变：{path}")
        content = stream.read(65537)
        after = os.fstat(stream.fileno())
        current = path.lstat()
        if len(content) != before.st_size or identity(after) != identity(before) or identity(current) != identity(before):
            raise ValueError(f"容量配方输入在读取时改变：{path}")
    return content, identity(before)


def unique_object(pairs: list) -> dict:
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"SDK 配方存在重复字段：{name}")
        result[name] = value
    return result


def source_relative_path(value: str) -> str:
    if (not isinstance(value, str) or not value or "\\" in value
            or value.startswith("/") or any(part in ("", ".", "..", ".git") for part in value.split("/"))):
        raise ValueError("SDK 配方必须声明不越界的唯一源码相对路径")
    return value


def exact_fields(value: dict, names: set) -> None:
    if not isinstance(value, dict) or set(value) != names:
        raise ValueError("SDK 配方字段不完整或存在未知字段")


def read_recipe() -> tuple[dict, bytes, tuple]:
    raw, identity = read_source_file(ROOT / "sdk-lock.json")
    recipe = json.loads(raw, object_pairs_hook=unique_object)
    exact_fields(recipe, {"schema_version", "idf", "lwip", "tlsf", "derivation_stamp", "managed_patches"})
    if type(recipe["schema_version"]) is not int or recipe["schema_version"] != 2:
        raise ValueError("SDK lock 必须声明 schema 2 的精确容量派生合同")
    for name, fields in (("idf", {"repository", "revision"}), ("lwip", {"repository", "revision", "path"}),
                         ("tlsf", {"revision", "path"})):
        entry = recipe[name]
        exact_fields(entry, fields)
        if not isinstance(entry["revision"], str) or not re.fullmatch(r"[0-9a-f]{40}", entry["revision"]):
            raise ValueError(f"SDK 配方 {name} 必须锁定完整提交")
        if name != "tlsf" and (not isinstance(entry["repository"], str) or not re.fullmatch(
                r"https://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9-]+\.git", entry["repository"])):
            raise ValueError(f"SDK 配方 {name} 必须声明受控公开来源")
    if recipe["lwip"]["path"] != "components/lwip/lwip" or recipe["tlsf"]["path"] != "components/heap/tlsf":
        raise ValueError("SDK 子来源装配路径与精确容量合同不符")
    if recipe["derivation_stamp"] != "esp-sdk-derivation.json":
        raise ValueError("SDK 派生只接受唯一根 stamp")
    declarations = recipe["managed_patches"]
    if not isinstance(declarations, list) or len(declarations) != 2:
        raise ValueError("SDK 容量派生恰有 idf 和 tlsf 两份补丁")
    seen_repositories, seen_paths = set(), set()
    for declaration in declarations:
        exact_fields(declaration, {"repository", "path", "sha256", "files"})
        name = declaration["repository"]
        if name not in ("idf", "tlsf") or name in seen_repositories:
            raise ValueError("SDK 容量补丁仓库重复或不受支持")
        seen_repositories.add(name)
        if declaration["path"] != f"tools/sdk-patches/capacity-{name}.patch":
            raise ValueError("SDK 容量补丁路径与唯一生产者不符")
        if not isinstance(declaration["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", declaration["sha256"]):
            raise ValueError("SDK 容量补丁摘要无效")
        files = declaration["files"]
        if not isinstance(files, list) or len(files) != (8 if name == "idf" else 3):
            raise ValueError("SDK 容量派生必须恰有八个 IDF 和三个 TLSF 原件")
        for item in files:
            exact_fields(item, {"path", "before_sha256", "after_sha256"})
            relative = source_relative_path(item["path"])
            full = relative if name == "idf" else recipe["tlsf"]["path"] + "/" + relative
            if (full in seen_paths or relative == recipe["derivation_stamp"]
                    or (name == "idf" and (relative == recipe["tlsf"]["path"] or relative.startswith(recipe["tlsf"]["path"] + "/")))):
                raise ValueError("SDK 容量原件路径重复或越入另一来源")
            seen_paths.add(full)
            for field in ("before_sha256", "after_sha256"):
                if not isinstance(item[field], str) or not re.fullmatch(r"[0-9a-f]{64}", item[field]):
                    raise ValueError("SDK 容量原件摘要无效")
    return recipe, raw, identity


def patch_inputs(recipe: dict | None = None) -> list[tuple[dict, Path]]:
    recipe = recipe or read_recipe()[0]
    result = []
    for declaration in recipe["managed_patches"]:
        path = ROOT / declaration["path"]
        parent = path.parent
        while parent != ROOT:
            if not stat.S_ISDIR(parent.lstat().st_mode):
                raise ValueError("SDK 容量补丁父目录不能借用其他来源")
            parent = parent.parent
        raw, _ = read_source_file(path)
        if hashlib.sha256(raw).hexdigest() != declaration["sha256"]:
            raise ValueError(f"SDK 容量补丁摘要与本地配方不符：{declaration['path']}")
        result.append((declaration, path))
    return result


def repository_paths(path: Path) -> dict[str, Path]:
    idf = path.resolve(strict=True)
    return {"idf": idf, "tlsf": idf / "components/heap/tlsf"}


def git_environment() -> dict:
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
                 "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                 "GIT_REPLACE_REF_BASE", "GIT_GRAFT_FILE"):
        if os.environ.get(name):
            raise ValueError(f"Git 环境不能重定向来源或 alternate 对象：{name}")
    # Every Git command reads the actual locked objects, regardless of caller flags.
    return {**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"}


def git(path: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(path), *args], check=True, capture_output=True, text=True, env=git_environment()
    ).stdout.rstrip("\n")


def git_boolean(path: Path, key: str) -> bool:
    """按 Git 的作用域、include 与原生布尔规则读取最终有效值。"""
    result = subprocess.run(
        ["git", "-C", str(path), "config", "--bool", "--get", key],
        text=True, capture_output=True, env=git_environment())
    if result.returncode == 1:
        return False
    value = result.stdout.strip()
    if result.returncode == 0 and value in ("true", "false"):
        return value == "true"
    raise ValueError(f"来源 Git 布尔配置无效：{key}：{path}\n{result.stderr.strip()}")


def verify_object_bytes(path: Path) -> None:
    """重算独立对象库每个对象的原始 Git OID，不改写或忽略历史内容。"""
    object_format = git(path, "rev-parse", "--show-object-format")
    if object_format not in ("sha1", "sha256"):
        raise ValueError("来源 Git 对象摘要格式不受支持")
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(["git", "-C", str(path), "cat-file", "--batch-all-objects",
                                    "--batch", "--unordered"], stdout=subprocess.PIPE,
                                   stderr=errors, env=git_environment())
        try:
            while True:
                header = process.stdout.readline(1024)
                if not header:
                    break
                fields = header.rstrip(b"\n").split()
                if not header.endswith(b"\n") or len(fields) != 3:
                    raise ValueError(f"来源 Git 对象批处理帧不完整：{path}")
                object_id, kind, raw_size = fields
                if kind not in (b"blob", b"tree", b"commit", b"tag") or not raw_size.isdigit():
                    raise ValueError(f"来源 Git 对象类型或长度无效：{path}")
                size = int(raw_size)
                digest = hashlib.new(object_format, kind + b" " + raw_size + b"\0")
                remaining = size
                while remaining:
                    chunk = process.stdout.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError(f"来源 Git 对象原始字节被截断：{path}")
                    digest.update(chunk)
                    remaining -= len(chunk)
                if process.stdout.read(1) != b"\n" or digest.hexdigest().encode() != object_id:
                    raise ValueError(f"来源 Git 对象原始字节与 OID 不符：{path}")
            if process.wait():
                errors.seek(0)
                raise ValueError(f"来源 Git 对象读取失败：{path}\n"
                                 f"{errors.read().decode(errors='replace').strip()}")
        finally:
            process.stdout.close()
            if process.poll() is None:
                process.terminate()
            process.wait()


def verify_tracked_tree(path: Path, source_root: Path, gitlink_overrides: dict, revision: str,
                        capacity_files: dict | None = None, capacity_seen: set | None = None,
                        capacity_patched: bool = False, capacity_stamp: str | None = None) -> None:
    """直接以 HEAD 的 Git blob 字节与类型核对源码，不读取 index 的内容提示。"""
    result = subprocess.run(["git", "-C", str(path), "ls-tree", "-r", "-z", revision],
                            check=True, capture_output=True, env=git_environment())
    object_format = git(path, "rev-parse", "--show-object-format")
    if object_format not in ("sha1", "sha256"):
        raise ValueError("来源 Git 对象摘要格式不受支持")
    tree_entries = [entry for entry in result.stdout.split(b"\0") if entry]
    expected_index = set()
    for entry in tree_entries:
        header, name = entry.split(b"\t", 1)
        mode, _, object_id = header.split()
        expected_index.add((mode, object_id, b"0", name))
    index = subprocess.run(["git", "-C", str(path), "ls-files", "--stage", "-z"],
                           check=True, capture_output=True, env=git_environment())
    actual_index = set()
    for entry in index.stdout.split(b"\0"):
        if entry:
            header, name = entry.split(b"\t", 1)
            mode, object_id, stage = header.split()
            actual_index.add((mode, object_id, stage, name))
    if actual_index != expected_index:
        raise ValueError(f"来源索引与锁定 HEAD 树不符：{path}")
    checked_directories = {path}
    for entry in tree_entries:
        header, raw_name = entry.split(b"\t", 1)
        mode, kind, object_id = header.split()
        relative = Path(os.fsdecode(raw_name))
        if relative.is_absolute() or any(piece in (".", "..") for piece in relative.parts):
            raise ValueError("来源 HEAD 路径不能越出 checkout")
        file = path / relative
        parent = file.parent
        while parent not in checked_directories:
            if not stat.S_ISDIR(parent.lstat().st_mode):
                raise ValueError(f"来源 tracked 父目录类型与 HEAD 不符：{parent}")
            checked_directories.add(parent)
            parent = parent.parent
        actual = file.lstat()
        if mode == b"160000" and kind == b"commit":
            if not stat.S_ISDIR(actual.st_mode):
                raise ValueError(f"来源 gitlink 目录类型与 HEAD 不符：{file}")
            expected = gitlink_overrides.get(file.relative_to(source_root).as_posix(),
                                             object_id.decode("ascii"))
            if git(file, "rev-parse", "HEAD") != expected:
                raise ValueError(f"来源 gitlink 提交与锁定 HEAD 不符：{file}")
            verify_complete_repository(file, source_root=source_root,
                                       gitlink_overrides=gitlink_overrides, revision=expected,
                                       capacity_files=capacity_files, capacity_seen=capacity_seen,
                                       capacity_patched=capacity_patched, capacity_stamp=capacity_stamp)
            continue
        if kind != b"blob" or mode not in (b"100644", b"100755", b"120000"):
            raise ValueError(f"来源 HEAD 包含不支持的文件类型：{file}")
        capacity_item = (capacity_files or {}).get(file.relative_to(source_root).as_posix())
        if capacity_item and (mode != b"100644" or path.resolve() != (
                source_root if capacity_item["repository"] == "idf" else source_root / "components/heap/tlsf")):
            raise ValueError(f"SDK 容量原件必须属于正确仓库的普通非执行 blob：{file}")
        if mode == b"120000":
            if not stat.S_ISLNK(actual.st_mode):
                raise ValueError(f"来源 tracked 符号链接类型与 HEAD 不符：{file}")
            content = os.fsencode(os.readlink(file))
            digest = hashlib.new(object_format, b"blob " + str(len(content)).encode() + b"\0")
            digest.update(content)
        else:
            if (not stat.S_ISREG(actual.st_mode)
                    or bool(actual.st_mode & stat.S_IXUSR) != (mode == b"100755")):
                raise ValueError(f"来源 tracked 文件类型或执行位与 HEAD 不符：{file}")
            digest = hashlib.new(object_format, b"blob " + str(actual.st_size).encode() + b"\0")
            capacity_digest = hashlib.sha256() if capacity_item else None
            descriptor = os.open(file, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            with os.fdopen(descriptor, "rb") as stream:
                opened = os.fstat(stream.fileno())
                if (opened.st_dev, opened.st_ino, opened.st_mode, opened.st_size) != (
                        actual.st_dev, actual.st_ino, actual.st_mode, actual.st_size):
                    raise ValueError(f"来源 tracked 文件在检查期间改变：{file}")
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
                    if capacity_digest is not None:
                        capacity_digest.update(chunk)
                after = os.fstat(stream.fileno())
                if (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (
                        opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns):
                    raise ValueError(f"来源 tracked 文件在检查期间改变：{file}")
            current = file.lstat()
            if (current.st_dev, current.st_ino, current.st_mode) != (
                    actual.st_dev, actual.st_ino, actual.st_mode):
                raise ValueError(f"来源 tracked 文件在检查期间被替换：{file}")
        if capacity_item:
            original = subprocess.run(["git", "-C", str(path), "cat-file", "blob", object_id.decode("ascii")],
                                      check=True, capture_output=True, env=git_environment()).stdout
            if hashlib.sha256(original).hexdigest() != capacity_item["before_sha256"]:
                raise ValueError(f"SDK 容量原件 HEAD blob 与 before 摘要不符：{file}")
            expected = capacity_item["after_sha256"] if capacity_patched else capacity_item["before_sha256"]
            if capacity_digest.hexdigest() != expected:
                raise ValueError(f"SDK 容量原始字节与精确派生摘要不符：{file}")
            capacity_seen.add(file.relative_to(source_root).as_posix())
        if (not capacity_item or not capacity_patched) and digest.hexdigest() != object_id.decode("ascii"):
            raise ValueError(f"来源存在其他修改（未提交）：tracked 原始字节与 HEAD blob 不符：{file}")


def verify_complete_repository(path: Path, source_root: Path | None = None,
                               gitlink_overrides: dict | None = None,
                               revision: str | None = None,
                               capacity_files: dict | None = None, capacity_seen: set | None = None,
                               capacity_patched: bool = False, capacity_stamp: str | None = None) -> None:
    git_environment()
    metadata = path / ".git"
    if metadata.is_symlink() or (metadata.is_dir() and any(
            item.is_symlink() for item in metadata.rglob("*"))):
        raise ValueError(f"来源 Git 元数据及对象库不能包含符号链接：{path}")
    def git_path(*arguments: str) -> Path:
        value = Path(git(path, *arguments))
        return value if value.is_absolute() else path / value

    git_metadata = path / ".git"
    git_directory_path = git_path("rev-parse", "--git-dir")
    if git_metadata.is_symlink() or git_directory_path.is_symlink():
        raise ValueError(f"SDK Git 元数据不能以符号链接借用其他来源：{path}")
    git_directory = git_directory_path.resolve(strict=True)
    if any(item.is_symlink() for item in git_directory.rglob("*")):
        raise ValueError(f"来源 Git 元数据及对象库不能包含符号链接：{path}")
    common_directory = git_path("rev-parse", "--git-common-dir").resolve(strict=True)
    if git_directory != common_directory:
        raise ValueError(f"SDK 来源不能使用借用主仓对象库的 linked worktree：{path}")
    source_root = (source_root or path).resolve(strict=True)
    if not path.resolve().is_relative_to(source_root):
        raise ValueError(f"SDK 子来源必须位于完整根来源目录：{path}")
    if path.resolve() == source_root:
        if not git_directory.is_relative_to(source_root):
            raise ValueError(f"SDK 根 Git 元数据必须位于来源自身目录：{path}")
    elif git_metadata.is_file():
        root_git_directory = Path(git(source_root, "rev-parse", "--absolute-git-dir")).resolve(strict=True)
        if not git_directory.is_relative_to(root_git_directory / "modules"):
            raise ValueError(f"SDK absorbed 子模块 Git 元数据必须归属根来源的 modules：{path}")
    elif git_directory != (path / ".git").resolve(strict=True):
        raise ValueError(f"SDK 子来源必须拥有自身 .git 目录：{path}")
    if git(path, "for-each-ref", "--format=%(refname)", "refs/replace/"):
        raise ValueError(f"SDK 来源不能包含 replace 对象引用：{path}")
    grafts = git_path("rev-parse", "--git-path", "info/grafts")
    if grafts.exists() or grafts.is_symlink():
        raise ValueError(f"SDK 来源不能包含 grafts 历史替换：{path}")
    if git_metadata.is_file():
        binding = subprocess.run(
            ["git", "-C", str(path), "config", "--local", "--path", "--get", "core.worktree"],
            text=True, capture_output=True, env=git_environment())
        if binding.returncode or not binding.stdout.strip():
            raise ValueError(f"SDK Git 元数据文件必须原生绑定当前来源：{path}")
        worktree = Path(binding.stdout.rstrip("\n"))
        if not worktree.is_absolute():
            worktree = git_directory / worktree
        if worktree.resolve() != path.resolve():
            raise ValueError(f"SDK Git 元数据文件指向另一工作树：{path}")
    objects = git_path("rev-parse", "--git-path", "objects")
    if (objects.is_symlink() or not objects.is_dir()
            or objects.resolve() != git_directory / "objects"
            or any(item.is_symlink() for item in objects.rglob("*"))):
        raise ValueError(f"SDK 来源对象库必须归属于该独立仓库，不能以符号链接借用对象：{path}")
    alternate = Path(git(path, "rev-parse", "--git-path", "objects/info/alternates"))
    if not alternate.is_absolute():
        alternate = path / alternate
    if alternate.exists() or alternate.is_symlink():
        raise ValueError(f"SDK 来源不能通过 alternates 借用其他仓库对象：{path}")
    if git(path, "rev-parse", "--show-toplevel") != str(path.resolve()):
        raise ValueError(f"SDK 来源未独立初始化：{path}")
    if git(path, "rev-parse", "--is-shallow-repository") != "false":
        raise ValueError(f"SDK 来源必须保有完整历史，不能使用 shallow clone：{path}")
    config_keys = set(git(path, "config", "--null", "--name-only", "--list").split("\0"))
    for key in sorted(config_keys):
        if key == "extensions.partialclone" or (
                key.startswith("remote.") and key.endswith(".partialclonefilter")):
            raise ValueError(f"SDK 来源不能使用 partial clone：{path}")
        if key.startswith("remote.") and key.endswith(".promisor") and git_boolean(path, key):
            raise ValueError(f"SDK 来源不能使用 partial clone：{path}")
    # Git resolves include, scope precedence and every accepted boolean spelling.
    # Cone only selects a mode; it does not enable sparse checkout on its own.
    if git_boolean(path, "core.sparseCheckout"):
        raise ValueError(f"SDK 来源不能使用 sparse checkout：{path}")
    revision = revision or git(path, "rev-parse", "HEAD")
    if git(path, "rev-parse", "HEAD") != revision:
        raise ValueError(f"来源未锁定完整提交 {revision}：{path}")
    result = subprocess.run(["git", "-C", str(path), "fsck", "--connectivity-only",
                             "--no-dangling"], text=True, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=git_environment())
    if result.returncode:
        raise ValueError(f"SDK 来源对象不完整：{path}\n{result.stderr.strip()}")

    verify_object_bytes(path)
    verify_tracked_tree(path, source_root, gitlink_overrides or {}, revision,
                        capacity_files, capacity_seen, capacity_patched, capacity_stamp)
    others = {name for name in git(path, "ls-files", "--others", "-z").split("\0") if name}
    if capacity_patched and capacity_stamp and path.resolve() == source_root:
        others.discard(capacity_stamp)
    if others:
        raise ValueError(f"来源存在其他修改（含 ignored 的未提交 untracked 内容）：{path}")



def check(path: Path, *, patched: bool = True) -> None:
    recipe, raw, recipe_identity = read_recipe()
    patches = patch_inputs(recipe)
    patch_snapshots = {}
    for declaration, file in patches:
        snapshot = read_source_file(file)
        if hashlib.sha256(snapshot[0]).hexdigest() != declaration["sha256"]:
            raise ValueError("SDK 容量补丁在稳定快照前改变")
        patch_snapshots[file] = snapshot
    idf = path.resolve(strict=True)
    lwip = idf / recipe["lwip"]["path"]
    if git(idf, "rev-parse", "HEAD") != recipe["idf"]["revision"]:
        raise ValueError("ESP-IDF 提交与 sdk-lock.json 不符")
    if (not lwip.is_dir()
            or git(lwip, "rev-parse", "--show-toplevel") != str(lwip.resolve())
            or git(lwip, "rev-parse", "HEAD") != recipe["lwip"]["revision"]):
        raise ValueError("lwIP 提交与 sdk-lock.json 不符")
    tlsf_entry = git(idf, "ls-tree", recipe["idf"]["revision"], "--", recipe["tlsf"]["path"])
    expected_entry = "160000 commit " + recipe["tlsf"]["revision"] + "\t" + recipe["tlsf"]["path"]
    if tlsf_entry != expected_entry:
        raise ValueError("TLSF 必须保持锁定 IDF 的原生 gitlink 身份")
    stamp = idf / recipe["derivation_stamp"]
    if patched:
        stamp_snapshot = read_source_file(stamp, mode=0o400)
        if stamp_snapshot[0] != raw:
            raise ValueError("SDK derivation stamp 与 canonical 精确配方不符")
    else:
        try:
            stamp.lstat()
        except FileNotFoundError:
            pass
        else:
            raise ValueError("装配前 SDK 原件不能含任何 derivation stamp")
    capacity_files = {}
    for declaration, _ in patches:
        for item in declaration["files"]:
            relative = item["path"] if declaration["repository"] == "idf" else recipe["tlsf"]["path"] + "/" + item["path"]
            capacity_files[relative] = {**item, "repository": declaration["repository"]}
    seen = set()
    verify_complete_repository(idf, gitlink_overrides={recipe["lwip"]["path"]: recipe["lwip"]["revision"]},
                               revision=recipe["idf"]["revision"], capacity_files=capacity_files,
                               capacity_seen=seen, capacity_patched=patched,
                               capacity_stamp=recipe["derivation_stamp"] if patched else None)
    if seen != set(capacity_files):
        raise ValueError("SDK 容量原件未被真实完整 HEAD 树唯一消费")
    if read_source_file(ROOT / "sdk-lock.json") != (raw, recipe_identity):
        raise ValueError("canonical SDK 配方在完整来源检查期间改变")
    for file, snapshot in patch_snapshots.items():
        if read_source_file(file) != snapshot:
            raise ValueError("SDK 容量补丁在完整来源检查期间改变")
    if patched:
        if read_source_file(stamp, mode=0o400) != stamp_snapshot:
            raise ValueError("SDK derivation stamp 在完整来源检查期间改变")
    else:
        try:
            stamp.lstat()
        except FileNotFoundError:
            pass
        else:
            raise ValueError("原件完整检查期间出现 derivation stamp")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    try:
        check(args.path)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"ESP Base SDK 检查失败：{error}\n")
    print("ESP Base SDK 完整来源、原件与精确容量派生符合 sdk-lock.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
