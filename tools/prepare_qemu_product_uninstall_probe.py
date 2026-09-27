#!/usr/bin/env python3
"""Add the scheduled Base uninstall probe to an isolated source archive.

The ordinary product has no product-operation caller, so its linker omits the
uninstall entry. This script modifies only the caller's archive copy. Signed
input, package seeding, QEMU execution and post-run Flash checks remain
explicit steps documented in docs/operations/product-uninstall-qemu-checkpoint.md.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


PROBE = '''/* QEMU-only scheduled caller. This file is modified only in the isolated
 * test archive; the managed product source has no product operation caller. */
static volatile unsigned s_qemu_probe_stage;
static volatile int s_qemu_probe_stop;
static volatile int s_qemu_probe_uninstall;
static volatile int s_qemu_probe_reopen;
static volatile int s_qemu_probe_release;

static void qemu_product_uninstall_probe(void *context)
{
    (void)context;
    static const uint8_t expected_package_sha256[32] = {
@@DIGEST@@
    };
    static const char operation_id[] = "99999999-9999-4999-8999-999999999999";
    esp_base_storage_claim_t claim = {0};
    if (!esp_base_storage_claim(&s_storage_owner, &claim)) {
        s_qemu_probe_stage = 10;
        ESP_LOGE(TAG, "QEMU_PROBE_CLAIM_FAILED");
        vTaskDelete(NULL);
        return;
    }
    s_qemu_probe_stage = 1;
    s_qemu_probe_stop = esp_base_container_product_stop_confirmed(&claim);
    ESP_LOGI(TAG, "QEMU_PROBE_STOP result=%d", s_qemu_probe_stop);
    if (s_qemu_probe_stop) {
        s_qemu_probe_stage = 2;
        s_qemu_probe_uninstall = esp_base_container_product_uninstall(
            &claim, operation_id, @@SEQUENCE@@U, expected_package_sha256);
        ESP_LOGI(TAG, "QEMU_PROBE_UNINSTALL result=%d", s_qemu_probe_uninstall);
        if (s_qemu_probe_uninstall == ESP_BASE_CONTAINER_UNINSTALL_COMPLETE) {
            s_qemu_probe_stage = 3;
            s_qemu_probe_reopen = esp_base_container_product_boot(
                &claim, esp_base_protocol_boot_id());
            ESP_LOGI(TAG, "QEMU_PROBE_SAME_BOOT result=%d", s_qemu_probe_reopen);
        }
    }
    s_qemu_probe_release = esp_base_storage_release(&claim);
    s_qemu_probe_stage = 4;
    ESP_LOGI(TAG, "QEMU_PROBE_DONE stop=%d uninstall=%d reopen=%d released=%d",
             s_qemu_probe_stop, s_qemu_probe_uninstall,
             s_qemu_probe_reopen, s_qemu_probe_release);
    vTaskDelete(NULL);
}

'''

READY = '''    ESP_LOGI(TAG, "ESP_BASE_READY hardware_outputs=untouched provisioning=required container=%s",
             product == ESP_BASE_CONTAINER_RUNNING ? "running" :
             product == ESP_BASE_CONTAINER_EMPTY ? "empty" : "not_configured");
}'''

CALL = '''    if (xTaskCreate(qemu_product_uninstall_probe, "qemu_product_probe",
                    16384, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "QEMU_PROBE_TASK_FAILED");
    }
}'''


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True,
                        help="independent Git archive directory, never the managed checkout")
    parser.add_argument("--package-sha256", required=True,
                        help="SHA-256 of the exact signed package in seeded ECS2")
    parser.add_argument("--expected-sequence", type=int, required=True,
                        help="seeded ECS2 sequence before uninstall")
    args = parser.parse_args()

    source_root = args.source_root.resolve(strict=True)
    if source_root.joinpath(".git").exists():
        parser.error("source-root must be an isolated archive without .git")
    try:
        digest = bytes.fromhex(args.package_sha256)
    except ValueError:
        parser.error("package-sha256 must be hexadecimal")
    if len(digest) != 32:
        parser.error("package-sha256 must contain exactly 32 bytes")
    if not 0 < args.expected_sequence < 0xFFFFFFFF:
        parser.error("expected-sequence must be between 1 and UINT32_MAX-1")

    main_c = source_root / "firmware/apps/esp_base/main/esp_base_main.c"
    original = main_c.read_text()
    if original.count("void app_main(void)") != 1 or original.count(READY) != 1:
        parser.error("Base app_main source does not match the tested insertion points")
    if "QEMU_PROBE_DONE" in original:
        parser.error("probe is already present")

    byte_lines = "\n".join(
        "        " + ", ".join(f"0x{byte:02x}" for byte in digest[offset:offset + 8]) + ","
        for offset in range(0, 32, 8)
    )
    probe = PROBE.replace("@@DIGEST@@", byte_lines).replace(
        "@@SEQUENCE@@", str(args.expected_sequence)
    )
    updated = original.replace("void app_main(void)", probe + "void app_main(void)")
    updated = updated.replace(READY, READY[:-1] + CALL)
    main_c.write_text(updated)
    print("QEMU 产品卸载测试源码")
    print(f"  输入: {main_c}")
    print(f"  原始 SHA-256: {sha256(original.encode())}")
    print(f"  测试 SHA-256: {sha256(updated.encode())}")
    print(f"  包 SHA-256: {digest.hex()}")
    print(f"  ECS2 初始序号: {args.expected_sequence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
