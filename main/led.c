#include "led.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "led";

static const blink_config_t *cfg = NULL;
static led_strip_handle_t strip;
static bool led_ok = false;

void led_init(const blink_config_t *c) {
    cfg = c;
    led_strip_config_t s = { .strip_gpio_num = cfg->ledGpio, .max_leds = 1 };
    led_strip_rmt_config_t r = { .resolution_hz = 10*1000*1000, .flags.with_dma = false };
    // Kein ESP_ERROR_CHECK: ein ungültiger ledGpio aus der NVS-Config würde
    // sonst einen Panic-Boot-Loop erzeugen, der nur per Flash-Erase endet.
    esp_err_t err = led_strip_new_rmt_device(&s, &r, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init fehlgeschlagen (GPIO %d): %s",
                 cfg->ledGpio, esp_err_to_name(err));
        return;
    }
    led_ok = true;
    led_strip_clear(strip);
}

// Helligkeit skalieren statt fest durch 16 zu teilen. Vorher landeten von den
// 24 Bit des Farbwaehlers im Panel nur 4 Bit pro Kanal auf der LED: #0F0F0F war
// aus, und #101010 bis #1F1F1F waren nicht zu unterscheiden. Der Default von
// ledBrightness = 16 entspricht genau der bisherigen Helligkeit.
void led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ok) return;
    uint32_t bright = (uint32_t)cfg->ledBrightness;
    led_strip_set_pixel(strip, 0,
                        (uint32_t)r * bright / 255,
                        (uint32_t)g * bright / 255,
                        (uint32_t)b * bright / 255);
    led_strip_refresh(strip);
}

void led_set_rgb(const uint8_t rgb[3]) { led_set(rgb[0], rgb[1], rgb[2]); }

void led_off(void) {
    if (!led_ok) return;
    led_strip_clear(strip);
    led_strip_refresh(strip);
}

void led_show_worst_status(const SbbDeparture deps[DEP_COUNT]) {
    int worst = 0;
    for (int i = 0; i < DEP_COUNT; i++) {
        if (!deps[i].valid) continue;
        int s = 0;
        if (deps[i].cancelled)                        s = 3;
        else if (deps[i].delay >= cfg->delayBigMin)   s = 2;
        else if (deps[i].delay >= cfg->delaySmallMin) s = 1;
        if (s > worst) worst = s;
    }
    switch (worst) {
        case 3:  led_set_rgb(cfg->ledCancelledRgb);  break;
        case 2:  led_set_rgb(cfg->ledDelayBigRgb);   break;
        case 1:  led_set_rgb(cfg->ledDelaySmallRgb); break;
        default: led_set_rgb(cfg->ledOkRgb);         break;
    }
}
