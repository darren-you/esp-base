#include "esp_base_safety.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "base_safety";

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external_pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse_error";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        return "other";
    }
}

esp_err_t esp_base_safety_start(esp_base_safety_t *safety)
{
    if (safety == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    safety->reset_reason = reset_reason_name(esp_reset_reason());
    ESP_LOGI(TAG, "hardware outputs remain untouched; reset_reason=%s", safety->reset_reason);
    return ESP_OK;
}
