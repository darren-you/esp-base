#!/usr/bin/env python3
"""Only dial the official FRPS proxy; never the Base local management port."""
import hashlib
import json
from pathlib import Path
import sys

from frp_ota import FrpClient

endpoint, image, target, mode, output = sys.argv[1:]
target = {"esp32c3": "esp32c3/esp_base", "esp32": "esp32/esp_base"}[target]
client = FrpClient(endpoint, "22222222-2222-4222-8222-222222222222", bytes(range(32)))
operation = "44444444-4444-4444-8444-444444444444"
overlap = []
source_boot = client.status()["boot_id"]


def progress(count, total):
    if not overlap and count >= 4096:
        value = client.result(operation)
        assert value["state"] == "running"
        overlap.append(value)


value = client.ota_start(operation, Path(image), target,
    result_timeout_seconds=120, poll_interval_seconds=0.1, progress=progress)
expected = "succeeded" if mode == "success" else "failed" if mode == "write_failure" else "unknown"
assert value["state"] == expected, value
assert len(overlap) == 1
running = None
if expected == "succeeded":
    assert value["boot_id"] != source_boot
    running = client.firmware_status()
    assert running["boot_id"] == value["boot_id"]
    assert running["result"] == {"firmware_sha256": value["result"]["sha256"],
        "image_size_bytes": value["result"]["image_size_bytes"],
        "target": value["result"]["target"], "ota_slot": value["result"]["target_slot"]}
if expected != "unknown":
    assert value["result"]["operation_id"] == operation
    assert value["result"]["sha256"] == hashlib.sha256(Path(image).read_bytes()).hexdigest()
    assert value["result"]["image_size_bytes"] == Path(image).stat().st_size
else:
    assert value["result"] is None and value["error_code"] == "storage_uncertain"
again = client.result(operation)
assert again["state"] == expected
Path(output, "responses.json").write_text(json.dumps({"source_boot": source_boot, "overlap": overlap, "final": value, "running_firmware": running, "again": again}, indent=2) + "\n")
print(f"FRPS_HTTP_SCENARIO PASS mode={mode} final={expected} operation={operation} repeat_writes=0")
