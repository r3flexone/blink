#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct led_strip_t *led_strip_handle_t;
typedef struct {
    int strip_gpio_num;
    uint32_t max_leds;
    int led_model;
    struct { uint32_t invert_out: 1; } flags;
} led_strip_config_t;
typedef struct {
    int clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    struct { uint32_t with_dma: 1; } flags;
} led_strip_rmt_config_t;
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *s,
                                   const led_strip_rmt_config_t *r,
                                   led_strip_handle_t *out);
esp_err_t led_strip_set_pixel(led_strip_handle_t h, uint32_t i, uint32_t r, uint32_t g, uint32_t b);
esp_err_t led_strip_refresh(led_strip_handle_t h);
esp_err_t led_strip_clear(led_strip_handle_t h);
