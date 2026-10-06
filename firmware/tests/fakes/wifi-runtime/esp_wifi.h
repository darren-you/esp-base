#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define ESP_ERR_WIFI_NOT_STARTED 0x3001
#define WIFI_AUTH_WPA2_PSK 1
#define WIFI_AUTH_WPA_WPA2_PSK 2
#define WIFI_AUTH_WPA3_PSK 3
#define WIFI_AUTH_WPA2_WPA3_PSK 4
#define WPA3_SAE_PWE_BOTH 1
#define WIFI_IF_STA 0
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 1

typedef struct { bool nvs_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct { int authmode; } threshold;
    int sae_pwe_h2e;
} wifi_sta_config_t;
typedef struct { wifi_sta_config_t sta; } wifi_config_t;
typedef struct { uint8_t ssid[33]; int authmode; } wifi_ap_record_t;

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record);
