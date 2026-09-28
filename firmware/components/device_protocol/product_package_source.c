// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_package_source.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"

enum {
    PACKAGE_CONNECT_TIMEOUT_MS = 5000,
    PACKAGE_READ_TIMEOUT_MS = 1000,
    PACKAGE_IDLE_TIMEOUT_US = 30000000,
    PACKAGE_TOTAL_TIMEOUT_US = 300000000,
};

struct esp_base_product_package_source {
    esp_http_client_handle_t client;
    uint32_t expected_size_bytes;
    uint32_t received_bytes;
    int64_t started_us;
    int64_t progress_us;
    bool failed;
};

static bool valid_url(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) return false;
    const size_t length = strnlen(url, ESP_BASE_PRODUCT_PACKAGE_URL_BYTES + 1U);
    if (length <= 9U || length > ESP_BASE_PRODUCT_PACKAGE_URL_BYTES) return false;
    const char *authority = url + 8;
    const char *path = strchr(authority, '/');
    if (path == NULL || path == authority) return false;
    size_t hostname_length = 0U;
    bool port = false;
    unsigned port_value = 0U;
    for (const char *cursor = authority; cursor < path; ++cursor) {
        const unsigned char c = (unsigned char)*cursor;
        if (c == ':') {
            if (port || hostname_length == 0U || cursor + 1 == path) return false;
            port = true;
            continue;
        }
        if (port) {
            if (c < '0' || c > '9') return false;
            port_value = port_value * 10U + (unsigned)(c - '0');
            if (port_value > 65535U) return false;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
            if (++hostname_length > 253U) return false;
        }
    }
    if (authority[0] == '.' || authority[0] == '-' ||
        authority[hostname_length - 1U] == '.' ||
        authority[hostname_length - 1U] == '-' ||
        (port && port_value == 0U)) return false;
    for (const char *cursor = path; *cursor != '\0'; ++cursor) {
        const unsigned char c = (unsigned char)*cursor;
        if (c <= 0x20U || c >= 0x7fU || c == '#' || c == '\\') return false;
    }
    return true;
}

bool esp_base_product_package_source_request_valid(
    const char *url, uint32_t expected_size_bytes)
{
    return expected_size_bytes != 0U && expected_size_bytes <= INT_MAX &&
        valid_url(url);
}

static bool deadline_valid(const esp_base_product_package_source_t *source)
{
    const int64_t now = esp_timer_get_time();
    return now >= source->started_us && now >= source->progress_us &&
        now - source->started_us < PACKAGE_TOTAL_TIMEOUT_US &&
        now - source->progress_us < PACKAGE_IDLE_TIMEOUT_US;
}

static bool connect_deadline_valid(
    const esp_base_product_package_source_t *source)
{
    const int64_t now = esp_timer_get_time();
    return now >= source->started_us &&
        now - source->started_us < PACKAGE_CONNECT_TIMEOUT_MS * 1000;
}

void esp_base_product_package_source_close(
    esp_base_product_package_source_t *source)
{
    if (source == NULL) return;
    if (source->client != NULL) (void)esp_http_client_cleanup(source->client);
    free(source);
}

esp_base_product_package_source_t *esp_base_product_package_source_open(
    const char *url, uint32_t expected_size_bytes, bool trusted_time)
{
    if (!trusted_time ||
        !esp_base_product_package_source_request_valid(url, expected_size_bytes))
        return NULL;
    esp_base_product_package_source_t *source = calloc(1, sizeof *source);
    if (source == NULL) return NULL;
    source->expected_size_bytes = expected_size_bytes;
    source->started_us = esp_timer_get_time();
    source->progress_us = source->started_us;
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = PACKAGE_READ_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    source->client = esp_http_client_init(&config);
    if (source->client == NULL || !connect_deadline_valid(source) ||
        esp_http_client_open(source->client, 0) != ESP_OK ||
        !connect_deadline_valid(source)) goto fail;
    int64_t content_length = -1;
    do {
        if (!deadline_valid(source)) goto fail;
        content_length = esp_http_client_fetch_headers(source->client);
        if (!deadline_valid(source)) goto fail;
    } while (content_length == -ESP_ERR_HTTP_EAGAIN);
    if (content_length != expected_size_bytes ||
        esp_http_client_get_status_code(source->client) != 200 ||
        esp_http_client_is_chunked_response(source->client) ||
        esp_http_client_get_content_length(source->client) != expected_size_bytes ||
        !deadline_valid(source)) goto fail;
    source->progress_us = esp_timer_get_time();
    return source;
fail:
    esp_base_product_package_source_close(source);
    return NULL;
}

bool esp_base_product_package_source_read(void *context,
    size_t relative_offset_bytes, uint8_t *destination, size_t size_bytes)
{
    esp_base_product_package_source_t *source = context;
    if (source == NULL || source->failed) return false;
    if (destination == NULL || size_bytes == 0U ||
        size_bytes > INT_MAX || relative_offset_bytes != source->received_bytes ||
        size_bytes > source->expected_size_bytes - source->received_bytes) {
        source->failed = true;
        return false;
    }
    size_t received = 0U;
    while (received < size_bytes) {
        if (!deadline_valid(source)) break;
        const int count = esp_http_client_read(source->client,
            (char *)destination + received, (int)(size_bytes - received));
        if (!deadline_valid(source)) break;
        if (count == -ESP_ERR_HTTP_EAGAIN) continue;
        if (count <= 0 || (size_t)count > size_bytes - received) break;
        received += (size_t)count;
        source->progress_us = esp_timer_get_time();
    }
    if (received != size_bytes) {
        source->failed = true;
        return false;
    }
    source->received_bytes += (uint32_t)size_bytes;
    if (source->received_bytes == source->expected_size_bytes &&
        !esp_http_client_is_complete_data_received(source->client)) {
        source->failed = true;
        return false;
    }
    return true;
}

bool esp_base_product_package_source_complete(
    const esp_base_product_package_source_t *source)
{
    return source != NULL && !source->failed &&
        source->received_bytes == source->expected_size_bytes &&
        deadline_valid(source) &&
        esp_http_client_is_complete_data_received(source->client);
}
