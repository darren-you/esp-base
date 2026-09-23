#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct { int unused; } esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(esp_netif_t *netif);
bool esp_netif_is_netif_up(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info);
