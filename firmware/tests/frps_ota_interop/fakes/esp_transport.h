#pragma once
#include "esp_err.h"
typedef void *esp_transport_handle_t;
typedef struct { int unused; } esp_transport_keep_alive_t;
esp_err_t esp_transport_destroy(esp_transport_handle_t transport);
