#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
bool rtc_gpio_is_valid_gpio(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
