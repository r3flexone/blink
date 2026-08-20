#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_TOUCHPAD, ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO, ESP_SLEEP_WAKEUP_UART,
} esp_sleep_wakeup_cause_t;
typedef enum { ESP_EXT1_WAKEUP_ALL_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1,
               ESP_EXT1_WAKEUP_ANY_LOW = 0 } esp_sleep_ext1_wakeup_mode_t;
typedef enum { ESP_PD_DOMAIN_RTC_PERIPH = 0, ESP_PD_DOMAIN_MAX } esp_sleep_pd_domain_t;
typedef enum { ESP_PD_OPTION_OFF = 0, ESP_PD_OPTION_ON = 1, ESP_PD_OPTION_AUTO = 2 } esp_sleep_pd_option_t;
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, esp_sleep_ext1_wakeup_mode_t mode);
esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t d, esp_sleep_pd_option_t o);
void esp_deep_sleep_start(void) __attribute__((noreturn));
