#include "button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEBOUNCE_MS 30
#define POLL_MS     20

static const blink_config_t *cfg = NULL;

void button_init(const blink_config_t *c) {
    cfg = c;
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << cfg->buttonGpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
}

static bool button_down(void) { return gpio_get_level(cfg->buttonGpio) == 0; }

bool button_pressed(void) {
    if (!button_down()) return false;
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    return button_down();
}

void button_wait_release(int timeout_ms) {
    int waited = 0;
    while (button_down() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        waited += POLL_MS;
    }
}

int button_measure_hold(int max_ms) {
    int held = 0;
    while (button_down() && held < max_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held += 50;
    }
    return held;
}
