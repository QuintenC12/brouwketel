/*
 * board_crowpanel7.cpp - Elecrow CrowPanel 7.0" ESP32-S3 (DIS08070H)
 *
 *   Scherm : 800x480 RGB-paneel (EK9716BD3 + EK73002ACGB), 16-bit parallel
 *   Touch  : GT911, I2C op GPIO19 (SDA) / GPIO20 (SCL)
 *   Licht  : GPIO2
 *
 * Pinnen en timing komen uit Elecrow's officiële PlatformIO-voorbeeld
 * (github.com/Elecrow-RD/CrowPanel-7.0-HMI-ESP32-Display-800x480, map V3.0).
 * Werkt voor hardwareversie V2.0 en V3.0: V3.0 heeft een extra PCA9557-chip
 * die de touch-reset regelt; op V2.0 doet die stap gewoon niets.
 *
 * Het scherm gebruikt de RGB-driver van ESP-IDF (esp_lcd) met twee
 * framebuffers in PSRAM. LVGL tekent rechtstreeks in die buffers en alleen
 * wat veranderd is wordt opnieuw getekend (DIRECT-modus).
 */
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include "board.h"

#define SCHERM_B 800
#define SCHERM_H 480

#define PIN_BACKLIGHT 2
#define PIN_I2C_SDA   19
#define PIN_I2C_SCL   20

/* Zet op 1 als de touch gespiegeld reageert (links/rechts of boven/onder omgekeerd) */
#define TOUCH_SPIEGEL_X 0
#define TOUCH_SPIEGEL_Y 0

/* ======================================================================
 * Scherm (RGB-paneel)
 * ====================================================================== */

static esp_lcd_panel_handle_t paneel = nullptr;
static volatile uint32_t vsync_teller = 0;

static bool IRAM_ATTR bij_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
    vsync_teller = vsync_teller + 1;
    return false;
}

static bool scherm_init(void **fb0, void **fb1) {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz = 24000000;
    cfg.timings.h_res = SCHERM_B;
    cfg.timings.v_res = SCHERM_H;
    cfg.timings.hsync_pulse_width = 48;
    cfg.timings.hsync_back_porch = 40;
    cfg.timings.hsync_front_porch = 40;
    cfg.timings.vsync_pulse_width = 31;
    cfg.timings.vsync_back_porch = 13;
    cfg.timings.vsync_front_porch = 1;
    cfg.timings.flags.hsync_idle_low = 0;
    cfg.timings.flags.vsync_idle_low = 0;
    cfg.timings.flags.de_idle_high = 0;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.timings.flags.pclk_idle_high = 0;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 2;
    cfg.bounce_buffer_size_px = SCHERM_B * 10;
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = 39;
    cfg.vsync_gpio_num = 40;
    cfg.de_gpio_num = 41;
    cfg.pclk_gpio_num = 0;
    cfg.disp_gpio_num = -1;
    // B0..B4, G0..G5, R0..R4
    const int data[16] = { 15, 7, 6, 5, 4,   9, 46, 3, 8, 16, 1,   14, 21, 47, 48, 45 };
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
    cfg.flags.fb_in_psram = 1;

    esp_err_t r = esp_lcd_new_rgb_panel(&cfg, &paneel);
    if (r == ESP_OK) {
        esp_lcd_rgb_panel_event_callbacks_t cb = {};
        cb.on_vsync = bij_vsync;
        r = esp_lcd_rgb_panel_register_event_callbacks(paneel, &cb, nullptr);
    }
    if (r == ESP_OK) r = esp_lcd_panel_reset(paneel);
    if (r == ESP_OK) r = esp_lcd_panel_init(paneel);
    if (r == ESP_OK) r = esp_lcd_rgb_panel_get_frame_buffer(paneel, 2, fb0, fb1);
    if (r != ESP_OK) {
        Serial.printf("[scherm] init mislukt: %s\n", esp_err_to_name(r));
        return false;
    }
    memset(*fb0, 0, SCHERM_B * SCHERM_H * 2);
    memset(*fb1, 0, SCHERM_B * SCHERM_H * 2);
    return true;
}

/* LVGL heeft (een deel van) een framebuffer getekend. Na het laatste deel
 * schakelen we het paneel over naar die buffer, bij de volgende vsync. */
static void scherm_flush(lv_display_t *disp, const lv_area_t *, uint8_t *px) {
    if (lv_display_flush_is_last(disp)) {
        uint32_t teller = vsync_teller;
        esp_lcd_panel_draw_bitmap(paneel, 0, 0, SCHERM_B, SCHERM_H, px);
        uint32_t start = millis();
        while (vsync_teller == teller && millis() - start < 50) taskYIELD();
    }
    lv_display_flush_ready(disp);
}

/* ======================================================================
 * Touch (GT911)
 * ====================================================================== */

static uint8_t gt911_adres = 0x5D;
static int touch_x = 0, touch_y = 0;
static bool touch_ingedrukt = false;

/* V3.0: PCA9557 I/O-expander (adres 0x18) regelt reset en INT van de touch */
static void pca9557_schrijf(uint8_t reg, uint8_t waarde) {
    Wire.beginTransmission(0x18);
    Wire.write(reg);
    Wire.write(waarde);
    Wire.endTransmission();
}

static void touch_reset_v3() {
    pca9557_schrijf(0x03, 0xFC);   // IO0 en IO1 als uitgang
    pca9557_schrijf(0x01, 0x00);   // beide laag
    delay(20);
    pca9557_schrijf(0x01, 0x01);   // IO0 (reset) hoog, INT laag -> adres 0x5D
    delay(100);
    pca9557_schrijf(0x03, 0xFE);   // IO1 (INT) terug als ingang
}

static bool gt911_lees(uint16_t reg, uint8_t *buf, size_t n) {
    Wire.beginTransmission(gt911_adres);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(gt911_adres, (uint8_t)n) != n) return false;
    for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

static void gt911_wis_status() {
    Wire.beginTransmission(gt911_adres);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0x00);
    Wire.endTransmission();
}

static bool touch_init() {
    touch_reset_v3();
    const uint8_t adressen[2] = { 0x5D, 0x14 };   // GT911 kan op beide adressen zitten
    for (uint8_t a : adressen) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            gt911_adres = a;
            Serial.printf("[touch] GT911 gevonden op 0x%02X\n", a);
            return true;
        }
    }
    Serial.println("[touch] GT911 niet gevonden");
    return false;
}

static void touch_lees(lv_indev_t *, lv_indev_data_t *data) {
    uint8_t status = 0;
    if (gt911_lees(0x814E, &status, 1) && (status & 0x80)) {
        uint8_t aantal = status & 0x0F;
        touch_ingedrukt = aantal > 0 && aantal <= 5;
        if (touch_ingedrukt) {
            uint8_t p[7];
            if (gt911_lees(0x814F, p, sizeof p)) {
                int x = p[1] | (p[2] << 8);
                int y = p[3] | (p[4] << 8);
                if (TOUCH_SPIEGEL_X) x = SCHERM_B - 1 - x;
                if (TOUCH_SPIEGEL_Y) y = SCHERM_H - 1 - y;
                touch_x = constrain(x, 0, SCHERM_B - 1);
                touch_y = constrain(y, 0, SCHERM_H - 1);
            }
        }
        gt911_wis_status();
    }
    data->state = touch_ingedrukt ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touch_x;
    data->point.y = touch_y;
}

/* ======================================================================
 * Alles samen
 * ====================================================================== */

bool board_init() {
    pinMode(PIN_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_BACKLIGHT, LOW);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);

    void *fb0 = nullptr, *fb1 = nullptr;
    if (!scherm_init(&fb0, &fb1)) return false;

    lv_display_t *disp = lv_display_create(SCHERM_B, SCHERM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb0, fb1, SCHERM_B * SCHERM_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, scherm_flush);

    touch_init();
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_lees);

    digitalWrite(PIN_BACKLIGHT, HIGH);
    Serial.printf("[scherm] OK - vrij PSRAM: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    return true;
}
