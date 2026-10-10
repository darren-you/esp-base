// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

void esp_base_capacity_poll(const char *boot_id, uint64_t now_ms);
void esp_base_capacity_before_reset(const char *boot_id, uint64_t now_ms);
