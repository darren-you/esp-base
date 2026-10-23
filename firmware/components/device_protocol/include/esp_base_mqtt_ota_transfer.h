// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_base_mqtt_ota_stream.h"

/* One static mailbox, owned by the already admitted OTA operation. Begin only
 * after receipt verification/inactive retirement. The control owner must bind
 * authentication to that immutable operation before offering any frame. */
bool esp_base_mqtt_ota_transfer_begin(uint32_t size, uint32_t total_timeout_ms,
    uint32_t idle_timeout_ms);
bool esp_base_mqtt_ota_transfer_request(ebase_mqtt_ota_frame_t *out);
ebase_mqtt_ota_admission_t esp_base_mqtt_ota_transfer_offer(const ebase_mqtt_ota_frame_t *verified);
int esp_base_mqtt_ota_transfer_read(void *context, uint8_t *out, size_t capacity,
    uint32_t timeout_ms);
bool esp_base_mqtt_ota_transfer_written(void *context, uint32_t written, uint32_t total);
bool esp_base_mqtt_ota_transfer_select(void);
/* Only the control owner, after the worker's completion has been observed. */
void esp_base_mqtt_ota_transfer_close(void);
