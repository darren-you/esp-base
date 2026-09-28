// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESP_BASE_PRODUCT_PACKAGE_URL_BYTES 1024U

typedef struct esp_base_product_package_source esp_base_product_package_source_t;

/* Pure request preflight, suitable before committing an operation intent. */
bool esp_base_product_package_source_request_valid(
    const char *url, uint32_t expected_size_bytes);

/* Open one authenticated, fixed-length HTTPS response for an already admitted
 * product operation. The caller must have verified this boot's trusted time
 * before opening TLS and must retain the original operation claim. */
esp_base_product_package_source_t *esp_base_product_package_source_open(
    const char *url, uint32_t expected_size_bytes, bool trusted_time);

/* Container requests consecutive chunks only. A skipped or repeated offset,
 * truncated body or expired transport fails closed before Flash receives it. */
bool esp_base_product_package_source_read(void *context,
    size_t relative_offset_bytes, uint8_t *destination, size_t size_bytes);

/* After the final chunk, require the SDK to have consumed exactly one body. */
bool esp_base_product_package_source_complete(
    const esp_base_product_package_source_t *source);
void esp_base_product_package_source_close(
    esp_base_product_package_source_t *source);
