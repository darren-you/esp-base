#!/usr/bin/env python3
"""业务应用的正式打包入口，由 Fast Deploy 的 package Job 调用：构建、签名、验签并封装成 ZIP。

环境变量：
  ESP_BASE_APP_PROJECT      应用工程目录（含 Cargo.toml）；相对路径以当前目录（产品仓根）为准
  ESP_BASE_APPLICATION_ID   Fast Deploy 身份里的应用 ID（package.id）
  ESP_BASE_SIGNING_KEY_FILE 或 ESP_BASE_SIGNING_KEY_ENV（FILE:VAR）  仓外的 RSA-3072 签名键，与固件是同一把
  ESP_BASE_SIGNING_TRUST    release 或 software-test；后者不得用于正式设备
Job 的声明：platform 为 plugin，variant 是应用名，architectures 是 [wasm32]。版本和构建号由 Fast Deploy 分配并写进
FAST_DEPLOY_IDENTITY_FILE；应用工程 Cargo.toml 的 version 必须等于这个版本。本入口不登记、不部署，也不连接设备。

包内：<应用名>.ebapp 是签名的应用包（登记到 ESP Tool Server 的就是它）；<应用名>.wasm 是包里的模块，供排查；
Cargo.lock 是依赖锁；manifest.json 记录清单、构建输入和各文件的摘要；build.json 是本次身份。
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import esp_base
import esp_base_app
import package_entry

ARCHITECTURE = "wasm32"
TARGET = "wasm32v1-none"


def project_directory(value):
    project = Path(value or "").resolve()
    if not value or not (project / "Cargo.toml").is_file():
        esp_base.fail("ESP_BASE_APP_PROJECT 要指向应用工程目录（含 Cargo.toml）")
    return project


def upward(start, name, stop):
    """从 start 往上找 name，最远到 stop（工作区根）；Cargo 和 rustup 也是这样找配置的。"""
    for directory in (start, *start.parents):
        if (directory / name).is_file():
            return directory / name
        if directory == stop:
            break
    return None


def workspace_root(project):
    """Cargo 工作区的根（Cargo.lock 在这里）。带 --locked：依赖锁缺失或与 Cargo.toml 不一致时不替工程生成或改写它。"""
    result = subprocess.run([esp_base.cargo(), "metadata", "--format-version", "1", "--no-deps", "--locked"], cwd=project,
                            capture_output=True, text=True)
    if result.returncode:
        reason = (result.stderr.strip().splitlines() or ["cargo metadata 失败"])[-1]
        esp_base.fail("正式包要用入库的、与 Cargo.toml 一致的 Cargo.lock 构建：" + reason)
    return Path(json.loads(result.stdout)["workspace_root"])


def toolchain(project):
    """Fast Deploy 冻结的工具链输入：这个工程实际用到的 Rust 工具链，以及锁、工具链声明和编译配置的摘要。

    不含签名键，执行时也不提供签名键。SDK 是工程里按路径引用的 esp-base，随产品仓的提交固定。
    """
    cargo = esp_base.cargo()

    def version(tool):
        return subprocess.check_output([str(Path(cargo).with_name(tool)), "--version"], cwd=project, text=True).strip()

    workspace = workspace_root(project)
    sources = {}
    for label, path in (("Cargo.lock", workspace / "Cargo.lock"), ("Cargo.toml", project / "Cargo.toml"),
                        ("workspace/Cargo.toml", workspace / "Cargo.toml" if workspace != project else None),
                        ("rust-toolchain.toml", upward(project, "rust-toolchain.toml", workspace)),
                        (".cargo/config.toml", upward(project, ".cargo/config.toml", workspace))):
        if path and path.is_file():
            sources[label] = esp_base.sha256(path)
    try:
        name = esp_base_app.manifest(project, 0, 1)["name"]
    except esp_base_app.AppError as error:
        esp_base.fail(str(error))
    return {"schema_version": 1, "app": name, "rust": {"rustc": version("rustc"), "cargo": version("cargo"), "target": TARGET},
            "sources": sources}


def inputs(project):
    """写进包内 manifest 的输入：工具链加签名信任根。"""
    key, trust = package_entry.signing_arguments()
    return {**toolchain(project),
            "signing": {"scheme": "rsa-3072-v2", "trust": trust, "public_key_sha256": esp_base.public_key_sha256(key)}}


def formal_identity(name):
    """读取 Fast Deploy 写出的本次身份，核对平台、应用身份、架构和应用名。"""
    path = os.environ.get("FAST_DEPLOY_IDENTITY_FILE", "")
    application = os.environ.get("ESP_BASE_APPLICATION_ID", "")
    if not path or not Path(path).is_absolute():
        esp_base.fail("本入口只能由 Fast Deploy 的 package Job 调用（缺少 FAST_DEPLOY_IDENTITY_FILE）")
    if not application:
        esp_base.fail("需要 ESP_BASE_APPLICATION_ID")
    expected = json.loads(Path(path).read_bytes())
    if (expected.get("platform") != "plugin" or expected.get("application_id") != application
            or expected.get("architecture") != ARCHITECTURE or expected.get("variant") != name
            or expected.get("local_validation") is not False or not isinstance(expected.get("version"), str)
            or type(expected.get("build_number")) is not int or expected["build_number"] < 1):
        esp_base.fail(f"本次身份不是应用 {name}（{application}）的正式身份")
    return expected


def package(artifact):
    run_directory = artifact.parent
    project = project_directory(os.environ.get("ESP_BASE_APP_PROJECT"))
    key, trust = package_entry.signing_arguments()
    frozen = inputs(project)
    name = frozen["app"]
    expected = formal_identity(name)
    version = f'{expected["version"]}+{expected["build_number"]}'

    # 构建目录放在本次 run 的收据目录里，不改动源码树；依赖按入库的 Cargo.lock。
    built = esp_base.build_app(project, expected["build_number"], locked=True, target_dir=run_directory / "app-build")
    if built["version"] != version:
        esp_base.fail(f'应用工程 Cargo.toml 的 version 与 Fast Deploy 分配的版本（{expected["version"]}）不一致')
    signed, manifest = esp_base.sign_app(built["package"], key)
    described = esp_base_app.describe(signed.read_bytes())
    if not described["signed"] or described["manifest"] != manifest or manifest["name"] != name or manifest["version"] != version:
        esp_base.fail("签名后的应用包与构建结果不一致")
    if inputs(project) != frozen:
        esp_base.fail("构建期间输入发生变化")

    stage = run_directory / "app-package"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    files = {f"{name}.ebapp": signed, f"{name}.wasm": Path(built["wasm"]), "Cargo.lock": workspace_root(project) / "Cargo.lock"}
    for entry, source in files.items():
        shutil.copyfile(source, stage / entry)
    document = {"schema_version": 1, "kind": "esp-base-app", "app": manifest, "inputs": frozen,
                "package": f"{name}.ebapp", "wasm_bytes": built["wasm_bytes"], "linear_bytes": built["linear_bytes"],
                "memory_estimate": built["memory_estimate"], "imports": built["imports"],
                "files": {entry: {"bytes": (stage / entry).stat().st_size, "sha256": esp_base.sha256(stage / entry)}
                          for entry in sorted(files)}}
    (stage / "manifest.json").write_bytes(package_entry.encoded(document))
    local = {k: v for k, v in expected.items() if k not in ("build_number", "source_commit_sha", "input_sha256")}
    local.update(local_validation=True, run_id="")
    (stage / "build.json").write_bytes(package_entry.encoded(local))
    for path in stage.iterdir():
        path.chmod(0o644)
    if formal_identity(name) != expected:
        esp_base.fail("构建期间本次身份发生变化")
    report = package_entry.package_directory(stage, expected, artifact)
    return {"artifact": report, "app_sha256": document["files"][f"{name}.ebapp"]["sha256"], "name": name, "version": version,
            "signing_trust": trust}


def main():
    try:
        if len(sys.argv) == 3 and sys.argv[1] == "inputs":
            sys.stdout.buffer.write(package_entry.encoded(toolchain(project_directory(sys.argv[2]))))
        elif len(sys.argv) == 2:
            # 签名和验签用官方的 espsecure，它在 IDF 自己的 Python 环境里。
            package_entry.ensure_idf_environment(__file__)
            sys.stdout.buffer.write(package_entry.encoded(package(Path(sys.argv[1]))))
        else:
            esp_base.fail("用法：app_package_entry.py inputs <应用工程目录>，或由 Fast Deploy 传入 ZIP 路径")
    except subprocess.CalledProcessError as error:
        esp_base.fail(f"{Path(str(error.cmd[0])).name} 失败，退出码 {error.returncode}")


if __name__ == "__main__":
    main()
