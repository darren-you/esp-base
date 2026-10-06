#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

extern const char *WIFI_EVENT;
extern const char *IP_EVENT;
#define ESP_EVENT_ANY_ID (-1)
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_STOP 2
#define WIFI_EVENT_STA_CONNECTED 3
#define WIFI_EVENT_STA_DISCONNECTED 4
#define IP_EVENT_STA_GOT_IP 5
#define IP_EVENT_STA_LOST_IP 6

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance);
