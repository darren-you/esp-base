#!/usr/bin/env python3
"""向独立 Base 源码归档加入 ESP32 QEMU 专用认证记录探针。"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


READY = '''    ESP_LOGI(TAG, "ESP_BASE_READY hardware_outputs=untouched provisioning=required container=%s",
             product == ESP_BASE_CONTAINER_RUNNING ? "running" :
             product == ESP_BASE_CONTAINER_EMPTY ? "empty" : "not_configured");
}'''

PROBE = r'''
/* 仓外 QEMU 探针：与 RUNNING guest 同进程，调用正式 provider 和 AEAD reader。 */
enum { QEMU_FRP_CIPHER_BYTES = 65536, QEMU_FRP_FEED_BYTES = 1024 };
static uint8_t s_qemu_frp_window[EFRP_AEAD_RX_CHUNK_BYTES];
static uint8_t s_qemu_frp_zero[QEMU_FRP_FEED_BYTES];
static const uint8_t s_qemu_frp_key[32] = {@@KEY@@};
static const uint8_t s_qemu_frp_prefix[16] = {@@PREFIX@@};
static const uint8_t s_qemu_frp_tag[16] = {@@TAG@@};
static const uint8_t s_qemu_frp_digest[32] = {@@DIGEST@@};

static void qemu_frp_failure(const char *stage, int result)
{
    ESP_LOGE(TAG, "QEMU_FRP_FAIL stage=%s result=%d owner_active=%u"
             " owner_next=%u store_state=%d lease=%" PRIu64,
             stage, result,
             atomic_load_explicit(&s_storage_owner.active_token, memory_order_acquire),
             atomic_load_explicit(&s_storage_owner.next_token, memory_order_acquire),
             s_frp_scratch.state, s_frp_scratch.active_lease);
    vTaskDelete(NULL);
}

#define QEMU_FRP_REQUIRE(condition, stage) do { \
    if (!(condition)) { qemu_frp_failure((stage), -1); return; } \
} while (0)
#define QEMU_FRP_OK(expression, stage) do { \
    efrp_result_t status = (expression); \
    if (status != EFRP_OK) { qemu_frp_failure((stage), status); return; } \
} while (0)

static void qemu_frp_sample(size_t *minimum_free, size_t *minimum_largest)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t largest_bytes = heap_caps_get_largest_free_block(caps);
    if (free_bytes < *minimum_free) *minimum_free = free_bytes;
    if (largest_bytes < *minimum_largest) *minimum_largest = largest_bytes;
}

static bool qemu_frp_feed_cipher(efrp_aead_flash_reader_t *reader,
                                  size_t *minimum_free, size_t *minimum_largest)
{
    for (size_t offset = 0; offset < QEMU_FRP_CIPHER_BYTES;) {
        size_t count = QEMU_FRP_CIPHER_BYTES - offset;
        if (count > sizeof s_qemu_frp_zero) count = sizeof s_qemu_frp_zero;
        size_t consumed = 0;
        if (efrp_aead_flash_feed(reader, s_qemu_frp_zero,
                                 count, &consumed) != EFRP_OK ||
            consumed != count) {
            return false;
        }
        offset += count;
        qemu_frp_sample(minimum_free, minimum_largest);
    }
    return true;
}

static void qemu_frp_authenticated_probe(void *context)
{
    (void)context;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t baseline_free = heap_caps_get_free_size(caps);
    const size_t baseline_largest = heap_caps_get_largest_free_block(caps);
    size_t minimum_free = baseline_free, minimum_largest = baseline_largest;
    const unsigned owner_before = atomic_load_explicit(
        &s_storage_owner.next_token, memory_order_acquire);
    const efrp_aead_flash_store_t *store =
        efrp_idf_flash_store_callbacks(&s_frp_scratch);
    QEMU_FRP_REQUIRE(store != NULL && s_frp_scratch.state == EFRP_IDF_FLASH_IDLE &&
                     s_frp_scratch.active_lease == 0 &&
                     atomic_load_explicit(&s_storage_owner.active_token,
                                          memory_order_acquire) == 0,
                     "initial_owner");
    ESP_LOGI(TAG, "QEMU_FRP_BEGIN guest=running owner=idle owner_next=%u"
             " baseline_free=%zu baseline_largest=%zu",
             owner_before, baseline_free, baseline_largest);

    efrp_aead_flash_reader_t reader = {0};
    QEMU_FRP_OK(efrp_aead_flash_reader_init(&reader, s_qemu_frp_key,
                                            store, s_qemu_frp_window,
                                            sizeof s_qemu_frp_window), "reader_init");
    size_t consumed = 0;
    QEMU_FRP_OK(efrp_aead_flash_feed(&reader, s_qemu_frp_prefix,
                                    sizeof s_qemu_frp_prefix, &consumed), "prefix");
    QEMU_FRP_REQUIRE(consumed == sizeof s_qemu_frp_prefix, "prefix_consumed");
    QEMU_FRP_REQUIRE(qemu_frp_feed_cipher(&reader, &minimum_free,
                                          &minimum_largest), "feed_cipher");
    const uint8_t *plaintext = NULL;
    size_t length = 0;
    QEMU_FRP_REQUIRE(efrp_aead_flash_plaintext(&reader, &plaintext, &length) ==
                     EFRP_WOULD_BLOCK && plaintext == NULL && length == 0 &&
                     reader.records == 0, "before_tag");
    const size_t feed_free = heap_caps_get_free_size(caps);
    const size_t feed_largest = heap_caps_get_largest_free_block(caps);
    consumed = 0;
    QEMU_FRP_OK(efrp_aead_flash_feed(&reader, s_qemu_frp_tag,
                                    sizeof s_qemu_frp_tag, &consumed), "valid_tag");
    QEMU_FRP_REQUIRE(consumed == sizeof s_qemu_frp_tag && reader.records == 1 &&
                     reader.flash_records == 1 && reader.flash_passes == 1 &&
                     reader.leased && reader.ready, "authenticated");
    const size_t auth_free = heap_caps_get_free_size(caps);
    const size_t auth_largest = heap_caps_get_largest_free_block(caps);
    qemu_frp_sample(&minimum_free, &minimum_largest);

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t digest[32];
    size_t digest_length = 0;
    QEMU_FRP_REQUIRE(psa_crypto_init() == PSA_SUCCESS &&
                     psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS,
                     "sha_start");
    for (unsigned index = 0; index < 16; ++index) {
        QEMU_FRP_OK(efrp_aead_flash_plaintext(&reader, &plaintext, &length),
                    "window_auth");
        QEMU_FRP_REQUIRE(plaintext != NULL && length == sizeof s_qemu_frp_window,
                         "window_length");
        QEMU_FRP_REQUIRE(psa_hash_update(&hash, plaintext, length) == PSA_SUCCESS,
                         "sha_update");
        QEMU_FRP_OK(efrp_aead_flash_consume_plaintext(&reader, length),
                    "window_consume");
        qemu_frp_sample(&minimum_free, &minimum_largest);
    }
    QEMU_FRP_REQUIRE(psa_hash_finish(&hash, digest, sizeof digest,
                                      &digest_length) == PSA_SUCCESS &&
                     digest_length == sizeof digest, "sha_finish");
    QEMU_FRP_REQUIRE(memcmp(digest, s_qemu_frp_digest, sizeof digest) == 0 &&
                     reader.flash_passes == 17 &&
                     reader.flash_read_bytes == UINT64_C(17) * QEMU_FRP_CIPHER_BYTES &&
                     !reader.leased && !reader.ready &&
                     s_frp_scratch.state == EFRP_IDF_FLASH_IDLE &&
                     s_frp_scratch.active_lease == 0, "all_windows_verified");
    QEMU_FRP_OK(efrp_aead_flash_finish(&reader), "valid_finish");
    QEMU_FRP_OK(efrp_aead_flash_reader_close(&reader), "valid_close");

    QEMU_FRP_OK(efrp_aead_flash_reader_init(&reader, s_qemu_frp_key,
                                            store, s_qemu_frp_window,
                                            sizeof s_qemu_frp_window), "bad_init");
    consumed = 0;
    QEMU_FRP_OK(efrp_aead_flash_feed(&reader, s_qemu_frp_prefix,
                                    sizeof s_qemu_frp_prefix, &consumed), "bad_prefix");
    QEMU_FRP_REQUIRE(consumed == sizeof s_qemu_frp_prefix, "bad_prefix_consumed");
    QEMU_FRP_REQUIRE(qemu_frp_feed_cipher(&reader, &minimum_free,
                                          &minimum_largest), "bad_feed_cipher");
    uint8_t bad_tag[sizeof s_qemu_frp_tag];
    memcpy(bad_tag, s_qemu_frp_tag, sizeof bad_tag);
    bad_tag[sizeof bad_tag - 1] ^= 1U;
    consumed = 0;
    QEMU_FRP_REQUIRE(efrp_aead_flash_feed(&reader, bad_tag, sizeof bad_tag,
                                          &consumed) == EFRP_AUTHENTICATION_FAILED &&
                     consumed == sizeof bad_tag && reader.records == 0 &&
                     reader.flash_records == 0, "bad_tag_rejected");
    QEMU_FRP_REQUIRE(efrp_aead_flash_plaintext(&reader, &plaintext, &length) ==
                     EFRP_AUTHENTICATION_FAILED && plaintext == NULL && length == 0,
                     "bad_tag_no_plaintext");
    QEMU_FRP_OK(efrp_aead_flash_reader_close(&reader), "bad_close");
    qemu_frp_sample(&minimum_free, &minimum_largest);
    for (size_t index = 0; index < sizeof s_qemu_frp_window; ++index)
        QEMU_FRP_REQUIRE(s_qemu_frp_window[index] == 0, "window_zero");
    const unsigned owner_after = atomic_load_explicit(
        &s_storage_owner.next_token, memory_order_acquire);
    QEMU_FRP_REQUIRE(owner_after > owner_before &&
                     atomic_load_explicit(&s_storage_owner.active_token,
                                          memory_order_acquire) == 0 &&
                     s_frp_scratch.state == EFRP_IDF_FLASH_IDLE &&
                     s_frp_scratch.active_lease == 0,
                     "final_owner");
    ESP_LOGI(TAG, "QEMU_FRP_PASS valid_bytes=65536 windows=16 passes=17"
             " read_bytes=1114112 bad_tag=rejected owner_claims=%u"
             " baseline_free=%zu baseline_largest=%zu feed_free=%zu feed_largest=%zu"
             " auth_free=%zu auth_largest=%zu final_free=%zu final_largest=%zu"
             " phase_min_free=%zu phase_min_largest=%zu boot_min_free=%zu"
             " stack_high_water=%u",
             owner_after - owner_before, baseline_free, baseline_largest,
             feed_free, feed_largest,
             auth_free, auth_largest, heap_caps_get_free_size(caps),
             heap_caps_get_largest_free_block(caps),
             minimum_free, minimum_largest,
             heap_caps_get_minimum_free_size(caps),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

'''


def c_array(data: bytes) -> str:
    return ", ".join(f"0x{byte:02x}" for byte in data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve(strict=True)
    if (root / ".git").exists():
        parser.error("只允许修改没有 .git 的独立源码归档")
    main_c = root / "firmware/apps/esp_base/main/esp_base_main.c"
    cmake = root / "firmware/apps/esp_base/main/CMakeLists.txt"
    original = main_c.read_text()
    if (original.count("void app_main(void)") != 1 or original.count(READY) != 1 or
            "QEMU_FRP_PASS" in original):
        parser.error("Base 插入点与固定源码不一致")
    key = bytes((index * 17 + 3) & 0xFF for index in range(32))
    nonce = bytes(range(12))
    header = (65536 + 16).to_bytes(4, "big")
    encryptor = Cipher(algorithms.AES(key), modes.CTR(nonce + (2).to_bytes(4, "big"))).encryptor()
    plaintext = encryptor.update(bytes(65536)) + encryptor.finalize()
    record = AESGCM(key).encrypt(nonce, plaintext, nonce + header)
    if record[:-16] != bytes(65536):
        raise RuntimeError("GCM 密文必须恰好为 64 KiB 零字节")
    probe = (PROBE.replace("@@KEY@@", c_array(key))
                  .replace("@@PREFIX@@", c_array(nonce + header))
                  .replace("@@TAG@@", c_array(record[-16:]))
                  .replace("@@DIGEST@@", c_array(hashlib.sha256(plaintext).digest())))
    updated = original.replace('#include "esp_base_container_product.h"',
        '#include "esp_base_container_product.h"\n#include "esp_frp_flash_reader.h"\n'
        '#include "esp_heap_caps.h"\n#include "psa/crypto.h"')
    updated = updated.replace("void app_main(void)", probe + "void app_main(void)")
    updated = updated.replace(READY, READY[:-1] + '''    if (product == ESP_BASE_CONTAINER_RUNNING &&
        xTaskCreate(qemu_frp_authenticated_probe, "qemu_frp_auth",
                    8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "QEMU_FRP_FAIL stage=task_create result=-1");
    }
}''')
    main_c.write_text(updated)
    cmake_text = cmake.read_text()
    if cmake_text.count("ota_operation remote_config safety_runtime time_runtime)") != 1:
        parser.error("CMake 依赖插入点与固定源码不一致")
    cmake.write_text(cmake_text.replace(
        "ota_operation remote_config safety_runtime time_runtime)",
        "ota_operation remote_config safety_runtime time_runtime mbedtls)"))
    print("QEMU 认证记录探针已写入独立源码归档")
    print(f"  原始主程序 SHA-256: {hashlib.sha256(original.encode()).hexdigest()}")
    print(f"  探针主程序 SHA-256: {hashlib.sha256(updated.encode()).hexdigest()}")
    print(f"  合法 GCM tag: {record[-16:].hex()}")
    print(f"  预期明文 SHA-256: {hashlib.sha256(plaintext).hexdigest()}")


if __name__ == "__main__":
    main()
