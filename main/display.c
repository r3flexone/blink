#include "display.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtol() fuer die I2C-Adresspruefung
#include <time.h>

static const char *TAG = "display";

#define OLED_WIDTH  128
#define OLED_HEIGHT  64
// Der 5x7-Font belegt inkl. Spalte Abstand 6 px pro Zeichen.
#define OLED_COLS   (OLED_WIDTH / 6)

static const blink_config_t *cfg = NULL;

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
void display_init(const blink_config_t *c) {
    cfg = c;
    // Tippfehler im Panel ("3G", "abc") darf nicht in einer unbrauchbaren
    // I2C-Adresse enden — ausserhalb des gültigen 7-Bit-Bereichs: Default.
    int oled_addr = (int)strtol(cfg->oledAddr, NULL, 16);
    if (oled_addr < 0x08 || oled_addr > 0x77) {
        ESP_LOGW(TAG, "OLED-Adresse '%s' ungueltig, nutze 0x3C", cfg->oledAddr);
        oled_addr = 0x3C;
    }
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = cfg->sclGpio, .sda_io_num = cfg->sdaGpio,
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

void display_departures(const SbbDeparture deps[DEP_COUNT], bool stale) {
    draw_header(cfg->station, stale);
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

void display_error(void) {
    draw_header(cfg->station, false);
    draw_text(0, 20, "API FEHLER");
    draw_text(0, 32, "PRUEFE NETZ...");
    oled_flush();
}

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

// Bei run_forever bleibt der Balken voll (total=1, remaining=1).
void display_countdown_bar(bool run_forever, TickType_t active_start, TickType_t active_end) {
    TickType_t t = xTaskGetTickCount();
    draw_countdown_bar(run_forever ? t : active_start, run_forever ? t + 1 : active_end);
    flush_page7();
}

static const char *WEEKDAY_ABBR[7] = {"SO","MO","DI","MI","DO","FR","SA"};

void display_sleep_info(int sleep_min) {
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


bool display_ready(void) { return oled_ok; }

void display_on(void)  { oled_cmd(0xAF); }
void display_off(void) { oled_cmd(0xAE); }

void display_clear(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
    oled_flush();
}

void display_set_inverted(bool on) { oled_cmd(on ? 0xA7 : 0xA6); }

// Ersetzt die frueher an drei Stellen in app_main ausgeschriebene Folge aus
// draw_header() + draw_text() + oled_flush().
void display_message(const char *title, const char *l1, const char *l2, const char *l3) {
    if (!oled_ok) return;
    draw_header(title, false);
    if (l1) draw_text(0, 20, l1);
    if (l2) draw_text(0, 32, l2);
    if (l3) draw_text(0, 44, l3);
    oled_flush();
}
