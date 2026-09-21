#include "esp_base_protocol.h"

#include <inttypes.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "base_reported";
static esp_base_protocol_context_t s_context;
static uint32_t s_boot_id;

static void heartbeat_task(void *argument)
{
    (void)argument;
    while (true) {
        ESP_LOGI(TAG,
                 "ESP_BASE_REPORTED schema=1 boot_id=%08" PRIx32 " uptime_ms=%" PRIu64
                 " free_heap=%" PRIu32 " min_free_heap=%" PRIu32
                 " device_id=%s firmware=%s chip=%s flash=%" PRIu32 " config_generation=%" PRIu32
                 " reset=%s provisioned=%s",
                 s_boot_id,
                 (uint64_t)(esp_timer_get_time() / 1000),
                 esp_get_free_heap_size(),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
                 s_context.device_id,
                 s_context.firmware_version,
                 s_context.chip_model,
                 s_context.flash_size_bytes,
                 s_context.config_generation,
                 s_context.reset_reason,
                 s_context.provisioned ? "true" : "false");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_context = *context;
    s_boot_id = esp_random();
    const BaseType_t created = xTaskCreate(heartbeat_task, "base_heartbeat", 4096, NULL, 5, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
