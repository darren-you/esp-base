// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_package_source.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t body[] = {1, 2, 3, 4, 5, 6};
static int64_t clock_us;
static int64_t open_time_us, header_time_us, read_time_us;
static int64_t announced_length;
static int status_code;
static bool chunked, complete, open_ok, eagain_once, header_eagain_once;
static bool header_eagain_forever, read_eagain_forever;
static unsigned init_calls, open_calls, cleanup_calls, read_calls, delay_calls;
static size_t body_offset;

static void reset(void)
{
    clock_us = 1000;
    open_time_us = header_time_us = read_time_us = 0;
    announced_length = sizeof body;
    status_code = 200;
    chunked = false;
    complete = true;
    open_ok = true;
    eagain_once = header_eagain_once = false;
    header_eagain_forever = read_eagain_forever = false;
    init_calls = open_calls = cleanup_calls = read_calls = delay_calls = 0;
    body_offset = 0;
}

int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1U);
    ++delay_calls;
    clock_us += 1000;
}
int esp_crt_bundle_attach(void *config) { assert(config != NULL); return ESP_OK; }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config != NULL && config->url != NULL && config->crt_bundle_attach ==
           esp_crt_bundle_attach && config->disable_auto_redirect &&
           config->timeout_ms == 1000 && config->buffer_size == 1024);
    ++init_calls;
    return &clock_us;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    assert(client == &clock_us && write_len == 0);
    ++open_calls;
    clock_us += open_time_us;
    return open_ok ? ESP_OK : -1;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    assert(client == &clock_us);
    clock_us += header_time_us;
    if (header_eagain_once || header_eagain_forever) {
        header_eagain_once = false;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    return announced_length;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ assert(client == &clock_us); return status_code; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client)
{ assert(client == &clock_us); return chunked; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{ assert(client == &clock_us); return announced_length; }
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length)
{
    assert(client == &clock_us && buffer != NULL && length > 0);
    ++read_calls;
    clock_us += read_time_us;
    if (eagain_once || read_eagain_forever) {
        eagain_once = false;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if (body_offset == sizeof body) return 0;
    const size_t count = (size_t)length < 2U ? (size_t)length : 2U;
    const size_t actual = count < sizeof body - body_offset ?
        count : sizeof body - body_offset;
    memcpy(buffer, body + body_offset, actual);
    body_offset += actual;
    return (int)actual;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{ assert(client == &clock_us); return complete && body_offset == sizeof body; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ assert(client == &clock_us); ++cleanup_calls; return ESP_OK; }

static void valid_stream(void)
{
    reset();
    header_eagain_once = true;
    esp_base_product_package_source_t *source =
        esp_base_product_package_source_open("https://packages.example.test:443/product.pkg?token=a", 6U, true);
    assert(source != NULL && init_calls == 1U && open_calls == 1U);
    uint8_t bytes[6] = {0};
    eagain_once = true;
    assert(esp_base_product_package_source_read(source, 0, bytes, 3U));
    assert(esp_base_product_package_source_read(source, 3U, bytes + 3U, 3U));
    assert(!memcmp(bytes, body, sizeof body));
    clock_us += 300000000; /* Offline Flash validation follows the final read. */
    assert(esp_base_product_package_source_complete(source));
    assert(delay_calls == 2U);
    esp_base_product_package_source_close(source);
    assert(cleanup_calls == 1U && read_calls >= 4U);

    reset();
    source = esp_base_product_package_source_open("https://host/x", 6U, true);
    assert(source != NULL);
    assert(esp_base_product_package_source_read(source, 0U, bytes, 3U));
    assert(!esp_base_product_package_source_read(source, 0U, bytes, 1U));
    assert(!esp_base_product_package_source_read(source, 3U, bytes + 3U, 3U));
    assert(!esp_base_product_package_source_complete(source));
    esp_base_product_package_source_close(source);
}

static void reject_bad_urls(void)
{
    static const char *const rejected[] = {
        "http://packages.example.test/x", "https://", "https://host",
        "https://user@host/x", "https://host:0/x", "https://host:65536/x",
        "https://host/x#part", "https://host/x\\y", "https://host/x y",
        "https://.host/x", "https://-host/x",
    };
    for (size_t index = 0; index < sizeof rejected / sizeof rejected[0]; ++index) {
        reset();
        assert(!esp_base_product_package_source_request_valid(rejected[index], 6U));
        assert(esp_base_product_package_source_open(rejected[index], 6U, true) == NULL);
        assert(init_calls == 0U);
    }
    reset();
    assert(esp_base_product_package_source_request_valid("https://host/x", 6U));
    assert(!esp_base_product_package_source_request_valid("https://host/x", 0U));
    assert(esp_base_product_package_source_open("https://host/x", 6U, false) == NULL);
    assert(esp_base_product_package_source_open("https://host/x", 0U, true) == NULL);
    assert(init_calls == 0U);
}

static void reject_wrong_response(void)
{
    reset();
    announced_length = 5;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    assert(cleanup_calls == 1U);
    reset();
    status_code = 302;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    reset();
    chunked = true;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    reset();
    open_ok = false;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    reset();
    open_time_us = 5000000;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    reset();
    header_time_us = 300000000;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    reset();
    header_eagain_forever = true;
    assert(esp_base_product_package_source_open("https://host/x", 6U, true) == NULL);
    assert(delay_calls == 30000U && cleanup_calls == 1U);
}

static void reject_late_or_incomplete_body(void)
{
    reset();
    esp_base_product_package_source_t *source =
        esp_base_product_package_source_open("https://host/x", 6U, true);
    assert(source != NULL);
    uint8_t bytes[6] = {0};
    read_time_us = 30000000;
    assert(!esp_base_product_package_source_read(source, 0, bytes, 6U));
    assert(!esp_base_product_package_source_complete(source));
    esp_base_product_package_source_close(source);

    reset();
    source = esp_base_product_package_source_open("https://host/x", 6U, true);
    assert(source != NULL);
    read_eagain_forever = true;
    assert(!esp_base_product_package_source_read(source, 0U, bytes, 6U));
    assert(delay_calls == 30000U);
    assert(!esp_base_product_package_source_complete(source));
    esp_base_product_package_source_close(source);

    reset();
    source = esp_base_product_package_source_open("https://host/x", 6U, true);
    assert(source != NULL);
    complete = false;
    assert(!esp_base_product_package_source_read(source, 0, bytes, 6U));
    assert(!esp_base_product_package_source_complete(source));
    esp_base_product_package_source_close(source);
}

int main(void)
{
    valid_stream();
    reject_bad_urls();
    reject_wrong_response();
    reject_late_or_incomplete_body();
    puts("  product_package_source passed (TLS, exact length, consecutive stream, deadlines)");
}
