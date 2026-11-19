// SPDX-License-Identifier: Apache-2.0
#include "public_ip.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base_protocol.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TASK_STACK_BYTES 8192 /* TLS 握手和证书校验都在这个任务的栈上 */
#define TIMEOUT_MS 10000
#define BODY_BYTES 64         /* 应答体只是一个地址；更长的不是回显 */

static const char *TAG = "base_public_ip";
static atomic_bool s_running;
static base_public_ip_cb_t s_done;

static char *fetch(void)
{
    const esp_http_client_config_t http = {
        .url = CONFIG_ESP_BASE_PUBLIC_IP_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) return NULL;
    char body[BODY_BYTES];
    int length = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200)
        length = esp_http_client_read_response(client, body, sizeof body);
    else
        ESP_LOGW(TAG, "echo failed: status=%d", esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    char ip[BASE_IP_TEXT_BYTES];
    if (length <= 0 || length >= (int)sizeof body || !base_public_ip_parse(ip, body, (size_t)length)) {
        if (length >= 0) ESP_LOGW(TAG, "echo body is not an address (%d bytes)", length);
        return NULL;
    }
    char *copy = malloc(strlen(ip) + 1);
    if (copy) strcpy(copy, ip);
    return copy;
}

static void task(void *argument)
{
    (void)argument;
    char *ip = fetch();
    const base_public_ip_cb_t done = s_done;
    atomic_store(&s_running, false);
    done(ip);
    vTaskDelete(NULL);
}

bool base_public_ip_start(base_public_ip_cb_t done)
{
    if (!done || !CONFIG_ESP_BASE_PUBLIC_IP_URL[0]) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_running, &expected, true)) return false;
    s_done = done;
    if (xTaskCreate(task, "public_ip", TASK_STACK_BYTES, NULL, BASE_SYSTEM_TASK_PRIORITY, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        return false;
    }
    return true;
}
