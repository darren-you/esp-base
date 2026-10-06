#!/usr/bin/env python3
"""一次性官方 FRPS 宿主联调构建器；只使用回环与明确软件输入。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys

FRP_SHA = "989cc876d92b815aeb0b6806fb861f0ee2b39a86"
OTA_SHA = "8ab62f98fba2ea8e76c2822d0e7bf1cb523088a1"


def remove_function(source: str, name: str) -> str:
    # Only known test definitions are removed. The production include is retained.
    match = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError("原测试定义缺失：" + name)
    at = match.end()
    depth, state = 1, "code"
    while depth:
        ch, following = source[at], source[at:at + 2]
        if state == "code":
            if following in {"//", "/*"}:
                state = following; at += 1
            elif ch in "\"'":
                state = ch
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        elif state in {"\"", "'"}:
            if ch == "\\":
                at += 1
            elif ch == state:
                state = "code"
        elif state == "//" and ch == "\n":
            state = "code"
        elif state == "/*" and following == "*/":
            state = "code"; at += 1
        at += 1
    return source[:match.start()] + source[at:]


def prepare_owner(base: Path, output: Path) -> None:
    source = (base / "firmware/tests/protocol_ota_owner_test.c").read_text()
    source = source[:source.index("static int start_frp(")]
    names = ["eota_available", "eota_validate_image_request", "esp_timer_get_time",
             "psa_hash_setup", "psa_hash_update", "psa_hash_finish", "psa_hash_abort",
             "psa_hash_compute", "psa_crypto_init", "esp_base_ota_receipt_register",
             "esp_base_ota_receipt_load_for_recovery", "esp_base_ota_receipt_record_failure",
             "esp_base_ota_receipt_query", "xTaskCreate", "vTaskDelete", "vTaskDelay",
             "esp_restart", "esp_base_ota_policy", "eota_prepare", "eota_retire_inactive",
             "eota_select", "eota_error", "esp_base_ota_observe_firmware_set",
             "eota_observe_slots", "eota_sha256_verified_image", "eota_validate_stream_request",
             "eota_prepare_stream"]
    for name in names:
        source = remove_function(source, name)
    source = source.replace('"../components/device_protocol/esp_base_protocol.c"',
        json.dumps(str(base / "firmware/components/device_protocol/esp_base_protocol.c")))
    source = source.replace("static uint64_t fake_now_ms;", "static atomic_uint_fast64_t fake_now_ms;")
    (output / "owner_shim.c").write_text("/* Generated from explicit owner test fakes; production protocol unchanged. */\n" + source)


def run(command: list[str], log: Path, *, cwd: Path | None = None, timeout_s: int = 240) -> None:
    with log.open("w") as stream:
        process = subprocess.Popen(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            status = process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            # Go FRPS, C peer and Python consumer are all in this test process group.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=10)
            stream.write(f"\n阶段超过有限期限 {timeout_s}s；已终止整个测试进程组\n")
            raise RuntimeError(f"阶段超时 {log.name}；日志已保留") from None
    if status:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        print(log.read_text()[-12000:], file=sys.stderr)
        raise RuntimeError(f"阶段失败 {log.name}，exit={status}")


def bounded_image(path: Path, target: str) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or not 288 <= before.st_size <= 0x1e0000:
            raise ValueError("镜像必须为槽容量内的有限普通文件：" + str(path))
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(0x1e0000 + 1)
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns) or len(data) != before.st_size:
            raise ValueError("读取中镜像改变：" + str(path))
        if data[0] != 0xe9 or int.from_bytes(data[12:14], "little") != (5 if target == "esp32c3" else 0) or data[80:112].split(b"\0")[0] != b"esp_base":
            raise ValueError("镜像实际chip/project与请求目标不符：" + str(path))
        return data
    finally:
        os.close(fd)


def source_manifest(root: Path) -> list[dict]:
    return [{"path": str(path.relative_to(root)), "size_bytes": path.stat().st_size,
             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in sorted(root.rglob("*")) if path.is_file() and
            not {".git", "__pycache__"}.intersection(path.relative_to(root).parts)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--components", required=True, type=Path)
    parser.add_argument("--mbedtls-source", required=True, type=Path)
    parser.add_argument("--frp-catalog", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--source-image", required=True, type=Path)
    parser.add_argument("--verification-key", required=True, type=Path)
    parser.add_argument("--sdk-python", required=True, type=Path)
    parser.add_argument("--target", choices=["esp32c3", "esp32"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frozen-manifest", type=Path,
                        help="既有软件归档的managed逐文件摘要；默认使用本轮Base受限归档")
    args = parser.parse_args()
    os.umask(0o077)
    base, components = args.base_root.resolve(), args.components.resolve()
    output = args.output.resolve()
    output.mkdir(mode=0o700)  # Existing evidence is never overwritten.
    catalog = json.loads(args.frp_catalog.read_text())
    if catalog["frp_version"] != "0.71.0":
        raise ValueError("现有官方互操作夹具只锁定0.71.0；catalog漂移时须重新核对")
    lock_path = base / ("firmware/dependencies.lock.esp32" if args.target == "esp32" else "firmware/dependencies.lock")
    lock = lock_path.read_text()
    if FRP_SHA not in lock or OTA_SHA not in lock or not re.search(r"^target: " + re.escape(args.target) + r"$", lock, re.M):
        raise ValueError("Base冻结组件身份与联调预期不符")
    for component in ["esp_frp", "esp_ota"]:
        expected = re.search(r"^  " + component + r":\n    component_hash: ([0-9a-f]{64})$", lock, re.M)
        if not expected or (components / component / ".component_hash").read_text().strip() != expected[1]:
            raise ValueError("实际managed组件hash与所选target lock不符：" + component)
    image_original, source_original = args.image.absolute(), args.source_image.absolute()
    image_data, source_data = bounded_image(image_original, args.target), bounded_image(source_original, args.target)
    if image_data == source_data:
        raise ValueError("源A与候选C必须不同且完整候选在app槽内")
    fixture = Path(__file__).resolve().parents[1] / "firmware/tests/frps_ota_interop"
    scenario = Path(__file__).resolve().with_name("frps_ota_scenario.py")
    inputs = output / "inputs"; inputs.mkdir()
    image, source_image = inputs / "candidate.bin", inputs / "source.bin"
    image.write_bytes(image_data); source_image.write_bytes(source_data)
    shutil.copyfile(lock_path, inputs / lock_path.name)
    frozen_path = args.frozen_manifest or base / "receipts/private/native_software_20261006/manifest.json"
    frozen = json.loads(frozen_path.read_text())
    selected = [entry for entry in frozen if entry["path"].startswith(args.target + "/managed_components/")]
    if not selected:
        raise ValueError("缺少所选目标的既有managed冻结摘要")
    expected_files = {entry["path"].split("/managed_components/", 1)[1] for entry in selected}
    actual_files = {str(path.relative_to(components)) for path in components.rglob("*")
                    if path.is_file() and ".git" not in path.relative_to(components).parts}
    if expected_files != actual_files:
        raise ValueError("实际managed文件集合与既有冻结原件不符")
    for entry in selected:
        path = components / entry["path"].split("/managed_components/", 1)[1]
        if path.stat().st_size != entry["size_bytes"] or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("实际managed文件与既有冻结原件不符：" + str(path))
    (inputs / "selected_managed_manifest.json").write_text(json.dumps(selected, indent=2) + "\n")
    shutil.copyfile(args.frp_catalog, inputs / "frp_catalog.json")
    mbedtls_before = source_manifest(args.mbedtls_source.resolve())
    (inputs / "mbedtls_source_manifest.json").write_text(json.dumps(mbedtls_before, indent=2) + "\n")
    shutil.copytree(fixture / "fakes", inputs / "fakes")
    prepare_owner(base, inputs)
    go_dir = output / "go_fixture"; go_dir.mkdir()
    upstream = components / "esp_frp/tests/crypto-interop"
    for path in upstream.iterdir():
        if path.name in {"go.mod", "go.sum"} or (path.suffix == ".go" and path.name != "main.go"):
            shutil.copyfile(path, go_dir / path.name)
    helpers = (upstream / "main.go").read_text().split("func main() {", 1)[0]
    (go_dir / "upstream_helpers.go").write_text(helpers.replace('\t"flag"\n', "").replace('\t"os"\n', ""))
    shutil.copyfile(fixture / "frps_ota_fixture.go", go_dir / "main.go")
    # SDK signature acceptance remains a host substitute; both bytes are
    # separately verified by the actual official SDK utility before transport.
    for label, path in [("source", source_image), ("candidate", image)]:
        run([str(args.sdk_python), "-m", "espsecure", "verify-signature", "--version",
             "2" if args.target == "esp32c3" else "1", "--keyfile",
             str(args.verification_key.resolve()), str(path)], output / (label + "_signature.log"))
    build = output / "build"
    flags = "-fsanitize=address,undefined -fno-omit-frame-pointer -g -ffunction-sections -fdata-sections -DMBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS"
    run(["cmake", "-S", str(fixture), "-B", str(build), "-DBASE_ROOT=" + str(base),
         "-DBASE_COMPONENTS=" + str(components), "-DMBEDTLS_SOURCE=" + str(args.mbedtls_source.resolve()),
         "-DFIXTURE_INPUT=" + str(inputs), "-DCHIP_ID=" + ("5" if args.target == "esp32c3" else "0"),
         "-DCMAKE_C_FLAGS=" + flags], output / "configure.log")
    run(["cmake", "--build", str(build), "--target", "frps_ota_peer", "-j", "6"], output / "build.log", timeout_s=900)
    run(["go", "build", "-mod=readonly", "-o", str(output / "frps_fixture"), "."], output / "go_build.log", cwd=go_dir, timeout_s=900)
    for name, command in [("compiler", ["cc", "--version"]), ("cmake", ["cmake", "--version"]),
                          ("go", ["go", "version"]), ("sdk_python", [str(args.sdk_python), "--version"]),
                          ("go_module", ["go", "version", "-m", str(output / "frps_fixture")])]:
        run(command, inputs / (name + ".txt"), timeout_s=30)
    shutil.copyfile(build / "CMakeCache.txt", inputs / "CMakeCache.txt")
    run(["go", "list", "-m", "-json", "github.com/fatedier/frp"], inputs / "official_frp_module.json", cwd=go_dir, timeout_s=30)
    for mode in ["success", "write_failure", "nvs_failure"]:
        mode_dir = output / mode; mode_dir.mkdir()
        run([str(output / "frps_fixture"), "-peer", str(build / "frps_ota_peer"), "-image", str(image),
             "-source-image", str(source_image),
             "-scenario", str(scenario), "-target", args.target,
             "-mode", mode, "-output", str(mode_dir)], output / (mode + ".log"), cwd=go_dir)
    if source_manifest(args.mbedtls_source.resolve()) != mbedtls_before:
        raise ValueError("宿主MbedTLS源码在联调期间改变")
    for entry in selected:
        path = components / entry["path"].split("/managed_components/", 1)[1]
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("联调期间managed冻结原件改变：" + str(path))
    receipt = {"target": args.target, "frp_version": catalog["frp_version"], "frp_sha": FRP_SHA,
        "ota_sha": OTA_SHA, "image_sha256": hashlib.sha256(image_data).hexdigest(),
        "image_size_bytes": len(image_data), "source_sha256": hashlib.sha256(source_data).hexdigest(),
        "source_size_bytes": len(source_data), "loopback_only": True, "real_frps": True,
        "host_substitutes": ["DNS loopback mapping", "POSIX scheduler", "Flash", "SDK image/signature API",
                             "file-backed NVS", "reboot and local confirmation",
                             "device identity/version/heap", "Wi-Fi/MQTT/time readiness",
                             "Base FRP owner readiness snapshot"],
        "physical_or_public_qualification": False,
        "image_original": str(image_original), "source_original": str(source_original),
        "lock_file": str(lock_path), "lock_sha256": hashlib.sha256(lock_path.read_bytes()).hexdigest(),
        "components_path": str(components), "managed_verified_files": len(selected),
        "frozen_manifest": str(frozen_path), "frozen_manifest_sha256": hashlib.sha256(frozen_path.read_bytes()).hexdigest(),
        "mbedtls_path": str(args.mbedtls_source.resolve()),
        "mbedtls_manifest_sha256": hashlib.sha256((inputs / "mbedtls_source_manifest.json").read_bytes()).hexdigest(),
        "go_module_evidence": str(inputs / "go_module.txt"),
        "verification_key_sha256": hashlib.sha256(args.verification_key.read_bytes()).hexdigest(),
        "fixture_manifest": source_manifest(fixture),
        "scenario_path": str(scenario),
        "scenario_sha256": hashlib.sha256(scenario.read_bytes()).hexdigest()}
    (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    # copytree/CMake may retain public source modes inside the private root.
    for path in output.rglob("*"):
        if path.is_dir():
            path.chmod(0o700)
        elif path.is_file():
            path.chmod(0o700 if path.stat().st_mode & 0o111 else 0o600)
    (output / "evidence_manifest.json").write_text(json.dumps(source_manifest(output), indent=2) + "\n")
    print(f"官方FRPS宿主联调\n  目标  {args.target}\n  场景  3/3通过\n  实板/公网资格  未授予\n  证据  {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
