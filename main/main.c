#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "sbb.h"
#include "secrets.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "nvs_config.h"
#include "http_server.h"
#include "nvs_flash.h"
#include "mdns.h"

static const char *TAG = "main";

// Konfiguration — geladen aus NVS in app_main, überall verfügbar
static blink_config_t cfg;

// Gesetzt vom HTTP-Server nach erfolgreichem POST /api/config
volatile bool g_cfg_dirty = false;

// Für GET /api/departures und /api/status (Deklarationen in http_server.h)
SbbDeparture g_last_deps[DEP_COUNT];
time_t g_last_deps_time = 0;
volatile bool g_in_window = false;
volatile bool g_run_forever = false;
time_t g_active_end_time = 0;

#define OLED_WIDTH  128
#define OLED_HEIGHT  64
// Der 5x7-Font belegt inkl. Spalte Abstand 6 px pro Zeichen.
#define OLED_COLS   (OLED_WIDTH / 6)

// pdMS_TO_TICKS() rechnet intern in 32 Bit (ms * configTICK_RATE_HZ) und
// läuft ab ca. 11.9 h über — ein Zeitfenster von 23 h oder ein OLED-Invert-
// Intervall von 1440 min ergäbe sonst eine viel zu kurze Dauer. Deshalb
// Minuten/Sekunden direkt in Ticks umrechnen.
#define MAX_DURATION_MIN (7 * 24 * 60)
static TickType_t minutes_to_ticks(uint32_t minutes) {
    if (minutes > MAX_DURATION_MIN) minutes = MAX_DURATION_MIN;
    return (TickType_t)minutes * 60 * configTICK_RATE_HZ;
}
static TickType_t seconds_to_ticks(uint32_t seconds) {
    if (seconds > MAX_DURATION_MIN * 60) seconds = MAX_DURATION_MIN * 60;
    return (TickType_t)seconds * configTICK_RATE_HZ;
}

// ===== NEOPIXEL =====
static led_strip_handle_t led_strip;
static bool led_ok = false;
static void led_init(void) {
    led_strip_config_t s = { .strip_gpio_num = cfg.ledGpio, .max_leds = 1 };
    led_strip_rmt_config_t r = { .resolution_hz = 10*1000*1000, .flags.with_dma = false };
    // Kein ESP_ERROR_CHECK: ein ungültiger ledGpio aus der NVS-Config würde
    // sonst einen Panic-Boot-Loop erzeugen, der nur per Flash-Erase endet.
    esp_err_t err = led_strip_new_rmt_device(&s, &r, &led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED init fehlgeschlagen (GPIO %d): %s",
                 cfg.ledGpio, esp_err_to_name(err));
        return;
    }
    led_ok = true;
    led_strip_clear(led_strip);
}
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ok) return;
    led_strip_set_pixel(led_strip, 0, r/16, g/16, b/16);
    led_strip_refresh(led_strip);
}
// Schlimmster Status aller gültigen Züge: Ausfall > grosse > kleine Verspätung > OK
static void led_show_worst_status(const SbbDeparture deps[DEP_COUNT]) {
    int worst = 0;
    for (int i = 0; i < DEP_COUNT; i++) {
        if (!deps[i].valid) continue;
        int s = 0;
        if (deps[i].cancelled)                       s = 3;
        else if (deps[i].delay >= cfg.delayBigMin)   s = 2;
        else if (deps[i].delay >= cfg.delaySmallMin) s = 1;
        if (s > worst) worst = s;
    }
    switch (worst) {
        case 3:  led_set(cfg.ledCancelledRgb[0],  cfg.ledCancelledRgb[1],  cfg.ledCancelledRgb[2]);  break;
        case 2:  led_set(cfg.ledDelayBigRgb[0],   cfg.ledDelayBigRgb[1],   cfg.ledDelayBigRgb[2]);   break;
        case 1:  led_set(cfg.ledDelaySmallRgb[0], cfg.ledDelaySmallRgb[1], cfg.ledDelaySmallRgb[2]); break;
        default: led_set(cfg.ledOkRgb[0],         cfg.ledOkRgb[1],         cfg.ledOkRgb[2]);         break;
    }
}

// ===== OLED =====
static i2c_master_dev_handle_t oled_dev;
static uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];
static bool oled_ok = false;

static void oled_cmd(uint8_t cmd) {
    if (!oled_dev) return;   // OLED-Init fehlgeschlagen oder noch nicht erfolgt
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_transmit(oled_dev, buf, 2, 100);
}
static void oled_flush(void) {
    if (!oled_ok) return;
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);
    oled_cmd(0x22); oled_cmd(0); oled_cmd(7);
    uint8_t buf[OLED_WIDTH + 1];
    for (int p = 0; p < 8; p++) {
        buf[0] = 0x40;
        memcpy(&buf[1], &framebuffer[p * OLED_WIDTH], OLED_WIDTH);
        i2c_master_transmit(oled_dev, buf, sizeof(buf), 100);
    }
}
static void oled_init_display(void) {
    // Tippfehler im Panel ("3G", "abc") darf nicht in einer unbrauchbaren
    // I2C-Adresse enden — ausserhalb des gültigen 7-Bit-Bereichs: Default.
    int oled_addr = (int)strtol(cfg.oledAddr, NULL, 16);
    if (oled_addr < 0x08 || oled_addr > 0x77) {
        ESP_LOGW(TAG, "OLED-Adresse '%s' ungueltig, nutze 0x3C", cfg.oledAddr);
        oled_addr = 0x3C;
    }
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = cfg.sclGpio, .sda_io_num = cfg.sdaGpio,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)oled_addr, .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &oled_dev) != ESP_OK) {
        oled_dev = NULL;
        i2c_del_master_bus(bus);   // sonst bleibt der I2C-Port belegt
        return;
    }
    oled_cmd(0xAE); oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x3F); oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40); oled_cmd(0x8D); oled_cmd(0x14);
    oled_cmd(0x20); oled_cmd(0x00); oled_cmd(0xA1); oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x12); oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0xF1); oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0xA4); oled_cmd(0xA6); oled_cmd(0xAF);
    oled_ok = true;
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
}
static void draw_pixel(int x, int y, bool on) {
    if (x<0||x>=OLED_WIDTH||y<0||y>=OLED_HEIGHT) return;
    if (on) framebuffer[x+(y/8)*OLED_WIDTH] |=  (1<<(y%8));
    else    framebuffer[x+(y/8)*OLED_WIDTH] &= ~(1<<(y%8));
}

// ===== FONT =====
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t font_umlaut[][5] = {
    {0x7D,0x12,0x11,0x12,0x7D},{0x3D,0x42,0x41,0x42,0x3D},{0x3E,0x41,0x40,0x41,0x3E},
    {0x22,0x54,0x54,0x54,0x78},{0x38,0x45,0x44,0x45,0x38},{0x3C,0x41,0x40,0x41,0x7C},
};
static void draw_glyph(int x, int y, const uint8_t *g) {
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            draw_pixel(x+c, y+r, (g[c]>>r) & 1);
}
static int draw_char_utf8(int x, int y, const unsigned char *s, int *consumed) {
    *consumed = 1;
    unsigned char c = s[0];
    if (c == 0xC3 && s[1] != 0) {
        *consumed = 2;
        unsigned char b = s[1];
        switch (b) {
            case 0x84: draw_glyph(x,y,font_umlaut[0]); return x+6;
            case 0x96: draw_glyph(x,y,font_umlaut[1]); return x+6;
            case 0x9C: draw_glyph(x,y,font_umlaut[2]); return x+6;
            case 0xA4: draw_glyph(x,y,font_umlaut[3]); return x+6;
            case 0xB6: draw_glyph(x,y,font_umlaut[4]); return x+6;
            case 0xBC: draw_glyph(x,y,font_umlaut[5]); return x+6;
        }
        char base = 0;
        if      ((b >= 0x80 && b <= 0x85) || (b >= 0xA0 && b <= 0xA5)) base = 'A';
        else if (b == 0x87 || b == 0xA7)                               base = 'C';
        else if ((b >= 0x88 && b <= 0x8B) || (b >= 0xA8 && b <= 0xAB)) base = 'E';
        else if ((b >= 0x8C && b <= 0x8F) || (b >= 0xAC && b <= 0xAF)) base = 'I';
        else if (b == 0x91 || b == 0xB1)                               base = 'N';
        else if ((b >= 0x92 && b <= 0x98) || (b >= 0xB2 && b <= 0xB8)) base = 'O';
        else if ((b >= 0x99 && b <= 0x9B) || (b >= 0xB9 && b <= 0xBB)) base = 'U';
        else if (b == 0x9D || b == 0xBD)                               base = 'Y';
        if (base) {
            draw_glyph(x, y, font5x7[base - 0x20]);
            return x + 6;
        }
        *consumed = 1;
    }
    // Folgebytes einer nicht unterstützten UTF-8-Sequenz (z.B. 3-Byte-Zeichen)
    // überspringen, ohne zu zeichnen — sonst frisst ein einzelnes Zeichen
    // mehrere Leerstellen auf der Zeile.
    if (c >= 0x80 && c <= 0xBF) return x;
    char ch = (char)c;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) { draw_glyph(x,y,font5x7[ch-32]); return x+6; }
    return x + 6;
}
// Kopiert hoechstens max_glyphs darstellbare Zeichen aus src nach dst.
// snprintf("%.Ns") zaehlt Bytes: "Delemont" mit Akzent belegt 9 Bytes, aber nur
// 8 Zellen — die Zeile wurde damit mal zu kurz, mal (mit Gleis-Suffix) breiter
// als die 128 px des Displays. draw_char_utf8() rendert jede UTF-8-Sequenz als
// genau eine 6-px-Zelle, also wird hier genauso gezaehlt.
static void copy_glyphs(char *dst, size_t dst_size, const char *src, int max_glyphs) {
    size_t o = 0;
    int glyphs = 0;
    for (size_t i = 0; src[i] && glyphs < max_glyphs; ) {
        unsigned char c = (unsigned char)src[i];
        size_t seq = 1;
        if      ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        // Am Stringende abgeschnittene Sequenz nicht halb uebernehmen
        for (size_t k = 1; k < seq; k++) if (!src[i + k]) { seq = 1; break; }
        if (o + seq >= dst_size) break;
        memcpy(dst + o, src + i, seq);
        o += seq; i += seq; glyphs++;
    }
    dst[o] = 0;
}

static void draw_text(int x, int y, const char *text) {
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        int consumed = 1;
        x = draw_char_utf8(x, y, s, &consumed);
        s += consumed;
    }
}
static void draw_header(const char *title, bool stale) {
    memset(framebuffer, 0, sizeof(framebuffer));
    char hdr[20];
    if (stale) snprintf(hdr, sizeof(hdr), "!%.14s", title);
    else       snprintf(hdr, sizeof(hdr), "%.15s", title);
    draw_text(0, 0, hdr);
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    if (ti.tm_year >= 100) {
        char clk[6];
        snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
        draw_text(128 - 5*6, 0, clk);
    }
    for (int x = 0; x < 128; x++) draw_pixel(x, 9, true);
}

// Textzeile y invertieren (Ausfall-Markierung)
static void invert_row(int y) {
    for (int px = 0; px < OLED_WIDTH; px++)
        for (int py = y; py < y + 8; py++) {
            bool cur = (framebuffer[px + (py/8)*OLED_WIDTH] >> (py%8)) & 1;
            draw_pixel(px, py, !cur);
        }
}

static void display_departures(SbbDeparture deps[DEP_COUNT], bool stale) {
    draw_header(cfg.station, stale);
    static const int yp[DEP_COUNT] = {14, 27, 40, 53};
    for (int i = 0; i < DEP_COUNT; i++) {
        if (!deps[i].valid) continue;
        int y = yp[i];
        char line[64];

        if (deps[i].cancelled) {
            snprintf(line, sizeof(line), "%s AUSFALL", deps[i].time);
            draw_text(0, y, line);
            invert_row(y);
            continue;
        }

        // Prefix "HH:MM" bzw. "HH:MM+7", Suffix " G12" — beides reines ASCII,
        // also ist strlen() hier zugleich die Zellenzahl. Was davon uebrig
        // bleibt, bekommt das Ziel; frueher standen feste %.Ns-Grenzen da, die
        // zusammen mit dem Gleis-Suffix ueber die 21 Zellen hinausliefen und
        // die letzte Gleisziffer abschnitten.
        char prefix[12], suffix[8] = "";
        if (deps[i].delay > 0) {
            int dly = deps[i].delay > 99 ? 99 : deps[i].delay;
            snprintf(prefix, sizeof(prefix), "%s+%d", deps[i].time, dly);
        } else {
            snprintf(prefix, sizeof(prefix), "%s", deps[i].time);
        }
        if (deps[i].platform[0])
            snprintf(suffix, sizeof(suffix), " G%.2s", deps[i].platform);

        int budget = OLED_COLS - (int)strlen(prefix) - 1 - (int)strlen(suffix);
        char dest[sizeof(deps[i].destination)];
        copy_glyphs(dest, sizeof(dest), deps[i].destination, budget > 0 ? budget : 0);
        snprintf(line, sizeof(line), "%s %s%s", prefix, dest, suffix);
        draw_text(0, y, line);
    }
    oled_flush();
}

static void display_error(void) {
    draw_header(cfg.station, false);
    draw_text(0, 20, "API FEHLER");
    draw_text(0, 32, "PRUEFE NETZ...");
    oled_flush();
}

// ===== BUTTON =====
// Entprellt: ein einzelner Störimpuls auf GPIO 0 soll das Gerät nicht schlafen legen.
static bool button_pressed(void) {
    if (gpio_get_level(cfg.buttonGpio) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(30));
    return gpio_get_level(cfg.buttonGpio) == 0;
}

// Wartet (begrenzt) bis der Taster losgelassen ist.
static void wait_button_release(int timeout_ms) {
    int waited = 0;
    while (gpio_get_level(cfg.buttonGpio) == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
}

// ===== COUNTDOWN BAR =====
static void draw_countdown_bar(TickType_t active_start, TickType_t active_end) {
    TickType_t now = xTaskGetTickCount();
    int total = (int)(active_end - active_start);
    int remaining = (int)(active_end - now);
    if (remaining < 0) remaining = 0;
    if (total <= 0) return;
    int bar_w = (remaining * OLED_WIDTH) / total;
    for (int x = 0; x < OLED_WIDTH; x++) {
        if (x < bar_w)
            framebuffer[x + 7 * OLED_WIDTH] |= 0xC0;
        else
            framebuffer[x + 7 * OLED_WIDTH] &= ~0xC0;
    }
}

static void flush_page7(void) {
    if (!oled_ok) return;
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);
    oled_cmd(0x22); oled_cmd(7); oled_cmd(7);
    uint8_t buf[OLED_WIDTH + 1];
    buf[0] = 0x40;
    memcpy(&buf[1], &framebuffer[7 * OLED_WIDTH], OLED_WIDTH);
    i2c_master_transmit(oled_dev, buf, sizeof(buf), 100);
}

// Countdown-Balken aktualisieren und nur Page 7 flushen.
// Bei run_forever bleibt der Balken voll (total=1, remaining=1).
static void redraw_bar(bool run_forever, TickType_t active_start, TickType_t active_end) {
    TickType_t t = xTaskGetTickCount();
    draw_countdown_bar(run_forever ? t : active_start, run_forever ? t + 1 : active_end);
    flush_page7();
}

// Restlaufzeit als Wanduhr-Zeitpunkt spiegeln, damit GET /api/status sagen
// kann, bis wann das Gerät wach bleibt — Ticks nützen dem Panel nichts.
static void publish_active_end(TickType_t end) {
    TickType_t now_ticks = xTaskGetTickCount();
    time_t now_wall; time(&now_wall);
    uint32_t remain_s = (end > now_ticks)
        ? (uint32_t)((end - now_ticks) / configTICK_RATE_HZ) : 0;
    g_active_end_time = now_wall + (time_t)remain_s;
}

// ===== SLEEP =====
static void go_to_sleep(uint64_t us) {
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
    if (oled_ok) oled_cmd(0xAE);
    if (led_ok) { led_strip_clear(led_strip); led_strip_refresh(led_strip); }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_timer_wakeup(us);
    // Interner Pullup gilt im Deep Sleep nur über die RTC-Domain — ohne
    // externen Pullup würde der Button-Pin sonst floaten (Geister-Wakeups).
    if (rtc_gpio_is_valid_gpio(cfg.buttonGpio)) {
        rtc_gpio_pullup_en(cfg.buttonGpio);
        rtc_gpio_pulldown_dis(cfg.buttonGpio);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup(1ULL << cfg.buttonGpio, ESP_EXT1_WAKEUP_ANY_LOW);
    } else {
        ESP_LOGW(TAG, "GPIO %d ist kein RTC-GPIO — kein Button-Wakeup", cfg.buttonGpio);
    }
    esp_deep_sleep_start();
}

// ===== WIFI =====
// NVS-Credentials, mit secrets.h als Compile-Time-Fallback
static void wifi_connect_from_cfg(void) {
    const char *ssid = cfg.ssid[0]     ? cfg.ssid     : WIFI_SSID;
    const char *pass = cfg.password[0] ? cfg.password : WIFI_PASS;
    sbb_wifi_init(ssid, pass);
}

// Reconnect anstossen und in 1-s-Scheiben darauf warten, dazwischen den Taster
// pollen. Ein einzelner 15-s-Block in sbb_wifi_reconnect() liess das Geraet bei
// WLAN-Problemen so lange nicht auf "Schlafen" reagieren.
#define WIFI_RECONNECT_TIMEOUT_S 15
static void wifi_reconnect_interruptible(bool *force_sleep) {
    if (sbb_wifi_is_connected()) return;
    sbb_wifi_reconnect_start();
    for (int i = 0; i < WIFI_RECONNECT_TIMEOUT_S; i++) {
        if (sbb_wifi_wait_connected(1000)) return;
        if (button_pressed()) {
            ESP_LOGI(TAG, "Button während Reconnect → Schlaf");
            wait_button_release(5000);
            *force_sleep = true;
            return;
        }
    }
}

// ===== NTP =====
static bool ntp_sync(void) {
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    if (!esp_sntp_enabled()) esp_sntp_init();
    else { esp_sntp_stop(); esp_sntp_init(); }

    time_t now; struct tm ti;
    int steps = (cfg.ntpTimeoutS * 1000) / 250;
    for (int i = 0; i < steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        time(&now); localtime_r(&now, &ti);
        if (ti.tm_year >= 100) {
            ESP_LOGI(TAG, "NTP OK (%02d:%02d:%02d)", ti.tm_hour, ti.tm_min, ti.tm_sec);
            return true;
        }
    }
    ESP_LOGE(TAG, "NTP FAIL nach %d s", cfg.ntpTimeoutS);
    return false;
}

// ===== BOARD INFO =====
static void log_board_info(void) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%d.%d, %d Core(s)",
             ci.revision / 100, ci.revision % 100, ci.cores);
    ESP_LOGI(TAG, "Heap frei: %lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
}

// ===== SCHLAF-INFO =====
static const char *WEEKDAY_ABBR[7] = {"SO","MO","DI","MI","DO","FR","SA"};

static void show_sleep_info(int sleep_min) {
    if (!oled_ok) return;
    draw_header("SCHLAFE", false);

    time_t now; struct tm nt;
    time(&now); localtime_r(&now, &nt);
    char info[24];
    // Ohne gueltige RTC-Zeit waere jede "BIS HH:MM"-Angabe frei erfunden
    // (sie kaeme aus dem Epoch-Startwert) — dann nur die Dauer zeigen.
    if (nt.tm_year >= 100) {
        time_t wake = now + (time_t)sleep_min * 60;
        struct tm wt; localtime_r(&wake, &wt);
        // Wochentag dazu, sobald der Schlaf ueber Mitternacht reicht: beim
        // Wochenend-Schlaf sagte "BIS 05:00" sonst nicht, welcher Tag gemeint war.
        if (wt.tm_wday == nt.tm_wday && sleep_min < 24 * 60)
            snprintf(info, sizeof(info), "BIS %02d:%02d", wt.tm_hour, wt.tm_min);
        else
            snprintf(info, sizeof(info), "BIS %s %02d:%02d",
                     WEEKDAY_ABBR[wt.tm_wday], wt.tm_hour, wt.tm_min);
        draw_text(0, 20, info);
    }

    if (sleep_min >= 60) {
        snprintf(info, sizeof(info), "(%dH %dMIN)", sleep_min / 60, sleep_min % 60);
    } else {
        snprintf(info, sizeof(info), "(%d MIN)", sleep_min);
    }
    draw_text(0, 32, info);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(2000));
}

// ===== ZEITFENSTER =====
// Liegt cur_min in einem konfigurierten Fenster? Setzt *rem_min (falls != NULL)
// auf die Minuten bis zum Fensterende. Ende <= Start heisst "ueber Mitternacht".
static bool find_active_window(const blink_config_t *c, int cur_min, int *rem_min) {
    for (int i = 0; i < c->timeWindowCount; i++) {
        int ws = c->timeWindows[i].startH * 60 + c->timeWindows[i].startM;
        int we = c->timeWindows[i].endH   * 60 + c->timeWindows[i].endM;
        bool inside = (we > ws) ? (cur_min >= ws && cur_min < we)
                                : (cur_min >= ws || cur_min < we);
        if (!inside) continue;
        if (rem_min) {
            int rem = we - cur_min;
            if (rem <= 0) rem += 24 * 60;   // Ende liegt am Folgetag
            *rem_min = rem;
        }
        return true;
    }
    return false;
}

// Minuten bis zum naechsten Fensterstart, der auch wirklich aktiv wird.
// Bei weekdaysOnly muessen Sa/So uebersprungen werden: sonst rechnete das
// Geraet am Samstag "noch 30 Min bis 06:45" aus und zeigte das auch an,
// obwohl dieses Fenster am Wochenende gar nicht greift.
static int minutes_to_next_window(const blink_config_t *c, const struct tm *ti) {
    int cur_min = ti->tm_hour * 60 + ti->tm_min;
    int best = 7 * 24 * 60;
    for (int day = 0; day <= 7; day++) {
        int wday = (ti->tm_wday + day) % 7;
        if (c->weekdaysOnly && (wday == 0 || wday == 6)) continue;
        for (int i = 0; i < c->timeWindowCount; i++) {
            int ws = c->timeWindows[i].startH * 60 + c->timeWindows[i].startM;
            int diff = day * 24 * 60 + ws - cur_min;
            if (diff > 0 && diff < best) best = diff;
        }
    }
    return best;
}

// g_in_window fuer GET /api/status nachfuehren. Muss regelmaessig laufen: bei
// run_forever laeuft das Geraet ueber das Fensterende hinaus weiter, und ein
// Config-Reload kann das gerade aktive Fenster entfernt haben. Frueher wurde
// das Flag genau einmal beim Start gesetzt und nie wieder — das Panel meldete
// dann bis zum Neustart "Im aktiven Zeitfenster".
static void publish_in_window(void) {
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    if (ti.tm_year < 100) { g_in_window = false; return; }
    bool weekend_skip = cfg.weekdaysOnly && (ti.tm_wday == 0 || ti.tm_wday == 6);
    g_in_window = !weekend_skip &&
                  find_active_window(&cfg, ti.tm_hour * 60 + ti.tm_min, NULL);
}

// ===== ZEITFENSTER-VALIDIERUNG =====
static void check_window_overlaps(void) {
    for (int i = 0; i < cfg.timeWindowCount; i++) {
        int s1 = cfg.timeWindows[i].startH * 60 + cfg.timeWindows[i].startM;
        int e1 = cfg.timeWindows[i].endH * 60 + cfg.timeWindows[i].endM;
        // e1 < s1 ist ein gewolltes Fenster über Mitternacht; nur e1 == s1 ist leer.
        if (e1 == s1) {
            ESP_LOGW(TAG, "Zeitfenster %d: Laenge 0!", i + 1);
        }
        bool wrap1 = (e1 <= s1);
        for (int j = i + 1; j < cfg.timeWindowCount; j++) {
            int s2 = cfg.timeWindows[j].startH * 60 + cfg.timeWindows[j].startM;
            int e2 = cfg.timeWindows[j].endH * 60 + cfg.timeWindows[j].endM;
            bool wrap2 = (e2 <= s2);
            // Overlap-Pruefung gilt nur fuer zwei nicht-umschlagende Fenster.
            if (!wrap1 && !wrap2 && s1 < e2 && s2 < e1) {
                ESP_LOGW(TAG, "Zeitfenster %d und %d ueberlappen!", i + 1, j + 1);
            }
        }
    }
}

// ===== MAIN =====
static bool in_weekend_window(const struct tm *ti, const blink_config_t *c) {
    int cur   = ti->tm_wday * 24 * 60 + ti->tm_hour * 60 + ti->tm_min;
    int start = c->weekendStartDay * 24 * 60 + c->weekendStartH * 60 + c->weekendStartM;
    int end   = c->weekendEndDay   * 24 * 60 + c->weekendEndH   * 60 + c->weekendEndM;
    if (start <= end) return (cur >= start && cur < end);
    return (cur >= start || cur < end);
}

void app_main(void) {
    // NVS + Config ganz oben (vor Hardware-Init, da GPIO-Pins aus Config kommen)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_config_load(&cfg);

    // Singular-API: auf allen ESP-IDF v5.x verfügbar. ESP_SLEEP_WAKEUP_UNDEFINED == 0
    // (Kaltstart), daher bleiben die wakeup==0 / !=0 Prüfungen weiter unten gültig.
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    bool woken_by_button = (wakeup == ESP_SLEEP_WAKEUP_EXT1);

    led_init();
    oled_init_display();

    if (wakeup == 0) {
        log_board_info();
        check_window_overlaps();
    }

    // Button-GPIO immer konfigurieren (für Halt-Erkennung und Sleep-Taste)
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << cfg.buttonGpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    int button_active_min = cfg.buttonActiveMin;
    if (woken_by_button) {
        int hold_ms = 0;
        while (gpio_get_level(cfg.buttonGpio) == 0 &&
               hold_ms < cfg.buttonLongPressMs + 1000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            hold_ms += 50;
        }
        if (hold_ms >= cfg.buttonLongPressMs) {
            button_active_min = cfg.buttonLongActiveMin;
        }
        ESP_LOGI(TAG, "Button %d ms -> %d Min aktiv", hold_ms, button_active_min);
        // Die Messschleife bricht nach buttonLongPressMs + 1 s ab. Wird der
        // Taster länger gehalten, sähe die Aktiv-Schleife ihn sofort als
        // "Sleep"-Druck und das Gerät schliefe direkt wieder ein.
        wait_button_release(10000);
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    bool time_valid = (ti.tm_year >= 100);
    bool ap_mode = false;
    bool wifi_started = false;
    bool ntp_tried = false;

    if (!time_valid) {
        if (oled_ok) {
            oled_cmd(0xAF);
            draw_header("KALTSTART", false);
            draw_text(0, 20, "WIFI+NTP...");
            oled_flush();
        }
        led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);
        wifi_connect_from_cfg();
        wifi_started = true;
        ap_mode = sbb_wifi_is_ap_mode();
        if (!ap_mode) {
            time_valid = ntp_sync();
            ntp_tried = true;
            time(&now); localtime_r(&now, &ti);
        }
    }

    int cur_min = ti.tm_hour * 60 + ti.tm_min;
    bool is_weekend = (ti.tm_wday == 0 || ti.tm_wday == 6);
    bool weekend_skip = cfg.weekdaysOnly && is_weekend;

    int active_rem_min = 0;   // Minuten bis zum Fenster-Ende (wrap-fähig)
    bool in_window = time_valid && !weekend_skip &&
                     find_active_window(&cfg, cur_min, &active_rem_min);
    g_in_window = in_window;

    if (!in_window && !woken_by_button && cfg.sleepEnabled && !ap_mode) {
        uint64_t sleep_us;
        int d;
        if (time_valid) {
            d = minutes_to_next_window(&cfg, &ti);
            if (cfg.weekendSleepEnabled && in_weekend_window(&ti, &cfg)) {
                int end_abs = cfg.weekendEndDay * 24 * 60 + cfg.weekendEndH * 60 + cfg.weekendEndM;
                int cur_abs = ti.tm_wday * 24 * 60 + cur_min;
                int d_weekend = end_abs - cur_abs;
                if (d_weekend < 0) d_weekend += 7 * 24 * 60;
                d = d_weekend;
                ESP_LOGI(TAG, "Wochenend-Fenster: schlafe %d Min", d);
            } else {
                if (d > cfg.sleepMaxMin) d = cfg.sleepMaxMin;
                if (weekend_skip) ESP_LOGI(TAG, "Wochenende, schlafe %d Min", d);
                else              ESP_LOGI(TAG, "Schlafe %d Min", d);
            }
            // d == 0 (genau auf der Fenstergrenze) hiesse "sofort aufwachen"
            if (d < 1) d = 1;
            sleep_us = (uint64_t)d * 60ULL * 1000000ULL;
        } else {
            // Ohne gültige Zeit exakt sleepFallbackS schlafen — die frühere
            // Umrechnung auf volle Minuten machte aus 90 s stille 60 s.
            sleep_us = (uint64_t)cfg.sleepFallbackS * 1000000ULL;
            d = (cfg.sleepFallbackS + 59) / 60;
        }
        show_sleep_info(d);
        go_to_sleep(sleep_us);
        return;
    }

    if (woken_by_button) ESP_LOGI(TAG, "Button aktiv (%d Min)", button_active_min);
    else                 ESP_LOGI(TAG, "Zeitfenster aktiv");

    if (oled_ok) {
        oled_cmd(0xAF);
        draw_header(cfg.station, false);
        draw_text(0, 20, "LADE ZUEGE...");
        oled_flush();
    }
    led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);

    // WiFi in jedem Fall aufbauen, wenn es die Kaltstart-Phase nicht schon tat.
    // Früher hing das an "wakeup != 0": nach einem esp_restart (Panel-Neustart)
    // ist die Wake-Cause aber UNDEFINED, während die RTC-Zeit gültig bleibt —
    // das Gerät lief dann ohne Netz weiter und zeigte dauerhaft "API FEHLER".
    if (!wifi_started) {
        wifi_connect_from_cfg();
        wifi_started = true;
        ap_mode = sbb_wifi_is_ap_mode();
    }
    if (!ap_mode && !ntp_tried) {
        // Stösst SNTP erneut an und korrigiert so die RTC-Drift über lange
        // Schlafphasen hinweg (kehrt sofort zurück, wenn die Zeit schon passt).
        ntp_sync();
        ntp_tried = true;
    }

    // mDNS + HTTP-Server (WiFi/netif bereits durch sbb_wifi_init aktiv)
    if (!ap_mode) {
        mdns_init();
        mdns_hostname_set("sbb-monitor");
        mdns_instance_name_set("SBB Monitor");
    }
    http_server_start();

    if (ap_mode) {
        if (oled_ok) {
            draw_header("KEIN WLAN", false);
            draw_text(0, 20, "SSID: SBB-MONITOR");
            draw_text(0, 32, "192.168.4.1");
            draw_text(0, 44, "WLAN EINRICHTEN");
            oled_flush();
        }
        led_set(cfg.ledLoadingRgb[0], cfg.ledLoadingRgb[1], cfg.ledLoadingRgb[2]);
        while (!g_cfg_dirty) {
            // Button im AP-Modus: sleepFallbackS schlafen statt fixer 30 s —
            // ein kurzer Zyklus würde sonst nur AP→Sleep→AP im Minutentakt kosten.
            if (button_pressed())
                go_to_sleep((uint64_t)cfg.sleepFallbackS * 1000000ULL);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // Kurze Pause damit die HTTP-Response noch ausgeliefert wird
        vTaskDelay(pdMS_TO_TICKS(500));
        // Neue WLAN-Credentials gespeichert → neu starten
        esp_restart();
    }

    TickType_t active_start = xTaskGetTickCount();
    TickType_t active_end;
    if (woken_by_button) {
        active_end = active_start + minutes_to_ticks((uint32_t)button_active_min);
    } else {
        int rem = active_rem_min;
        if (rem < 1) rem = 1;
        active_end = active_start + minutes_to_ticks((uint32_t)rem);
    }
    publish_active_end(active_end);

    // Dest-Filter: Pointer-Array aus cfg.destFilters[][] bauen
    const char *filter_ptrs[MAX_DEST_FILTERS] = {0};
    for (int i = 0; i < cfg.destFilterCount && i < MAX_DEST_FILTERS; i++)
        filter_ptrs[i] = cfg.destFilters[i];

    // static: aus dem Stack raus (verhindert Stack-Overflow im Main-Task)
    static SbbDeparture deps[DEP_COUNT];
    static SbbDeparture last_deps[DEP_COUNT];
    memset(deps, 0, sizeof(deps));
    memset(last_deps, 0, sizeof(last_deps));
    bool has_cached = false;
    time_t cached_time = 0;

    bool inverted = false;
    bool force_sleep = false;
    // sleepEnabled=false: Schleife läuft unbegrenzt (bis Button-Sleep oder Sleep wird aktiviert).
    // Gilt auch im Zeitfenster — sonst wäre das Gerät bei "Schlaf aus" nach dem
    // Fensterende trotzdem für sleepAfterS eingeschlafen.
    bool run_forever = !cfg.sleepEnabled && !woken_by_button;
    g_run_forever = run_forever;
    TickType_t next_invert = xTaskGetTickCount() +
        minutes_to_ticks((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440));

    while (!force_sleep && (run_forever || xTaskGetTickCount() < active_end)) {
        if (g_cfg_dirty) {
            bool was_forever = run_forever;
            esp_err_t load_err = nvs_config_load(&cfg);
            g_cfg_dirty = false;
            if (load_err != ESP_OK) {
                ESP_LOGE(TAG, "Config Reload fehlgeschlagen: %s", esp_err_to_name(load_err));
            } else {
                ESP_LOGI(TAG, "Config neu geladen (Web-Panel)");
            }
            run_forever = !cfg.sleepEnabled && !woken_by_button;
            g_run_forever = run_forever;
            if (was_forever && !run_forever) {
                // Sleep wurde aktiviert → frischen buttonActiveMin-Timer starten
                active_start = xTaskGetTickCount();
                active_end   = active_start + minutes_to_ticks((uint32_t)cfg.buttonActiveMin);
                publish_active_end(active_end);
                ESP_LOGI(TAG, "Sleep aktiviert → Timer %d Min", cfg.buttonActiveMin);
            }
            // Invert-Intervall neu ansetzen; bei 0 (aus) sofort zurückschalten,
            // sonst bliebe das Display bis zum Schlafen invertiert.
            if (cfg.oledInvertMin <= 0 && inverted) {
                oled_cmd(0xA6);
                inverted = false;
            }
            next_invert = xTaskGetTickCount() +
                minutes_to_ticks((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440));
            for (int i = 0; i < MAX_DEST_FILTERS; i++)
                filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
            // Die Fenster koennen sich gerade geaendert haben — sonst stuende
            // die Warnung nur im Log des Kaltstarts, also genau dann nicht,
            // wenn der Nutzer sie gerade verstellt hat.
            check_window_overlaps();
        }
        publish_in_window();
        wifi_reconnect_interruptible(&force_sleep);
        if (force_sleep) break;

        bool success = false;
        for (int attempt = 0; attempt < cfg.apiRetryCount && !success; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "API Retry %d/%d", attempt + 1, cfg.apiRetryCount);
                vTaskDelay(seconds_to_ticks((uint32_t)cfg.apiRetryDelayS));
            }
            success = sbb_get_departures(cfg.station, deps, filter_ptrs, cfg.destFilterCount);
        }

        bool show_stale = false;
        if (success) {
            memcpy(last_deps, deps, sizeof(deps));
            has_cached = true;
            time(&cached_time);
            memcpy(g_last_deps, deps, sizeof(g_last_deps));
            g_last_deps_time = cached_time;
        } else {
            time_t n; time(&n);
            if (has_cached && (n - cached_time) < cfg.staleMaxMin * 60) {
                show_stale = true;
            }
        }

        if (success) {
            led_show_worst_status(deps);
        } else {
            led_set(cfg.ledCancelledRgb[0], cfg.ledCancelledRgb[1], cfg.ledCancelledRgb[2]);
        }

        // Display
        if (success || show_stale) {
            display_departures(success ? deps : last_deps, show_stale);
        } else {
            display_error();
        }
        redraw_bar(run_forever, active_start, active_end);

        // Adaptiver Refresh
        int refresh_sec;
        if (success) {
            int min_to_next = -1;
            time_t n; struct tm nt; time(&n); localtime_r(&n, &nt);
            int cur_m = nt.tm_hour * 60 + nt.tm_min;
            for (int i = 0; i < DEP_COUNT; i++) {
                if (!deps[i].valid || deps[i].cancelled) continue;
                int h, m;
                if (sscanf(deps[i].time, "%d:%d", &h, &m) == 2) {
                    // Wrap-fähig: Zug nach Mitternacht zählt als zukünftig (≤ 12 h)
                    int diff = ((h * 60 + m + deps[i].delay) - cur_m + 24 * 60) % (24 * 60);
                    if (diff <= 12 * 60 && (min_to_next < 0 || diff < min_to_next))
                        min_to_next = diff;
                }
            }
            if (min_to_next < 0) {
                ESP_LOGW(TAG, "Kein Zug in Zukunft! cur_m=%d", cur_m);
                refresh_sec = cfg.refreshFarSec;
            } else if (min_to_next <= cfg.refreshNearMin) refresh_sec = cfg.refreshNearSec;
            else if (min_to_next <= cfg.refreshMidMin)    refresh_sec = cfg.refreshMidSec;
            else if (min_to_next <= cfg.refreshFarMin)    refresh_sec = cfg.refreshFarSec;
            else                                          refresh_sec = cfg.refreshVeryfarSec;
            if (min_to_next >= 0)
                ESP_LOGI(TAG, "Nächster Zug in %d Min -> Refresh %d s", min_to_next, refresh_sec);
        } else {
            refresh_sec = cfg.refreshMidSec;
        }

        // Wartephase mit LED-Blink, OLED-Invert und Uhr-Update
        TickType_t wait_end = xTaskGetTickCount() + seconds_to_ticks((uint32_t)refresh_sec);
        bool blink_on = true;
        TickType_t next_toggle = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
        TickType_t next_clock = xTaskGetTickCount() + pdMS_TO_TICKS(30 * 1000);
        TickType_t next_bar = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (xTaskGetTickCount() < wait_end && (run_forever || xTaskGetTickCount() < active_end)) {
            TickType_t t = xTaskGetTickCount();
            if (!success && cfg.ledErrorBlinkMs > 0 && t >= next_toggle) {
                blink_on = !blink_on;
                if (blink_on) led_set(cfg.ledCancelledRgb[0], cfg.ledCancelledRgb[1], cfg.ledCancelledRgb[2]);
                else          led_set(0, 0, 0);
                next_toggle = t + pdMS_TO_TICKS((uint32_t)cfg.ledErrorBlinkMs);
            }
            if (cfg.oledInvertMin > 0 && t >= next_invert) {
                inverted = !inverted;
                oled_cmd(inverted ? 0xA7 : 0xA6);
                next_invert = t + minutes_to_ticks((uint32_t)cfg.oledInvertMin);
            }
            if (t >= next_clock) {
                // Nur wiederholen, was die äußere Schleife entschieden hat:
                // sonst überschrieb der 30-s-Redraw die Fehlerseite mit alten
                // Daten — ohne "!"-Marker, also scheinbar aktuell.
                if (has_cached && (success || show_stale))
                    display_departures(last_deps, show_stale);
                else if (!success)
                    display_error();
                redraw_bar(run_forever, active_start, active_end);
                // Auch hier nachfuehren: zwischen zwei API-Zyklen liegen bis zu
                // refreshVeryfarSec, so lange soll das Panel nicht veralten.
                publish_in_window();
                next_clock = t + pdMS_TO_TICKS(30 * 1000);
            }
            if (t >= next_bar) {
                redraw_bar(run_forever, active_start, active_end);
                next_bar = t + pdMS_TO_TICKS(1000);
            }
            // Button während aktivem Betrieb → sofort schlafen
            if (button_pressed()) {
                ESP_LOGI(TAG, "Button gedrückt → Schlaf");
                wait_button_release(5000);   // sonst weckt derselbe Druck sofort wieder
                force_sleep = true;
                break;
            }
            // Config-Änderung → äußere Schleife sofort reagieren lassen
            if (g_cfg_dirty) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (force_sleep) break;
    }

    if (inverted) oled_cmd(0xA6);
    http_server_stop();
    show_sleep_info((cfg.sleepAfterS + 59) / 60);
    go_to_sleep((uint64_t)cfg.sleepAfterS * 1000000ULL);
}
