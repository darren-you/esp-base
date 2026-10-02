#pragma once
#include "esp_http_client.h"
typedef struct esp_transport_fake *esp_transport_handle_t;
esp_err_t esp_transport_destroy(esp_transport_handle_t transport);
