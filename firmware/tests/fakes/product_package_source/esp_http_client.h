#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
typedef struct {
    const char *url;
    esp_err_t (*crt_bundle_attach)(void *config);
    bool disable_auto_redirect;
    int timeout_ms;
    int buffer_size;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
