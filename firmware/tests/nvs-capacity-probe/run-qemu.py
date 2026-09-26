#!/usr/bin/env python3
"""Run one synthetic ESP32-C3 NVS phase in QEMU; never opens a serial device."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import time

FLASH_BYTES = 4 * 1024 * 1024
APP_OFFSET = 0x10000
APP_BYTES = 0x100000


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--flash", required=True, type=Path)
    parser.add_argument("--stage", required=True, type=int, choices=(1, 2, 3))
    parser.add_argument("--qemu", default="qemu-system-riscv32",
                        help="Espressif QEMU binary with esp32c3 machine support")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--stop-after-revision", type=int,
                        help="stage 2 only: terminate after completed stats for this revision")
    parser.add_argument("--expect-revision", type=int,
                        help="stage 3 restart revision; defaults to 100")
    parser.add_argument("--log-prefix", type=str,
                        help="safe output prefix for an additional synthetic run")
    args = parser.parse_args()
    if args.stop_after_revision is not None and (
        args.stage != 2 or not 4 <= args.stop_after_revision <= 100
    ):
        parser.error("--stop-after-revision requires stage 2 and revision 4..100")
    if args.expect_revision is not None and args.stage != 3:
        parser.error("--expect-revision is stage 3 only")
    if args.log_prefix is not None and not re.fullmatch(r"[a-z0-9-]+", args.log_prefix):
        parser.error("--log-prefix must be lowercase kebab-case")

    project_root = Path(__file__).resolve().parents[3]
    image = args.flash.resolve()
    if image.is_relative_to(project_root):
        parser.error("--flash must be outside the repository")
    image.parent.mkdir(parents=True, exist_ok=True)
    build = args.build_dir.resolve()
    manifest = json.loads((build / "flasher_args.json").read_text())
    flash_files = {int(offset, 16): build / name
                   for offset, name in manifest["flash_files"].items()}
    app = flash_files.get(APP_OFFSET)
    if app is None:
        parser.error("build has no factory app at 0x10000")
    app_bytes = app.read_bytes()
    if len(app_bytes) > APP_BYTES:
        parser.error("probe app exceeds its synthetic factory partition")

    if args.stage == 1:
        if image.exists():
            parser.error("stage 1 requires a new flash path")
        flash = bytearray(b"\xff" * FLASH_BYTES)
        for offset, path in flash_files.items():
            payload = path.read_bytes()
            if offset + len(payload) > FLASH_BYTES:
                parser.error(f"flash file exceeds 4 MiB: {path}")
            flash[offset:offset + len(payload)] = payload
        image.write_bytes(flash)
    else:
        if image.stat().st_size != FLASH_BYTES:
            parser.error("existing synthetic flash must be exactly 4 MiB")
        with image.open("r+b") as file:
            file.seek(APP_OFFSET)
            file.write(b"\xff" * APP_BYTES)
            file.seek(APP_OFFSET)
            file.write(app_bytes)

    command = [args.qemu, "-nographic", "-machine", "esp32c3",
               "-drive", f"file={image},if=mtd,format=raw"]
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert process.stdout is not None
    reader = selectors.DefaultSelector()
    reader.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    expected = f"PROBE_DONE=stage{args.stage}".encode()
    stop_marker = (f"PROBE_STATS={args.stop_after_revision} ".encode()
                   if args.stop_after_revision is not None else None)
    def completed_stop_line(data: bytes) -> bool:
        return stop_marker is not None and any(
            line.startswith(stop_marker) and line.endswith(b"\n")
            for line in data.splitlines(keepends=True)
        )
    deadline = time.monotonic() + args.timeout_seconds
    try:
        while time.monotonic() < deadline:
            for key, _ in reader.select(timeout=1):
                chunk = os.read(key.fd, 4096)
                if chunk:
                    output.extend(chunk)
            if expected in output or b"PROBE_FAIL=" in output or completed_stop_line(output):
                break
            if process.poll() is not None:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        reader.close()

    prefix = args.log_prefix or f"stage{args.stage}"
    log = image.with_name(f"{prefix}-qemu.log")
    log.write_bytes(output)
    probe_lines = [line.strip() for line in output.decode(errors="replace").splitlines()
                   if "PROBE_" in line]
    evidence = image.with_name(f"{prefix}-evidence.log")
    evidence.write_text("\n".join(probe_lines) + "\n")
    steps = sum(line.startswith("PROBE_STEP=") for line in probe_lines)
    expected_steps = (args.stop_after_revision - 3 if stop_marker is not None
                      else {1: 3, 2: 97, 3: 0}[args.stage])
    restart_ok = args.stage == 1 or "PROBE_RESTART_MATCH=1" in probe_lines
    stale_cas_ok = args.stage != 1 or any(
        line.startswith("PROBE_STALE_CAS=") and
        len(line.split()) == 2 and
        line.split()[0].split("=")[1] == line.split()[1].split("=")[1]
        for line in probe_lines
    )
    wanted_revision = args.expect_revision if args.expect_revision is not None else 100
    revision_ok = args.stage != 3 or all(
        any(line.startswith(f"{key}={wanted_revision} ") for line in probe_lines)
        for key in ("PROBE_RESTART_CONFIG_REV", "PROBE_RESTART_OTA_REV",
                    "PROBE_RESTART_CONTAINER_REV")
    )
    completion_ok = (completed_stop_line(output) if stop_marker is not None
                     else expected in output)
    success = (completion_ok and b"PROBE_FAIL=" not in output and
               steps == expected_steps and restart_ok and stale_cas_ok and revision_ok)
    print(f"stage={args.stage} steps={steps} marker={completion_ok} "
          f"restart_ok={restart_ok} stale_cas_ok={stale_cas_ok} "
          f"revision_ok={revision_ok} terminated_after_sdk_return={stop_marker is not None} "
          f"log={evidence}")
    for line in probe_lines[-8:]:
        print(line)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
