#pragma once

void test_log(const char *format, ...);

#define ESP_LOGI(tag, ...) do { (void)(tag); test_log(__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); test_log(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); test_log(__VA_ARGS__); } while (0)
