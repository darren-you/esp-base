#pragma once
#include <stdio.h>

#define ESP_LOGI(tag, ...) ((void)(tag))
#ifdef ESP_BASE_TEST_CAPTURE_LOG
void esp_base_test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(tag, ...) esp_base_test_log(tag, __VA_ARGS__)
#else
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#endif
