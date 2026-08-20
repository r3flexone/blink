#pragma once
void esp_log_write(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) esp_log_write(1, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_write(2, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) esp_log_write(3, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) esp_log_write(4, tag, __VA_ARGS__)
