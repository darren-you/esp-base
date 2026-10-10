"""用真实 Git 对象库验证 SDK 来源不得借用外部对象。"""
import importlib.util
import hashlib
import json
import sys
import os
from pathlib import Path
import subprocess
import shutil
import shlex
import tempfile
import unittest
from unittest.mock import patch
from sdk_capacity_fixture import CapacityFixture

SPEC = importlib.util.spec_from_file_location("check_sdk", Path(__file__).with_name("check_sdk.py"))
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)


class CapacityFixtureIsolationTest(unittest.TestCase):
    def anonymous_environment(self, root):
        environment = dict(os.environ)
        for name in list(environment):
            if name.startswith(("GIT_CONFIG", "GIT_AUTHOR_", "GIT_COMMITTER_")) or name == "EMAIL":
                environment.pop(name)
        environment.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                           GIT_CONFIG_SYSTEM="/dev/null", GIT_CONFIG_GLOBAL="/dev/null")
        return environment

    def test_fixture_ignores_inherited_system_global_and_command_filters(self):
        for scope in ("system", "global", "command"):
            with self.subTest(scope=scope), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                configuration = root / "host-gitconfig"
                configuration.write_text("[filter \"lfs\"]\n\tclean = cat\n\tsmudge = cat\n")
                environment = self.anonymous_environment(root)
                if scope == "command":
                    environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="filter.lfs.clean", GIT_CONFIG_VALUE_0="cat")
                else:
                    environment["GIT_CONFIG_" + scope.upper()] = str(configuration)
                with patch.dict(os.environ, environment, clear=True):
                    capacity = CapacityFixture(root)
                    capacity.apply()
                    self.assertEqual(capacity.check().returncode, 0)

    def test_managed_wrong_child_commit_uses_owned_identity_without_host_identity(self):
        import test_managed_sdk
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.dict(os.environ, self.anonymous_environment(root), clear=True):
                capacity = CapacityFixture(root)
                capacity.apply()
                case = test_managed_sdk.ManagedSDKTests("test_unavailable_or_wrong_other_submodule")
                case.root = root
                case.capacity = capacity
                try:
                    case.test_unavailable_or_wrong_other_submodule()
                except subprocess.CalledProcessError as error:
                    self.fail("匿名夹具原生子仓 commit 失败：" + error.stderr.decode(errors="replace"))


class SDKObjectOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        self.git(self.source, "init", "-q", "-b", "master")
        self.git(self.source, "config", "user.name", "SDK fixture")
        self.git(self.source, "config", "user.email", "sdk@example.invalid")
        (self.source / "source.c").write_text("int source;\n")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "-qm", "fixture")

    @staticmethod
    def git(path, *args):
        return subprocess.run(["git", "-C", str(path), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def test_accepts_complete_self_owned_source(self):
        SDK.verify_complete_repository(self.source)

    def test_rejects_shared_clone_even_when_fsck_passes(self):
        shared = self.root / "shared"
        self.git(self.root, "clone", "-q", "--shared", str(self.source), str(shared))
        self.git(shared, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "alternates"):
            SDK.verify_complete_repository(shared)

    def test_rejects_symlinked_git_directory_even_when_fsck_passes(self):
        alias = self.root / "symlinked-git-directory"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        (alias / ".git").symlink_to(self.source / ".git", target_is_directory=True)
        self.git(alias, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(alias)

    def test_rejects_unbound_gitfile_even_when_fsck_passes(self):
        alias = self.root / "unbound-gitfile"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        (alias / ".git").write_text("gitdir: " + str(self.source / ".git") + "\n")
        self.git(alias, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(alias)

    def test_rejects_linked_worktree_even_when_fsck_passes(self):
        linked = self.root / "linked"
        self.git(self.source, "worktree", "add", "-q", "--detach", str(linked), "HEAD")
        try:
            self.git(linked, "fsck", "--connectivity-only", "--no-dangling")
            with self.assertRaisesRegex(ValueError, "linked"):
                SDK.verify_complete_repository(linked)
        finally:
            self.git(self.source, "worktree", "remove", str(linked))

    def test_rejects_symlinked_object_storage_even_when_fsck_passes(self):
        objects = self.source / ".git/objects"
        outside = self.root / "outside-objects"
        objects.rename(outside)
        objects.symlink_to(outside, target_is_directory=True)
        self.git(self.source, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "对象库"):
            SDK.verify_complete_repository(self.source)

    def test_accepts_normal_absorbed_submodule_gitfile(self):
        parent = self.root / "parent"
        parent.mkdir()
        self.git(parent, "init", "-q", "-b", "master")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "module")
        module = parent / "module"
        self.assertTrue((module / ".git").is_file())
        SDK.verify_complete_repository(module, source_root=parent)

    def test_rejects_ignored_dirty_recursive_submodule(self):
        sdk = self.root / "sdk"
        framework = self.root / "framework"
        for path in (sdk, framework):
            path.mkdir()
            self.git(path, "init", "-q", "-b", "master")
            self.git(path, "config", "user.name", "SDK fixture")
            self.git(path, "config", "user.email", "sdk@example.invalid")
        self.git(framework, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "leaf")
        self.git(framework, "add", ".")
        self.git(framework, "commit", "-qm", "nested framework")
        for source, relative in ((self.source, "components/lwip/lwip"), (framework, "framework")):
            self.git(sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                     str(source), relative)
        self.git(sdk, "add", ".")
        self.git(sdk, "commit", "-qm", "SDK fixture")
        sdk_revision = self.git(sdk, "rev-parse", "HEAD")
        (self.source / "source.c").write_text("corrected source\n")
        self.git(self.source, "add", ".")
        self.git(self.source, "commit", "-qm", "lwIP correction")
        corrected = self.git(self.source, "rev-parse", "HEAD")
        lwip = sdk / "components/lwip/lwip"
        self.git(lwip, "fetch", "-q", "origin")
        self.git(lwip, "checkout", "-q", "--detach", corrected)
        self.git(sdk, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
        # submodule update resets lwIP; restore the single intentional override.
        self.git(lwip, "checkout", "-q", "--detach", corrected)
        capacity_root = self.root / "capacity-contract"
        capacity_root.mkdir()
        capacity = CapacityFixture(capacity_root, sdk=sdk)
        capacity.apply()
        with patch.object(SDK, "ROOT", capacity.product):
            SDK.check(sdk)
            self.git(sdk, "config", "submodule.framework.ignore", "all")
            self.git(sdk / "framework", "config", "submodule.leaf.ignore", "all")
            (sdk / "framework/leaf/source.c").write_text("unverified source\n")
            with self.assertRaises(ValueError):
                SDK.check(sdk)

    def test_rejects_environment_alternate_objects(self):
        with patch.dict(os.environ, {"GIT_ALTERNATE_OBJECT_DIRECTORIES":
                                    str(self.source / ".git/objects")}):
            with self.assertRaisesRegex(ValueError, "alternate"):
                SDK.verify_complete_repository(self.source)

    def test_rejects_environment_object_directory(self):
        with patch.dict(os.environ, {"GIT_OBJECT_DIRECTORY": str(self.source / ".git/objects")}):
            with self.assertRaisesRegex(ValueError, "alternate"):
                SDK.verify_complete_repository(self.source)

    def test_rejects_external_separate_git_directory_with_worktree_binding(self):
        separate = self.root / "separate"
        outside = self.root / "separate.git"
        self.git(self.root, "clone", "-q", "--separate-git-dir=" + str(outside),
                 str(self.source), str(separate))
        self.git(separate, "config", "core.worktree", str(separate))
        self.assertEqual(self.git(separate, "status", "--porcelain"), "")
        self.git(separate, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(separate)

    def test_rejects_git_environment_redirecting_metadata_and_worktree(self):
        alias = self.root / "environment-redirect"
        shutil.copytree(self.source, alias, ignore=shutil.ignore_patterns(".git"))
        with patch.dict(os.environ, {"GIT_DIR": str(self.source / ".git"),
                                    "GIT_WORK_TREE": str(alias)}):
            with self.assertRaisesRegex(ValueError, "Git 环境"):
                SDK.verify_complete_repository(alias)

    def test_rejects_replace_ref_even_when_head_and_status_match(self):
        original = self.git(self.source, "rev-parse", "HEAD")
        filename = self.source / "source.c"
        original_bytes = filename.read_bytes()
        filename.write_text("int substituted_business;\n")
        self.git(self.source, "add", filename.name)
        self.git(self.source, "commit", "-qm", "different source fixture")
        replacement = self.git(self.source, "rev-parse", "HEAD")
        self.git(self.source, "replace", original, replacement)
        self.git(self.source, "checkout", "-q", "--detach", original)
        self.assertEqual(self.git(self.source, "rev-parse", "HEAD"), original)
        self.assertEqual(self.git(self.source, "status", "--porcelain"), "")
        self.assertEqual(filename.read_text(), "int substituted_business;\n")
        # The verifier's ordinary Git reads ignore replacement refs even before rejection.
        self.assertEqual(SDK.git(self.source, "show", "HEAD:" + filename.name).encode(),
                         original_bytes.rstrip(b"\n"))
        with self.assertRaisesRegex(ValueError, "replace"):
            SDK.verify_complete_repository(self.source)

    def test_rejects_grafts_even_when_fsck_passes(self):
        head = self.git(self.source, "rev-parse", "HEAD")
        (self.source / ".git/info/grafts").write_text(head + "\n")
        self.git(self.source, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "grafts"):
            SDK.verify_complete_repository(self.source)

    def test_rejects_absorbed_metadata_outside_own_root_modules(self):
        parent = self.root / "parent"
        parent.mkdir()
        self.git(parent, "init", "-q", "-b", "master")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(self.source), "module")
        module = parent / "module"
        SDK.verify_complete_repository(module, source_root=parent)
        directory = Path(self.git(module, "rev-parse", "--absolute-git-dir"))
        outside = self.root / "outside-module.git"
        directory.rename(outside)
        (module / ".git").write_text("gitdir: " + str(outside) + "\n")
        self.git(self.root, "config", "--file", str(outside / "config"),
                 "core.worktree", str(module))
        self.git(module, "fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "Git 元数据"):
            SDK.verify_complete_repository(module, source_root=parent)




import shlex
import sys
import zlib


class SourceIntegrityTest(unittest.TestCase):
    """真实磁盘字节、文件模式与元数据不能由 Git 的干净状态替代。"""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        self._git("init", "-q", "-b", "master")
        self._git("config", "user.name", "来源完整性 fixture")
        self._git("config", "user.email", "source@example.invalid")
        self.good = b"int good;\n"
        self.evil = b"int evil;\n"
        self.filename = self.source / "source.c"
        self.filename.write_bytes(self.good)
        self.filename.chmod(0o644)
        self._commit()
        self._verify()

    def _run_git(self, *args, check=True):
        return subprocess.run(["git", "-C", str(self.source), *args], check=check,
                              capture_output=True, env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"})

    def _git(self, *args):
        return self._run_git(*args).stdout.decode().strip()

    def _commit(self):
        self._git("add", ".")
        self._git("commit", "-qm", "真实来源 fixture")
        self.head = self._git("rev-parse", "HEAD")

    def _verify(self):
        SDK.verify_complete_repository(self.source)

    def _assert_rejected(self, *, metadata=False):
        if metadata:
            with self.assertRaisesRegex(ValueError, "Git 元数据"):
                self._verify()
        else:
            with self.assertRaises((ValueError, RuntimeError, subprocess.SubprocessError)):
                self._verify()

    def _assert_hidden_byte_divergence(self):
        self.assertEqual(self._git("rev-parse", "HEAD"), self.head)
        self.assertEqual(self._git("status", "--porcelain", "--untracked-files=normal",
                                   "--ignore-submodules=none"), "")
        self.assertEqual(self._run_git("cat-file", "blob", "HEAD:source.c").stdout, self.good)
        self.assertEqual(self.filename.read_bytes(), self.evil)
        self.assertEqual(self._run_git("diff", "--exit-code", "HEAD").returncode, 0)
        self._git("fsck", "--connectivity-only", "--no-dangling")

    def test_rejects_clean_filter_hiding_raw_source_change(self):
        command = shlex.join([sys.executable, "-c",
                              'import sys; sys.stdin.buffer.read(); sys.stdout.buffer.write(b"int good;\\n")'])
        self._git("config", "filter.fixture.clean", command)
        self._git("config", "filter.fixture.smudge", "cat")
        (self.source / ".git/info/attributes").write_text("*.c filter=fixture\n")
        self.filename.write_bytes(self.evil)
        self._assert_hidden_byte_divergence()
        self.assertEqual(self._git("hash-object", "--path=source.c", "source.c"),
                         self._git("rev-parse", "HEAD:source.c"))
        self.assertNotEqual(self._git("hash-object", "--no-filters", "source.c"),
                            self._git("rev-parse", "HEAD:source.c"))
        self._assert_rejected()

    def test_rejects_skip_worktree_hiding_raw_source_change(self):
        self._git("update-index", "--skip-worktree", "source.c")
        self.filename.write_bytes(self.evil)
        self._assert_hidden_byte_divergence()
        self.assertEqual(self._git("ls-files", "-v", "--", "source.c"), "S source.c")
        self._assert_rejected()

    def test_rejects_assume_unchanged_hiding_raw_source_change(self):
        self._git("update-index", "--assume-unchanged", "source.c")
        self.filename.write_bytes(self.evil)
        self._assert_hidden_byte_divergence()
        self.assertEqual(self._git("ls-files", "-v", "--", "source.c"), "h source.c")
        self._assert_rejected()

    def test_rejects_executable_change_hidden_by_core_filemode(self):
        self.assertTrue(self._git("ls-tree", "HEAD", "--", "source.c").startswith("100644 "))
        self._git("config", "core.filemode", "false")
        self.filename.chmod(0o755)
        self.assertEqual(self._git("status", "--porcelain"), "")
        self.assertEqual(self.filename.read_bytes(), self.good)
        self.assertEqual(self._run_git("cat-file", "blob", "HEAD:source.c").stdout, self.good)
        self.assertEqual(self._git("rev-parse", "HEAD"), self.head)
        self.assertNotEqual(self.filename.stat().st_mode & 0o111, 0)
        self._assert_rejected()

    def _tracked_symlink(self):
        link = self.source / "link.c"
        link.symlink_to("source.c")
        self._commit()
        self.assertTrue(self._git("ls-tree", "HEAD", "--", "link.c").startswith("120000 "))
        self.assertEqual(self._run_git("cat-file", "blob", "HEAD:link.c").stdout, b"source.c")
        self._verify()
        self._git("update-index", "--skip-worktree", "link.c")
        return link

    def test_rejects_tracked_symlink_replaced_with_regular_file(self):
        link = self._tracked_symlink()
        link.unlink()
        link.write_bytes(b"source.c")
        self.assertFalse(link.is_symlink())
        self.assertEqual(link.read_bytes(), self._run_git("cat-file", "blob", "HEAD:link.c").stdout)
        self.assertEqual(self._git("status", "--porcelain"), "")
        self._assert_rejected()

    def test_rejects_tracked_symlink_target_change(self):
        link = self._tracked_symlink()
        (self.root / "other.c").write_bytes(self.evil)
        link.unlink()
        link.symlink_to("../other.c")
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), "../other.c")
        self.assertEqual(link.read_bytes(), self.evil)
        self.assertEqual(self._run_git("cat-file", "blob", "HEAD:link.c").stdout, b"source.c")
        self.assertEqual(self._git("status", "--porcelain"), "")
        self._assert_rejected()

    def test_rejects_tracked_parent_directory_symlinked_outside_source(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "source.c").write_bytes(self.good)
        self._commit()
        self._verify()
        self._git("update-index", "--skip-worktree", "nested/source.c")
        outside = self.root / "outside-source-directory"
        nested.rename(outside)
        nested.symlink_to(outside, target_is_directory=True)
        (self.source / ".git/info/exclude").write_text("/nested\n")
        self.assertTrue(nested.is_symlink())
        self.assertEqual((nested / "source.c").read_bytes(), self.good)
        self.assertEqual(self._run_git("cat-file", "blob", "HEAD:nested/source.c").stdout,
                         self.good)
        self.assertEqual(self._git("status", "--porcelain"), "")
        self.assertEqual(self._git("rev-parse", "HEAD"), self.head)
        self._assert_rejected()

    def _external_metadata_symlink(self, relative):
        metadata = self.source / ".git" / relative
        outside = self.root / ("outside-" + relative)
        is_directory = metadata.is_dir()
        original_bytes = None if is_directory else metadata.read_bytes()
        metadata.rename(outside)
        metadata.symlink_to(outside, target_is_directory=is_directory)
        self.assertTrue(metadata.is_symlink())
        self.assertEqual(metadata.resolve(), outside.resolve())
        if original_bytes is not None:
            self.assertEqual(outside.read_bytes(), original_bytes)
        # Git itself may fail for external HEAD/refs; the guard must reject the
        # metadata link explicitly before depending on such version-specific errors.
        self._assert_rejected(metadata=True)

    def test_rejects_external_index_metadata_symlink(self):
        self._external_metadata_symlink("index")

    def test_rejects_external_head_metadata_symlink(self):
        self._external_metadata_symlink("HEAD")

    def test_rejects_external_config_metadata_symlink(self):
        self._external_metadata_symlink("config")

    def test_rejects_external_refs_metadata_symlink(self):
        self._external_metadata_symlink("refs")

    def test_rejects_corrupt_head_blob_when_connectivity_fsck_passes(self):
        blob = self._git("rev-parse", "HEAD:source.c")
        path = self.source / ".git/objects" / blob[:2] / blob[2:]
        self.assertTrue(path.is_file())
        self.assertEqual(self._run_git("cat-file", "blob", blob).stdout, self.good)
        self._git("fsck", "--full", "--no-dangling")
        path.chmod(0o600)
        path.write_bytes(zlib.compress(b"blob " + str(len(self.evil)).encode() + b"\0" + self.evil))
        self.assertEqual(self.filename.read_bytes(), self.good)
        self.assertEqual(self._git("rev-parse", "HEAD"), self.head)
        self._git("fsck", "--connectivity-only", "--no-dangling")
        full = self._run_git("fsck", "--full", "--no-dangling", check=False)
        self.assertNotEqual(full.returncode, 0)
        self.assertIn(b"mismatch", full.stdout + full.stderr)
        self._assert_rejected()




class SourceObjectIntegrityTest(unittest.TestCase):
    """Git 返回的储存 OID 不能代替对象原始类型、长度和内容的独立校验。"""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name).resolve() / "source"
        self.source.mkdir()
        self._git("init", "-q", "-b", "master")
        self._git("config", "user.name", "对象原始字节 fixture")
        self._git("config", "user.email", "object@example.invalid")
        self.good = b"int good;\n"
        self.evil = b"int evil;\n"
        (self.source / "source.c").write_bytes(self.good)
        self._git("add", ".")
        self._git("commit", "-qm", "真实对象 fixture")
        self.head = self._git("rev-parse", "HEAD")
        self._verify()

    def _run_git(self, *args, check=True, input_bytes=None):
        return subprocess.run(["git", "-C", str(self.source), *args], check=check,
                              capture_output=True, input=input_bytes,
                              env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"})

    def _git(self, *args):
        return self._run_git(*args).stdout.decode().strip()

    def _verify(self):
        SDK.verify_complete_repository(self.source)

    def _corrupt_blob_preserving_storage_oid(self, blob):
        path = self.source / ".git/objects" / blob[:2] / blob[2:]
        self.assertTrue(path.is_file())
        path.chmod(0o600)
        path.write_bytes(zlib.compress(b"blob " + str(len(self.evil)).encode() + b"\0" + self.evil))
        self.assertEqual(self._git("rev-parse", "HEAD"), self.head)
        self.assertEqual((self.source / "source.c").read_bytes(), self.good)
        self.assertEqual(self._run_git("cat-file", "blob", blob).stdout, self.evil)
        batch = self._run_git("cat-file", "--batch", input_bytes=(blob + "\n").encode()).stdout
        self.assertEqual(batch, blob.encode() + b" blob " + str(len(self.evil)).encode()
                         + b"\n" + self.evil + b"\n")
        self.assertNotEqual(self._run_git("hash-object", "--stdin", input_bytes=self.evil)
                            .stdout.decode().strip(), blob)
        self._git("fsck", "--connectivity-only", "--no-dangling")
        with self.assertRaisesRegex(ValueError, "对象原始字节与 OID 不符"):
            self._verify()

    def test_rejects_head_blob_even_when_cat_file_reports_original_storage_oid(self):
        blob = self._git("rev-parse", "HEAD:source.c")
        self.assertEqual(self._run_git("cat-file", "blob", blob).stdout, self.good)
        self._corrupt_blob_preserving_storage_oid(blob)

    def test_rejects_corrupt_dangling_blob_when_connectivity_fsck_passes(self):
        payload = b"unreferenced original object;\n"
        blob = self._run_git("hash-object", "-w", "--stdin", input_bytes=payload).stdout.decode().strip()
        reachable = self._git("rev-list", "--all", "--objects").splitlines()
        self.assertFalse(any(line.split()[0] == blob for line in reachable))
        self.assertEqual(self._run_git("cat-file", "blob", blob).stdout, payload)
        self._verify()
        self._corrupt_blob_preserving_storage_oid(blob)




import contextlib
import io
from unittest.mock import patch


class SDKMainRecursiveSourceTest(unittest.TestCase):
    """实际 SDK CLI 必须核对锁定递归树，同时允许唯一 lwIP 覆盖。"""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.sdk = self.root / "sdk"
        lwip_upstream = self.root / "lwip-upstream"
        leaf_upstream = self.root / "leaf-upstream"
        framework_upstream = self.root / "framework-upstream"
        for repository in (self.sdk, lwip_upstream, leaf_upstream, framework_upstream):
            repository.mkdir()
            self._git(repository, "init", "-q", "-b", "master")
            self._git(repository, "config", "user.name", "递归 SDK fixture")
            self._git(repository, "config", "user.email", "sdk-graph@example.invalid")
        self.good = b"int dependency_good;\n"
        self.evil = b"int dependency_evil;\n"
        (lwip_upstream / "tcp.c").write_bytes(b"int original_lwip;\n")
        self.lwip_original = self._commit(lwip_upstream)
        (leaf_upstream / "source.c").write_bytes(self.good)
        self.leaf_original = self._commit(leaf_upstream)
        self._git(framework_upstream, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                  str(leaf_upstream), "leaf")
        self.framework_original = self._commit(framework_upstream)
        for upstream, relative in ((lwip_upstream, "components/lwip/lwip"),
                                   (framework_upstream, "framework")):
            self._git(self.sdk, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                      str(upstream), relative)
        (self.sdk / "sdk.c").write_bytes(b"int sdk_good;\n")
        self.sdk_original = self._commit(self.sdk)
        self._git(self.sdk, "-c", "protocol.file.allow=always", "submodule", "update", "--init",
                  "--recursive", "--checkout")
        self.lwip = self.sdk / "components/lwip/lwip"
        self.framework = self.sdk / "framework"
        self.leaf = self.framework / "leaf"
        (lwip_upstream / "tcp.c").write_bytes(b"int corrected_lwip;\n")
        self.lwip_fixed = self._commit(lwip_upstream)
        self._git(self.lwip, "fetch", "-q", "origin")
        self._git(self.lwip, "checkout", "-q", "--detach", self.lwip_fixed)
        capacity_root = self.root / "capacity-contract"
        capacity_root.mkdir()
        self.capacity = CapacityFixture(capacity_root, sdk=self.sdk)
        self.sdk_original = self.capacity.recipe["idf"]["revision"]
        self.raw_status = self.capacity.raw_status
        self.capacity.apply()
        self.lock = self.capacity.recipe
        self.assertTrue((self.sdk / ".git").is_dir())
        self.assertTrue((self.leaf / ".git").is_file())
        leaf_metadata = Path(self._git(self.leaf, "rev-parse", "--absolute-git-dir")).resolve()
        self.assertTrue(leaf_metadata.is_relative_to(self.sdk / ".git/modules"))

    def _run_git(self, repository, *args):
        return subprocess.run(["git", "-C", str(repository), *args], check=True,
                              capture_output=True, env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"})

    def _git(self, repository, *args):
        return self._run_git(repository, *args).stdout.decode().strip()

    def _commit(self, repository):
        self._git(repository, "add", ".")
        self._git(repository, "commit", "-qm", "完整 SDK fixture")
        return self._git(repository, "rev-parse", "HEAD")

    def _assert_main(self, accepted, error=None):
        stdout = io.StringIO()
        stderr = io.StringIO()
        injection = patch.object(SDK, "ROOT", self.capacity.product)
        argv = ["source-fixture", "--path", str(self.sdk)]
        with injection, patch.object(sys, "argv", argv), contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            try:
                result = SDK.main()
            except SystemExit as failure:
                result = failure.code
        self.assertEqual(result == 0, accepted, stdout.getvalue() + stderr.getvalue())
        if error:
            self.assertIn(error, stderr.getvalue())

    def _hide_child_status(self):
        self._git(self.sdk, "config", "submodule.framework.ignore", "all")
        self._git(self.framework, "config", "submodule.leaf.ignore", "all")
        self._git(self.sdk, "update-index", "--skip-worktree", "framework")
        self._git(self.leaf, "update-index", "--skip-worktree", "source.c")

    def test_main_accepts_complete_absorbed_graph_with_only_lwip_override(self):
        self.assertEqual(self._git(self.sdk, "rev-parse", "HEAD"), self.sdk_original)
        self.assertEqual(self._git(self.sdk, "rev-parse", "HEAD:components/lwip/lwip"), self.lwip_original)
        self.assertEqual(self._git(self.lwip, "rev-parse", "HEAD"), self.lwip_fixed)
        self.assertEqual(self.raw_status, "M components/lwip/lwip")
        self.assertEqual(len(self._git(self.sdk, "status", "--porcelain", "--ignore-submodules=none").splitlines()), 11)
        self._assert_main(True)
        self._hide_child_status()
        self._assert_main(True)

    def test_main_rejects_hidden_child_raw_bytes_with_unchanged_index(self):
        self._hide_child_status()
        (self.leaf / "source.c").write_bytes(self.evil)
        self.assertEqual(self._git(self.leaf, "status", "--porcelain"), "")
        self.assertEqual(self._git(self.framework, "status", "--porcelain"), "")
        self.assertEqual(self._git(self.sdk, "rev-parse", "HEAD"), self.sdk_original)
        self.assertEqual(self._run_git(self.leaf, "cat-file", "blob", "HEAD:source.c").stdout, self.good)
        self.assertEqual((self.leaf / "source.c").read_bytes(), self.evil)
        self.assertEqual(self._git(self.sdk, "ls-files", "--stage", "--", "framework"),
                         "160000 " + self.framework_original + " 0\tframework")
        self.assertEqual(self._git(self.leaf, "ls-files", "--stage", "--", "source.c").split()[1],
                         self._git(self.leaf, "rev-parse", "HEAD:source.c"))
        self._assert_main(False, "原始字节")

    def test_main_rejects_hidden_staged_gitlink_removal_with_all_source_bytes_unchanged(self):
        self._hide_child_status()
        self._git(self.sdk, "update-index", "--no-skip-worktree", "framework")
        self._git(self.sdk, "update-index", "--force-remove", "framework")
        (self.sdk / ".git/info/exclude").write_text("/framework\n")
        self.assertEqual(self._git(self.sdk, "diff", "--cached", "--name-only"), "")
        self.assertNotIn(" framework", self._git(self.sdk, "submodule", "status", "--recursive"))
        self.assertEqual(self._git(self.sdk, "ls-files", "--stage", "--", "framework"), "")
        self.assertTrue(self._git(self.sdk, "ls-tree", self.sdk_original, "--", "framework")
                        .startswith("160000 "))
        self.assertEqual(self._run_git(self.leaf, "cat-file", "blob", "HEAD:source.c").stdout, self.good)
        self.assertEqual((self.leaf / "source.c").read_bytes(), self.good)
        self.assertEqual(self._git(self.leaf, "rev-parse", "HEAD"), self.leaf_original)
        self._assert_main(False, "索引")



class SparseConfigurationTest(unittest.TestCase):
    """Git 最终生效的布尔配置与实际完整源码共同决定是否接受来源。"""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.global_config = self.root / "global.gitconfig"
        self.global_config.write_text("")
        self.included_config = self.root / "included.gitconfig"
        self.environment = patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": str(self.global_config), "GIT_CONFIG_NOSYSTEM": "1",
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.source = self.root / "source"
        self.source.mkdir()
        self._git("init", "-q", "-b", "master")
        self._git("config", "user.name", "稀疏配置 fixture")
        self._git("config", "user.email", "sparse@example.invalid")
        (self.source / "source.c").write_text("int source;\n")
        (self.source / "omitted").mkdir()
        (self.source / "omitted/source.c").write_text("int complete;\n")
        self._git("add", ".")
        self._git("commit", "-qm", "完整来源 fixture")

    def _git(self, *args, check=True):
        return subprocess.run(["git", "-C", str(self.source), *args], check=check,
                              capture_output=True, text=True)

    def _set(self, key, value, *scope):
        self._git("config", *scope, key, value)

    def _verify(self):
        SDK.verify_complete_repository(self.source)

    def _effective_sparse(self):
        result = self._git("config", "--bool", "--get", "core.sparseCheckout", check=False)
        return result.returncode, result.stdout.strip()

    def _include(self, value):
        self.included_config.write_text("[core]\n\tsparseCheckout = " + value + "\n")
        self._set("include.path", str(self.included_config), "--local")

    def test_accepts_global_true_overridden_by_local_false(self):
        self._set("core.sparseCheckout", "true", "--global")
        self._set("core.sparseCheckout", "false", "--local")
        self.assertEqual(self._effective_sparse(), (0, "false"))
        self._verify()

    def test_rejects_global_false_overridden_by_local_true(self):
        self._set("core.sparseCheckout", "false", "--global")
        self._set("core.sparseCheckout", "true", "--local")
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()

    def test_accepts_worktree_false_overriding_local_true(self):
        self._set("core.sparseCheckout", "true", "--local")
        self._set("extensions.worktreeConfig", "true", "--local")
        self._set("core.sparseCheckout", "false", "--worktree")
        self.assertEqual(self._effective_sparse(), (0, "false"))
        self._verify()

    def test_rejects_worktree_true_overriding_local_false(self):
        self._set("core.sparseCheckout", "false", "--local")
        self._set("extensions.worktreeConfig", "true", "--local")
        self._set("core.sparseCheckout", "true", "--worktree")
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()

    def test_accepts_included_local_false_after_local_true(self):
        self._set("core.sparseCheckout", "true", "--local")
        self._include("false")
        self.assertEqual(self._effective_sparse(), (0, "false"))
        self._verify()

    def test_rejects_included_local_true_after_local_false(self):
        self._set("core.sparseCheckout", "false", "--local")
        self._include("true")
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()

    def test_accepts_inactive_cone_mode_with_complete_source(self):
        self._set("core.sparseCheckout", "false", "--local")
        self._set("core.sparseCheckoutCone", "true", "--local")
        self.assertEqual(self._effective_sparse(), (0, "false"))
        self._verify()

    def test_rejects_invalid_effective_boolean(self):
        self._set("core.sparseCheckout", "invalid-boolean", "--local")
        self.assertNotEqual(self._effective_sparse()[0], 0)
        with self.assertRaises((ValueError, RuntimeError, subprocess.CalledProcessError)) as raised:
            self._verify()
        detail = getattr(raised.exception, "stderr", "") or str(raised.exception)
        self.assertIn("sparse", detail.lower())

    def test_rejects_nonzero_numeric_effective_true(self):
        self._set("core.sparseCheckout", "2", "--local")
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()

    def test_rejects_implicit_boolean_effective_true(self):
        config = self.source / ".git/config"
        with config.open("a") as output:
            output.write("[core]\n\tsparseCheckout\n")
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()


    def _effective_promisor(self, remote="origin"):
        result = self._git("config", "--bool", "--get", "remote." + remote + ".promisor", check=False)
        return result.returncode, result.stdout.strip()

    def test_accepts_explicit_promisor_false(self):
        self._set("remote.origin.promisor", "false", "--local")
        self.assertEqual(self._effective_promisor(), (0, "false"))
        self._verify()

    def test_accepts_local_promisor_false_overriding_global_true(self):
        self._set("remote.origin.promisor", "true", "--global")
        self._set("remote.origin.promisor", "false", "--local")
        self.assertEqual(self._effective_promisor(), (0, "false"))
        self._verify()

    def test_accepts_worktree_promisor_false_overriding_local_true(self):
        self._set("remote.origin.promisor", "true", "--local")
        self._set("extensions.worktreeConfig", "true", "--local")
        self._set("remote.origin.promisor", "false", "--worktree")
        self.assertEqual(self._effective_promisor(), (0, "false"))
        self._verify()

    def test_accepts_included_promisor_false_overriding_local_true(self):
        self._set("remote.origin.promisor", "true", "--local")
        self.included_config.write_text('[remote "origin"]\n\tpromisor = false\n')
        self._set("include.path", str(self.included_config), "--local")
        self.assertEqual(self._effective_promisor(), (0, "false"))
        self._verify()

    def test_accepts_false_mixed_case_remote_subsection(self):
        self._set("remote.Mirror.Name.promisor", "false", "--local")
        self.assertEqual(self._effective_promisor("Mirror.Name"), (0, "false"))
        self._verify()

    def test_rejects_true_mixed_case_remote_despite_lowercase_false(self):
        self._set("remote.Mirror.Name.promisor", "true", "--local")
        self._set("remote.mirror.name.promisor", "false", "--local")
        self.assertEqual(self._effective_promisor("Mirror.Name"), (0, "true"))
        self.assertEqual(self._effective_promisor("mirror.name"), (0, "false"))
        with self.assertRaisesRegex(ValueError, "partial"):
            self._verify()

    def test_rejects_local_promisor_true_overriding_global_false(self):
        self._set("remote.origin.promisor", "false", "--global")
        self._set("remote.origin.promisor", "true", "--local")
        self.assertEqual(self._effective_promisor(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "partial"):
            self._verify()

    def test_rejects_nonzero_numeric_promisor_true(self):
        self._set("remote.origin.promisor", "2", "--local")
        self.assertEqual(self._effective_promisor(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "partial"):
            self._verify()

    def test_rejects_implicit_promisor_true(self):
        with (self.source / ".git/config").open("a") as output:
            output.write('[remote "origin"]\n\tpromisor\n')
        self.assertEqual(self._effective_promisor(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "partial"):
            self._verify()

    def test_rejects_invalid_effective_promisor_boolean(self):
        self._set("remote.origin.promisor", "invalid-boolean", "--local")
        self.assertNotEqual(self._effective_promisor()[0], 0)
        with self.assertRaises((ValueError, RuntimeError, subprocess.CalledProcessError)) as raised:
            self._verify()
        detail = getattr(raised.exception, "stderr", "") or str(raised.exception)
        self.assertIn("promisor", detail.lower())

    def test_rejects_partial_filter_marker_with_promisor_false(self):
        self._set("remote.origin.promisor", "false", "--local")
        self._set("remote.origin.partialCloneFilter", "blob:none", "--local")
        with self.assertRaisesRegex(ValueError, "partial"):
            self._verify()

    def _assert_ignored_rejected(self):
        self.assertEqual(self._git("ls-files", "--others", "--exclude-standard").stdout, "")
        self.assertTrue(self._git("ls-files", "--others", "-z").stdout)
        with self.assertRaisesRegex(ValueError, "ignored|untracked"):
            self._verify()

    def test_rejects_source_hidden_by_git_info_exclude(self):
        with (self.source / ".git/info/exclude").open("a") as output:
            output.write("injected.c\n")
        (self.source / "injected.c").write_text("int injected;\n")
        self._assert_ignored_rejected()

    def test_rejects_source_hidden_by_tracked_gitignore(self):
        (self.source / ".gitignore").write_text("injected.c\n")
        self._git("add", ".gitignore")
        self._git("commit", "-qm", "显式源码忽略规则 fixture")
        (self.source / "injected.c").write_text("int injected;\n")
        self._assert_ignored_rejected()

    def test_rejects_source_hidden_by_global_ignore(self):
        ignore_file = self.root / "global.ignore"
        ignore_file.write_text("injected.c\n")
        self._set("core.excludesFile", str(ignore_file), "--global")
        (self.source / "injected.c").write_text("int injected;\n")
        self._assert_ignored_rejected()

    def test_rejects_ignored_source_inside_cache_named_directory(self):
        with (self.source / ".git/info/exclude").open("a") as output:
            output.write("__pycache__/\n")
        cache = self.source / "__pycache__"
        cache.mkdir()
        (cache / "injected.py").write_text("runtime_value = 37\n")
        self._assert_ignored_rejected()

    def test_rejects_ignored_source_symlink(self):
        outside = self.root / "outside.c"
        outside.write_text("int injected;\n")
        with (self.source / ".git/info/exclude").open("a") as output:
            output.write("injected.c\n")
        (self.source / "injected.c").symlink_to(outside)
        self._assert_ignored_rejected()


    def _omit_tracked_source(self):
        self._git("sparse-checkout", "set", "--no-cone", "/source.c")
        self.assertTrue((self.source / "source.c").is_file())
        self.assertFalse((self.source / "omitted/source.c").exists())

    def test_rejects_actual_sparse_checkout(self):
        self._omit_tracked_source()
        self.assertEqual(self._effective_sparse(), (0, "true"))
        with self.assertRaisesRegex(ValueError, "sparse"):
            self._verify()

    def test_rejects_missing_raw_source_after_sparse_flag_disabled(self):
        self._omit_tracked_source()
        self._set("core.sparseCheckout", "false", "--worktree")
        self.assertEqual(self._effective_sparse(), (0, "false"))
        with self.assertRaises((ValueError, FileNotFoundError)):
            self._verify()



class CapacityDerivationTest(unittest.TestCase):
    """真实 Git 正负图执行同一生产 CLI，stamp不能替代原件/索引/对象证明。"""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="base-capacity-source-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.capacity = CapacityFixture(self.root)

    def test_prepare_rejects_crlf_on_each_managed_repository_before_first_write(self):
        for name in ("idf", "tlsf"):
            with self.subTest(repository=name), tempfile.TemporaryDirectory() as directory:
                capacity = CapacityFixture(Path(directory))
                declaration = next(item for item in capacity.recipe["managed_patches"] if item["repository"] == name)
                repository = capacity.sdk if name == "idf" else capacity.tlsf
                relative = declaration["files"][-1]["path"]
                metadata = Path(capacity.git(repository, "rev-parse", "--absolute-git-dir"))
                (metadata / "info/attributes").write_text(relative + " text eol=crlf\n")
                originals = {(item["repository"], file["path"]):
                             (capacity.sdk if item["repository"] == "idf" else capacity.tlsf).joinpath(file["path"]).read_bytes()
                             for item in capacity.recipe["managed_patches"] for file in item["files"]}
                result = capacity.prepare()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("内置 Git", result.stderr)
                for (repo_name, path), content in originals.items():
                    actual = (capacity.sdk if repo_name == "idf" else capacity.tlsf) / path
                    self.assertEqual(actual.read_bytes(), content, "任何受管路径首写前必须已拒绝")
                self.assertFalse(capacity.stamp.exists())


    def test_check_rejects_wrong_idf_and_lwip_fetch_origins(self):
        self.apply()
        for name, repo in (("idf", self.capacity.sdk), ("lwip", self.capacity.lwip)):
            with self.subTest(repository=name):
                self.capacity.git(repo, "remote", "set-url", "origin", "https://example.invalid/untrusted.git")
                self.reject("origin")
                self.capacity.git(repo, "remote", "set-url", "origin", self.capacity.recipe[name]["repository"])

    def test_check_rejects_fetch_url_rewrite_and_multiple_raw_origins(self):
        self.apply()
        repo = self.capacity.lwip
        raw = self.capacity.recipe["lwip"]["repository"]
        self.capacity.git(repo, "config", "url.https://example.invalid/untrusted.git.insteadOf", raw)
        self.reject("origin")
        self.capacity.git(repo, "config", "--remove-section", "url.https://example.invalid/untrusted.git")
        self.capacity.git(repo, "config", "--add", "remote.origin.url", raw)
        self.reject("origin")

    def test_prepare_rejects_encoding_before_first_write(self):
        with tempfile.TemporaryDirectory() as directory:
            capacity = CapacityFixture(Path(directory))
            metadata = Path(capacity.git(capacity.tlsf, "rev-parse", "--absolute-git-dir"))
            relative = capacity.recipe["managed_patches"][1]["files"][-1]["path"]
            (metadata / "info/attributes").write_text(relative + " working-tree-encoding=UTF-16LE\n")
            before = {(name, path): (capacity.sdk if name == "idf" else capacity.tlsf).joinpath(path).read_bytes()
                      for name, path in capacity.original}
            result = capacity.prepare()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("内置 Git", result.stderr)
            for (name, path), raw in before.items():
                self.assertEqual((capacity.sdk if name == "idf" else capacity.tlsf).joinpath(path).read_bytes(), raw)
            self.assertFalse(capacity.stamp.exists())

    def test_prepare_preserves_safe_lf_and_utf8_with_host_crlf_defaults(self):
        for attribute in ("text eol=lf", "-text eol=crlf", "working-tree-encoding=UTF-8", "ident"):
            with self.subTest(attribute=attribute), tempfile.TemporaryDirectory() as directory:
                capacity = CapacityFixture(Path(directory))
                capacity.git(capacity.sdk, "config", "core.autocrlf", "true")
                capacity.git(capacity.sdk, "config", "core.eol", "crlf")
                relative = capacity.recipe["managed_patches"][0]["files"][-1]["path"]
                (capacity.sdk / ".git/info/attributes").write_text(relative + " " + attribute + "\n")
                capacity.apply()
                self.assertEqual(capacity.check().returncode, 0)
                self.assertEqual(capacity.stamp.read_bytes(), (capacity.product / "sdk-lock.json").read_bytes())

    def test_check_accepts_equivalent_ssh_fetch_identity(self):
        self.apply()
        for name, repo in (("idf", self.capacity.sdk), ("lwip", self.capacity.lwip)):
            value = self.capacity.recipe[name]["repository"].removeprefix("https://github.com/")
            self.capacity.git(repo, "remote", "set-url", "origin", "git@github.com:" + value)
        self.assertEqual(self.capacity.check().returncode, 0)

    def test_source_check_never_executes_fsmonitor_from_local_or_command_scope(self):
        self.apply()
        marker = self.root / "monitor-ran"
        script = self.root / "monitor.sh"
        script.write_text("#!/bin/sh\nprintf ran > " + shlex.quote(str(marker)) + "\nprintf 'token\\0'\n")
        script.chmod(0o755)
        for command_scope in (False, True):
            with self.subTest(command_scope=command_scope):
                marker.unlink(missing_ok=True)
                environment = self.capacity.environment.copy()
                if command_scope:
                    self.capacity.git(self.capacity.sdk, "config", "--unset", "core.fsmonitor")
                    environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="core.fsmonitor", GIT_CONFIG_VALUE_0=str(script))
                else:
                    self.capacity.git(self.capacity.sdk, "config", "core.fsmonitor", str(script))
                result = subprocess.run([sys.executable, str(self.capacity.product / "tools/check_sdk.py"),
                                         "--path", str(self.capacity.sdk)], capture_output=True, text=True,
                                        env=environment, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(marker.exists(), "来源检查不得执行 fsmonitor")

    def test_prepare_allows_unselected_native_lfs_drivers(self):
        for scope in ("system", "global", "command"):
            with self.subTest(scope=scope), tempfile.TemporaryDirectory() as directory:
                capacity = CapacityFixture(Path(directory))
                configuration = Path(directory) / "registered-lfs"
                pairs = (("clean", "git-lfs clean -- %f"), ("smudge", "git-lfs smudge -- %f"),
                         ("process", "git-lfs filter-process"))
                for kind, command in pairs:
                    capacity.git(capacity.root, "config", "--file", str(configuration), "filter.lfs." + kind, command)
                capacity.environment.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null",
                                            GIT_CONFIG_NOSYSTEM="0", GIT_CONFIG_COUNT="0")
                if scope == "command":
                    capacity.environment.update(GIT_CONFIG_COUNT="3")
                    for index, (kind, command) in enumerate(pairs):
                        capacity.environment["GIT_CONFIG_KEY_" + str(index)] = "filter.lfs." + kind
                        capacity.environment["GIT_CONFIG_VALUE_" + str(index)] = command
                else:
                    capacity.environment["GIT_CONFIG_" + scope.upper()] = str(configuration)
                result = capacity.prepare()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(capacity.check().returncode, 0)
                self.assertEqual(capacity.stamp.read_bytes(), (capacity.product / "sdk-lock.json").read_bytes())

    def test_prepare_allows_filter_bound_only_to_unmodified_file(self):
        marker = self.root / "unused-filter-ran"
        command = shlex.join([sys.executable, "-c", 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("ran")', str(marker)])
        (self.capacity.sdk / ".git/info/attributes").write_text(".gitmodules filter=source-probe\n")
        self.capacity.git(self.capacity.sdk, "config", "filter.source-probe.clean", command)
        result = self.capacity.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.capacity.check().returncode, 0)
        self.assertFalse(marker.exists())

    def test_prepare_distinguishes_boolean_attributes_from_same_named_drivers(self):
        for setting in ("", " !filter", " filter", " -filter"):
            with self.subTest(setting=setting), tempfile.TemporaryDirectory() as directory:
                capacity = CapacityFixture(Path(directory))
                marker = Path(directory) / "sentinel-driver-ran"
                command = shlex.join([sys.executable, "-c",
                    'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("ran")', str(marker)])
                for driver in ("set", "unset", "unspecified"):
                    for kind in ("clean", "smudge", "process"):
                        capacity.git(capacity.sdk, "config", "filter." + driver + "." + kind, command)
                relative = capacity.recipe["managed_patches"][0]["files"][-1]["path"]
                (capacity.sdk / ".git/info/attributes").write_text(relative + setting + "\n" if setting else "")
                def objects():
                    return {str(path.relative_to(capacity.sdk)): path.read_bytes()
                            for path in capacity.sdk.rglob("*") if path.is_file() and "/objects/" in str(path)}
                original_objects = objects()
                result = capacity.prepare()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(objects(), original_objects, "属性判别不得写入 Git 对象")
                self.assertFalse(marker.exists(), "布尔和未设置属性不得执行同名驱动")
                self.assertEqual(capacity.check().returncode, 0)

    def test_prepare_rejects_same_named_literal_drivers_without_execution(self):
        for driver in ("set", "unset", "unspecified", "unspecified=other"):
            with self.subTest(driver=driver), tempfile.TemporaryDirectory() as directory:
                capacity = CapacityFixture(Path(directory))
                marker = Path(directory) / "literal-driver-ran"
                command = shlex.join([sys.executable, "-c",
                    'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("ran")', str(marker)])
                capacity.git(capacity.sdk, "config", "filter." + driver + ".clean", command)
                relative = capacity.recipe["managed_patches"][0]["files"][-1]["path"]
                (capacity.sdk / ".git/info/attributes").write_text(relative + " filter=" + driver + "\n")
                original = {(name, file): (capacity.sdk if name == "idf" else capacity.tlsf).joinpath(file).read_bytes()
                            for name, file in capacity.original}
                result = capacity.prepare()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("外部 Git filter", result.stderr)
                self.assertFalse(marker.exists())
                self.assertFalse(capacity.stamp.exists())
                for (name, file), content in original.items():
                    self.assertEqual((capacity.sdk if name == "idf" else capacity.tlsf).joinpath(file).read_bytes(), content)

    def test_prepare_rejects_later_repository_filter_before_any_write(self):
        before = {(name, relative): (self.capacity.sdk if name == "idf" else self.capacity.tlsf).joinpath(relative).read_bytes()
                  for name, relative in self.capacity.original}
        marker = self.root / "late-repository-filter-ran"
        command = shlex.join([sys.executable, "-c", 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("ran")', str(marker)])
        metadata = Path(self.capacity.git(self.capacity.tlsf, "rev-parse", "--absolute-git-dir"))
        relative = self.capacity.recipe["managed_patches"][1]["files"][-1]["path"]
        (metadata / "info/attributes").write_text(relative + " filter=source-probe\n")
        self.capacity.git(self.capacity.tlsf, "config", "filter.source-probe.clean", command)
        result = self.capacity.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("外部 Git filter", result.stderr)
        for (name, relative), content in before.items():
            self.assertEqual((self.capacity.sdk if name == "idf" else self.capacity.tlsf).joinpath(relative).read_bytes(), content)
        self.assertFalse(marker.exists())
        self.assertFalse(self.capacity.stamp.exists())

    def test_prepare_rejects_external_filters_before_mutation(self):
        before = self.tracked().read_bytes()
        marker = self.root / "apply-filter-ran"
        command = shlex.join([sys.executable, "-c", 'import sys,pathlib; data=sys.stdin.buffer.read(); pathlib.Path(sys.argv[1]).write_text("ran"); sys.stdout.buffer.write(data)', str(marker)])
        attributes = self.root / "outside-attributes"
        relative = str(self.tracked().relative_to(self.capacity.sdk))
        attributes.write_text(relative + " filter=source-probe\n")
        self.capacity.git(self.capacity.sdk, "config", "core.attributesFile", str(attributes))
        original_environment = self.capacity.environment.copy()
        for scope in ("local", "global", "system", "command"):
            for kind in ("clean", "smudge", "process"):
                with self.subTest(scope=scope, kind=kind):
                    key = "filter.source-probe." + kind
                    self.capacity.environment = original_environment.copy()
                    configuration = self.root / (scope + "-filter-config")
                    configuration.write_text("")
                    if scope == "local":
                        self.capacity.git(self.capacity.sdk, "config", key, command)
                    elif scope == "command":
                        self.capacity.environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0=key, GIT_CONFIG_VALUE_0=command)
                    else:
                        self.capacity.git(self.root, "config", "--file", str(configuration), key, command)
                        self.capacity.environment["GIT_CONFIG_" + scope.upper()] = str(configuration)
                        self.capacity.environment["GIT_CONFIG_NOSYSTEM"] = "0"
                    try:
                        result = self.capacity.prepare()
                        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("外部 Git filter", result.stderr)
                        self.assertFalse(marker.exists(), "补丁检查前不得执行 filter")
                        self.assertEqual(self.tracked().read_bytes(), before)
                        self.assertFalse(self.capacity.stamp.exists())
                    finally:
                        if scope == "local":
                            self.capacity.git(self.capacity.sdk, "config", "--unset", key)
                        self.capacity.environment = original_environment.copy()

    def test_layout_imports_reject_fifo_recipe_without_waiting(self):
        for name in ("preflight_v3_migration.py", "prepare_native_layout.py", "archive_esp32_at.py", "device_control.py"):
            shutil.copyfile(Path(__file__).with_name(name), self.capacity.product / "tools" / name)
        lock = self.capacity.product / "sdk-lock.json"
        lock.unlink(); os.mkfifo(lock)
        for entry in ("preflight_v3_migration.py", "prepare_native_layout.py"):
            with self.subTest(entry=entry):
                result = self.capacity.command(entry, "--help", timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("普通文件", result.stderr)

    def apply(self):
        self.capacity.apply()
        self.assertEqual(self.capacity.check().returncode, 0)

    def reject(self, text=None):
        result = self.capacity.check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        if text:
            self.assertIn(text, result.stderr)

    def tracked(self, repository="idf"):
        declaration = next(item for item in self.capacity.recipe["managed_patches"] if item["repository"] == repository)
        path = self.capacity.sdk if repository == "idf" else self.capacity.tlsf
        return path / declaration["files"][0]["path"]

    def metadata(self):
        result = []
        for repository in (self.capacity.sdk, self.capacity.lwip, self.capacity.tlsf, self.capacity.extra):
            metadata = Path(self.capacity.git(repository, "rev-parse", "--absolute-git-dir"))
            result.append((self.capacity.git(repository, "rev-parse", "HEAD"),
                           (metadata / "index").read_bytes(), (metadata / "config").read_bytes(),
                           self.capacity.git(repository, "for-each-ref", "--format=%(refname) %(objectname)")))
        return result

    def test_real_prepare_complete_derivation_and_idempotency_keep_original_git(self):
        original = self.metadata()
        self.reject("缺失")
        self.apply()
        self.assertEqual(self.metadata(), original)
        self.assertEqual(self.capacity.stamp.read_bytes(), (self.capacity.product / "sdk-lock.json").read_bytes())
        self.assertEqual(self.capacity.stamp.stat().st_mode & 0o777, 0o400)
        before = {(name, item["path"]): (repo / item["path"]).read_bytes()
                  for name, repo in (("idf", self.capacity.sdk), ("tlsf", self.capacity.tlsf))
                  for declaration in self.capacity.recipe["managed_patches"] if declaration["repository"] == name
                  for item in declaration["files"]}
        self.assertEqual(self.capacity.prepare().returncode, 0)
        self.assertEqual(self.metadata(), original)
        self.assertEqual(before, {(name, item["path"]): (repo / item["path"]).read_bytes()
                                 for name, repo in (("idf", self.capacity.sdk), ("tlsf", self.capacity.tlsf))
                                 for declaration in self.capacity.recipe["managed_patches"] if declaration["repository"] == name
                                 for item in declaration["files"]})

    def test_correct_stamp_on_raw_sdk_is_rejected(self):
        self.capacity.stamp.write_bytes((self.capacity.product / "sdk-lock.json").read_bytes())
        self.capacity.stamp.chmod(0o400)
        self.reject("精确派生摘要")

    def test_partial_patch_without_stamp_rejects_before_prepare_mutation(self):
        file = self.tracked()
        key = "idf", self.capacity.recipe["managed_patches"][0]["files"][0]["path"]
        file.write_bytes(self.capacity.after[key])
        original = self.metadata()
        before = file.read_bytes()
        result = self.capacity.prepare()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(file.read_bytes(), before)
        self.assertEqual(self.metadata(), original)
        self.assertFalse(self.capacity.stamp.exists())

    def test_complete_patch_missing_stamp_is_rejected(self):
        self.apply()
        self.capacity.stamp.unlink()
        self.reject("缺失")
        self.assertEqual(self.capacity.prepare().returncode, 1)

    def test_one_managed_file_restored_to_before_is_rejected(self):
        self.apply()
        for repository in ("idf", "tlsf"):
            declaration = next(item for item in self.capacity.recipe["managed_patches"] if item["repository"] == repository)
            item = declaration["files"][0]
            file = self.tracked(repository)
            file.write_bytes(self.capacity.original[repository, item["path"]])
            self.reject("精确派生摘要")
            file.write_bytes(self.capacity.after[repository, item["path"]])

    def test_stamp_cannot_self_authorize_changed_final_source(self):
        self.apply()
        self.tracked().write_text("int attacker_source;\n")
        stamp = json.loads(self.capacity.stamp.read_bytes())
        stamp["managed_patches"][0]["files"][0]["after_sha256"] = hashlib.sha256(self.tracked().read_bytes()).hexdigest()
        self.capacity.stamp.chmod(0o600)
        self.capacity.stamp.write_text(json.dumps(stamp))
        self.capacity.stamp.chmod(0o400)
        self.reject("canonical")

    def test_before_digest_is_checked_from_actual_head_blob(self):
        self.apply()
        self.capacity.recipe["managed_patches"][0]["files"][0]["before_sha256"] = "0" * 64
        self.capacity.write_recipe(stamp=True)
        self.reject("HEAD blob")

    def test_duplicate_recipe_json_field_rejected(self):
        file = self.capacity.product / "sdk-lock.json"
        file.write_bytes(file.read_bytes().replace(b'"schema_version": 2,', b'"schema_version": 2, "schema_version": 2,', 1))
        self.reject("重复字段")

    def test_duplicate_managed_path_rejected(self):
        self.capacity.recipe["managed_patches"][0]["files"][1]["path"] = self.capacity.recipe["managed_patches"][0]["files"][0]["path"]
        self.capacity.write_recipe()
        self.reject("重复")

    def test_unknown_recipe_field_rejected(self):
        self.capacity.recipe["source_override"] = "anything"
        self.capacity.write_recipe()
        self.reject("未知字段")

    def test_escaping_or_metadata_recipe_path_rejected(self):
        for path in ("../outside.c", "/outside.c", "a//b.c", "a/./b.c", "a\\b.c", ".git/config"):
            with self.subTest(path=path):
                self.capacity.recipe["managed_patches"][0]["files"][0]["path"] = path
                self.capacity.write_recipe()
                self.reject("相对路径")

    def test_tlsf_revision_must_equal_original_parent_gitlink(self):
        self.capacity.recipe["tlsf"]["revision"] = "0" * 40
        self.capacity.write_recipe()
        self.reject("原生 gitlink")

    def test_managed_executable_and_symlink_types_rejected(self):
        self.apply()
        file = self.tracked()
        file.chmod(0o755)
        self.reject("文件类型或执行位")
        file.chmod(0o644)
        outside = self.root / "same-bytes.c"
        outside.write_bytes(file.read_bytes())
        file.unlink(); file.symlink_to(outside)
        self.reject("文件类型或执行位")

    def test_managed_parent_symlink_rejected(self):
        self.apply()
        parent = self.tracked().parent
        moved = self.root / "moved-source"
        parent.rename(moved)
        parent.symlink_to(moved, target_is_directory=True)
        self.reject("父目录类型")

    def test_root_and_tlsf_managed_index_changes_rejected(self):
        self.apply()
        for repository in (self.capacity.sdk, self.capacity.tlsf):
            with self.subTest(repository=repository):
                file = self.tracked("idf" if repository == self.capacity.sdk else "tlsf")
                self.capacity.git(repository, "add", "--", file.relative_to(repository).as_posix())
                self.reject("索引")
                self.capacity.git(repository, "reset", "-q", "HEAD", "--", file.relative_to(repository).as_posix())

    def test_unknown_ignored_source_in_all_actual_roots_rejected(self):
        self.apply()
        for repository in (self.capacity.sdk, self.capacity.tlsf, self.capacity.extra):
            with self.subTest(repository=repository):
                metadata = Path(self.capacity.git(repository, "rev-parse", "--absolute-git-dir"))
                (metadata / "info/exclude").write_text("unknown.c\n")
                file = repository / "unknown.c"
                file.write_text("int hidden_source;\n")
                self.reject("ignored")
                file.unlink()

    def test_same_stamp_name_in_child_source_has_no_exemption(self):
        self.apply()
        child = self.capacity.tlsf / self.capacity.stamp.name
        child.write_bytes(self.capacity.stamp.read_bytes())
        child.chmod(0o400)
        self.reject("ignored")

    def test_stamp_directory_symlink_fifo_and_writable_mode_rejected(self):
        self.apply()
        stamp = self.capacity.stamp
        raw = stamp.read_bytes()
        stamp.chmod(0o600)
        self.reject("权限")
        stamp.unlink(); stamp.mkdir()
        self.reject("普通文件")
        stamp.rmdir()
        other = self.root / "same-stamp.json"
        other.write_bytes(raw)
        stamp.symlink_to(other)
        self.reject("普通文件")
        stamp.unlink(); os.mkfifo(stamp, 0o400)
        self.reject("普通文件")

    def test_partial_and_sparse_child_not_hidden_by_valid_derivation(self):
        self.apply()
        self.capacity.git(self.capacity.tlsf, "config", "remote.origin.promisor", "true")
        self.reject("partial")
        self.capacity.git(self.capacity.tlsf, "config", "remote.origin.promisor", "false")
        self.capacity.git(self.capacity.extra, "config", "core.sparseCheckout", "true")
        self.reject("sparse")

    def test_corrupt_dangling_object_not_hidden_by_valid_derivation(self):
        self.apply()
        repo = self.capacity.tlsf
        payload = b"unreferenced original capacity object;\n"
        result = subprocess.run(["git", "-C", str(repo), "hash-object", "-w", "--stdin"], input=payload,
                                check=True, capture_output=True, env=self.capacity.environment)
        blob = result.stdout.decode().strip()
        metadata = Path(self.capacity.git(repo, "rev-parse", "--absolute-git-dir"))
        file = metadata / "objects" / blob[:2] / blob[2:]
        self.assertTrue(file.is_file())
        file.chmod(0o600)
        evil = b"attacker dangling object;\n"
        file.write_bytes(zlib.compress(b"blob " + str(len(evil)).encode() + b"\0" + evil))
        self.capacity.git(repo, "fsck", "--connectivity-only", "--no-dangling")
        self.reject("原始字节与 OID")

    def change_during_full_proof(self, target):
        self.apply()
        executable = shutil.which("git")
        wrapper = self.root / "git-observer"
        wrapper.mkdir()
        ready, release = self.root / "proof-ready", self.root / "proof-release"
        script = wrapper / "git"
        script.write_text("#!/usr/bin/env python3\nimport os,sys,time\nfrom pathlib import Path\n"
                          f"ready=Path({str(ready)!r});release=Path({str(release)!r})\n"
                          "if '--batch-all-objects' in sys.argv and not ready.exists():\n"
                          " ready.write_text('actual Git proof paused')\n"
                          " deadline=time.monotonic()+10\n"
                          " while not release.exists():\n"
                          "  if time.monotonic()>deadline: raise SystemExit(91)\n"
                          "  time.sleep(0.01)\n"
                          f"os.execv({executable!r},[{executable!r},*sys.argv[1:]])\n")
        script.chmod(0o755)
        environment = {**self.capacity.environment, "PATH": str(wrapper) + os.pathsep + os.environ["PATH"]}
        process = subprocess.Popen([sys.executable, str(self.capacity.product / "tools/check_sdk.py"),
                                    "--path", str(self.capacity.sdk)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=environment)
        try:
            import time
            deadline = time.monotonic() + 10
            while not ready.exists():
                self.assertIsNone(process.poll(), "生产 checker 尚未完成实际 Git 深来源读取")
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            file = self.capacity.stamp if target == "stamp" else self.capacity.product / "sdk-lock.json"
            raw = file.read_bytes()
            file.chmod(0o600)
            file.write_bytes(raw + b"\n")
            if target == "stamp": file.chmod(0o400)
            release.write_text("continue actual Git")
            stdout, stderr = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 1, stdout + stderr)
            self.assertIn("检查期间改变", stderr)
        finally:
            release.write_text("finish")
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=10)

    def test_stamp_change_during_real_deep_object_proof_rejected(self):
        self.change_during_full_proof("stamp")

    def test_local_recipe_change_during_real_deep_object_proof_rejected(self):
        self.change_during_full_proof("recipe")

    def assert_recipe_gate_rejects_real_entries(self, reason):
        """实际 CLI 与 SDK 容量 import 消费者都先经普通小文件门。"""
        shutil.copyfile(Path(__file__).with_name("test_sdk_capacity.py"),
                        self.capacity.product / "tools/test_sdk_capacity.py")
        # A missing source must not reach path.resolve/Git/deep proof: recipe fails first.
        unavailable = self.capacity.sdk / "uninitialized-source"
        entries = (("check_sdk.py", "--path"), ("prepare_sdk.py", "--path"),
                   ("test_sdk_capacity.py", "--idf-path"))
        for entry, option in entries:
            with self.subTest(entry=entry):
                result = self.capacity.command(entry, option, str(unavailable), timeout=5)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(reason, result.stderr)
                self.assertNotIn("FileNotFoundError", result.stderr)
                self.assertNotIn("CalledProcessError", result.stderr)
                self.assertFalse(self.capacity.stamp.exists())

    def test_recipe_fifo_rejected_by_real_cli_and_import_before_source_proof(self):
        recipe = self.capacity.product / "sdk-lock.json"
        recipe.unlink()
        os.mkfifo(recipe)
        self.assert_recipe_gate_rejects_real_entries("普通文件")

    def test_recipe_symlink_rejected_by_real_cli_and_import_before_source_proof(self):
        recipe = self.capacity.product / "sdk-lock.json"
        original = self.root / "canonical-recipe-target.json"
        original.write_bytes(recipe.read_bytes())
        recipe.unlink()
        recipe.symlink_to(original)
        self.assert_recipe_gate_rejects_real_entries("普通文件")

    def test_recipe_over_64k_rejected_by_real_cli_and_import_before_source_proof(self):
        recipe = self.capacity.product / "sdk-lock.json"
        recipe.write_bytes(recipe.read_bytes() + b" " * 65537)
        self.assert_recipe_gate_rejects_real_entries("大小越界")

    def test_patch_change_between_verified_read_and_stable_snapshot_rejected(self):
        self.apply()
        declaration = self.capacity.recipe["managed_patches"][0]
        file = self.capacity.product / declaration["path"]
        actual_reader = SDK.read_source_file
        calls = 0

        def read_with_actual_patch_change(path, **kwargs):
            nonlocal calls
            if path == file:
                calls += 1
                if calls == 2:
                    # Change the real file; return only the production reader's actual bytes/stat.
                    file.write_bytes(file.read_bytes() + b"\n")
            return actual_reader(path, **kwargs)

        with patch.object(SDK, "ROOT", self.capacity.product), \
                patch.object(SDK, "read_source_file", side_effect=read_with_actual_patch_change):
            with self.assertRaisesRegex(ValueError, "稳定快照前改变"):
                SDK.check(self.capacity.sdk)
        self.assertEqual(calls, 2)
        self.assertNotEqual(hashlib.sha256(file.read_bytes()).hexdigest(), declaration["sha256"])

if __name__ == "__main__":
    unittest.main()
