/**
 * display_hal.cpp — Display + Touch + Backlight + Battery
 * ══════════════════════════════════════════════════════
 * Initialises ST7701 via Arduino_GFX, GT911 touch via SensorLib,
 * CH32V003 IO expander (V4), and registers both with LVGL 8.3.
 *
 * CH32V003 register map (I2C 0x24):
 *   0x02 = OUTPUT   — GPIO output state
 *   0x03 = DIRECTION — pin direction (1=output on CH32V003)
 *   0x04 = INPUT    — GPIO input state (read-only)
 *   0x05 = PWM      — backlight PWM duty (0=bright, 247=off)
 *   0x06 = ADC      — battery voltage (2 bytes, LE, 10-bit)
 */

#include "display_hal.h"
#include "config.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "TouchDrvGT911.hpp"

// ── CH32V003 IO Expander registers ─────────────────────
#define CH32_REG_OUTPUT    0x02
#define CH32_REG_DIRECTION 0x03
#define CH32_REG_INPUT     0x04
#define CH32_REG_PWM       0x05
#define CH32_REG_ADC       0x06

// PWM brightness (inverted: lower value = brighter)
#define PWM_BRIGHTEST   30     // 100% brightness
#define PWM_DIMMEST    240     // ~0% visible
#define PWM_OFF        247     // backlight off

// ── GFX objects ────────────────────────────────────────
static Arduino_DataBus *bus = nullptr;
static Arduino_ESP32RGBPanel *rgbpanel = nullptr;
static Arduino_RGB_Display *gfx = nullptr;

// ── Touch ──────────────────────────────────────────────
static TouchDrvGT911 touchDrv;
static bool touchReady = false;

// ── LVGL buffers ───────────────────────────────────────
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

// ── Backlight / screen timeout state ───────────────────
static uint8_t  _brightness       = 80;      // current brightness 0-100
static bool     _backlightOn      = true;
static uint16_t _screenTimeout    = 30;      // seconds (0 = disabled)
static uint32_t _lastTouchMs      = 0;
static bool     _screenAsleep     = false;   // true → screen is off
static bool     _swallowUntilRelease = false; // eat touches until finger lifts

// ── CH32V003 I2C helpers ──────────────────────────────
static bool ch32_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t ch32_read8(uint8_t reg) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)IO_EXPANDER_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

// Convert brightness 0-100 to CH32V003 PWM duty (inverted)
static uint8_t pct_to_duty(uint8_t pct) {
    if (pct >= 100) return PWM_BRIGHTEST;
    if (pct == 0)   return PWM_OFF;
    uint16_t range = PWM_DIMMEST - PWM_BRIGHTEST;
    return PWM_DIMMEST - (uint8_t)((uint16_t)pct * range / 100);
}

// ── LVGL display flush callback ────────────────────────
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                               (uint16_t *)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1,
                             (uint16_t *)&color_p->full, w, h);
#endif

    lv_disp_flush_ready(drv);
}

// ── LVGL touch read callback ───────────────────────────
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (!touchReady) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    int16_t x, y;
    if (touchDrv.getPoint(&x, &y)) {
        _lastTouchMs = millis();

        // If screen was asleep, wake it but swallow until finger lifts
        if (_screenAsleep) {
            _screenAsleep = false;
            _swallowUntilRelease = true;
            display_set_brightness(_brightness);
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        // Still swallowing touches until finger fully lifts
        if (_swallowUntilRelease) {
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        _swallowUntilRelease = false;  // finger lifted, next touch is real
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── CH32V003 IO expander init ──────────────────────────
// GPIO bit map (V4 board):
//   bit 0 = EXIO0  charger status (INPUT)
//   bit 1 = EXIO1  LCD reset      (OUTPUT, active-low)
//   bit 2 = EXIO2  Touch reset    (OUTPUT, active-low)
//   bit 3 = EXIO3  SD_CS          (OUTPUT)
//   bit 4 = EXIO4  USB select     (OUTPUT)
//   bit 5 = EXIO5  BOOST_EN       (OUTPUT, backlight boost)
//   bit 6 = EXIO6  BEE_EN         (OUTPUT, buzzer)

#define EXIO_LCD_RST   (1 << 1)
#define EXIO_TP_RST    (1 << 2)
#define EXIO_BOOST_EN  (1 << 5)

static void ch32v003_init() {
    // Direction: bits 1-5 as outputs, bit 0 input (charger), bit 6 input (buzzer via ESP GPIO)
    ch32_write(CH32_REG_DIRECTION, 0x3E);

    // 1. Pull all outputs LOW (reset everything)
    ch32_write(CH32_REG_OUTPUT, 0x00);
    delay(20);

    // 2. Release resets + enable backlight boost — all outputs HIGH except buzzer
    ch32_write(CH32_REG_OUTPUT, 0xBF);
    delay(50);

    // Set initial brightness
    ch32_write(CH32_REG_PWM, pct_to_duty(_brightness));
    Serial.println("[DISP] CH32V003 init — LCD reset pulsed, boost enabled");
}

// ── Public API ────────────────────────────────────────
void display_power_latch() {
    // Placeholder — power latch not yet implemented
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);
}

void display_init() {
    // I2C for touch + IO expander (no-op if already called by power_latch)
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);

    ch32v003_init();

    // Buzzer pin — set LOW (silent)
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // GFX display
    bus = new Arduino_SWSPI(
        GFX_NOT_DEFINED, TFT_SPI_CS,
        TFT_SPI_SCK, TFT_SPI_MOSI, GFX_NOT_DEFINED);

    rgbpanel = new Arduino_ESP32RGBPanel(
        TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
        TFT_R0, TFT_R1, TFT_R2, TFT_R3, TFT_R4,
        TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
        TFT_B0, TFT_B1, TFT_B2, TFT_B3, TFT_B4,
        1, 10, 8, 50,
        1, 10, 8, 20);

    gfx = new Arduino_RGB_Display(
        SCREEN_W, SCREEN_H, rgbpanel,
        2 /* rotation */, true /* auto_flush */,
        bus, GFX_NOT_DEFINED,
        st7701_type1_init_operations,
        sizeof(st7701_type1_init_operations));

    // ST7701 SPI init — retry if needed (LCD must be out of reset)
    bool gfxOk = false;
    for (int attempt = 0; attempt < 3 && !gfxOk; attempt++) {
        if (attempt > 0) {
            Serial.printf("[DISP] GFX init retry %d — re-pulsing LCD reset\n", attempt);
            // Re-pulse LCD reset via CH32V003
            ch32_write(CH32_REG_OUTPUT, 0xFF & ~EXIO_LCD_RST);  // RST LOW
            delay(20);
            ch32_write(CH32_REG_OUTPUT, 0xFF);                   // RST HIGH
            delay(50);
        }
        gfxOk = gfx->begin();
    }
    if (!gfxOk) {
        Serial.println("[DISP] GFX init FAILED after 3 attempts");
    } else {
        Serial.println("[DISP] GFX init OK");
    }
    gfx->fillScreen(RGB565_BLACK);

    // Touch (no RST/IRQ pins wired on this board)
    touchDrv.setPins(-1, -1);  // -1 = not connected
    touchReady = touchDrv.begin(Wire, GT911_SLAVE_ADDRESS_L,
                                 TOUCH_SDA, TOUCH_SCL);
    if (!touchReady) {
        Serial.println("[DISP] Touch addr 0x5D failed — trying 0x14");
        touchReady = touchDrv.begin(Wire, GT911_SLAVE_ADDRESS_H,
                                     TOUCH_SDA, TOUCH_SCL);
    }
    if (touchReady) {
        touchDrv.setMaxCoordinates(SCREEN_W, SCREEN_H);
        touchDrv.setMirrorXY(true, true);   // match display rotation=2 (180°)
        Serial.println("[DISP] Touch init OK");
    } else {
        Serial.println("[DISP] Touch init FAILED");
    }

    // LVGL init
    lv_init();

    size_t bufSize = SCREEN_W * LV_BUF_LINES;
    buf1 = (lv_color_t *)ps_malloc(bufSize * sizeof(lv_color_t));
    buf2 = (lv_color_t *)ps_malloc(bufSize * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, bufSize);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    // Apply dark theme
    lv_theme_t *th = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_CYAN),
        true,  // dark mode
        LV_FONT_DEFAULT);
    lv_disp_set_theme(lv_disp_get_default(), th);

    Serial.println("[DISP] LVGL init OK");

    _lastTouchMs = millis();   // prevent immediate screen timeout on boot

}

void display_loop() {

    // ── While asleep: skip LVGL, poll touch directly for wake ──
    if (_screenAsleep) {
        if (touchReady) {
            int16_t x, y;
            if (touchDrv.getPoint(&x, &y)) {
                Serial.println("[DISP] Touch wake");
                ch32_write(CH32_REG_PWM, pct_to_duty(_brightness));
                _backlightOn  = true;
                _screenAsleep = false;
                _swallowUntilRelease = true;
                _lastTouchMs  = millis();
                // Force LVGL to redraw entire screen over the black fill
                lv_obj_invalidate(lv_scr_act());
            }
        }
        return;   // nothing else to do while asleep
    }

    lv_timer_handler();

    // ── Screen timeout check ──
    if (_screenTimeout > 0 && _backlightOn) {
        uint32_t elapsed = millis() - _lastTouchMs;
        if (elapsed > (uint32_t)_screenTimeout * 1000) {
            ch32_write(CH32_REG_PWM, PWM_OFF);
            gfx->fillScreen(RGB565_BLACK);     // black out LCD panel
            _backlightOn  = false;
            _screenAsleep = true;
            Serial.printf("[DISP] Screen sleep (%us)\n", _screenTimeout);
        }
    }
}

lv_disp_t *display_get() {
    return lv_disp_get_default();
}

// ═══════════════════════════════════════════════════════
//  Backlight / brightness
// ═══════════════════════════════════════════════════════

void display_set_brightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    _brightness = pct;
    if (pct == 0) {
        ch32_write(CH32_REG_PWM, PWM_OFF);
        _backlightOn = false;
    } else {
        ch32_write(CH32_REG_PWM, pct_to_duty(pct));
        _backlightOn  = true;
        _screenAsleep = false;
    }
}

uint8_t display_get_brightness() { return _brightness; }

bool display_is_backlight_on() { return _backlightOn; }

void display_set_screen_timeout(uint16_t secs) {
    _screenTimeout = secs;
    _lastTouchMs   = millis();   // reset timer on config change
    Serial.printf("[DISP] Screen timeout set to %u s\n", secs);
}

uint16_t display_get_screen_timeout() { return _screenTimeout; }

// ═══════════════════════════════════════════════════════
//  Battery / power (CH32V003 ADC + charger input)
// ═══════════════════════════════════════════════════════

static uint16_t _read_adc_once() {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(CH32_REG_ADC);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)IO_EXPANDER_ADDR, (uint8_t)2);
    if (Wire.available() >= 2) {
        uint8_t lo = Wire.read();
        uint8_t hi = Wire.read();
        return (uint16_t)(lo | (hi << 8)) & 0x3FF;
    }
    return 0;
}

uint16_t display_get_battery_raw() {
    // Average 8 samples to reduce ADC noise
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += _read_adc_once();
        delayMicroseconds(200);
    }
    return (uint16_t)(sum / 8);
}

uint16_t display_get_battery_mv() {
    // CH32V003 10-bit ADC, 3.3V ref, ~3:1 voltage divider
    // Calibrated: raw 422 = 4000 mV battery (measured with multimeter)
    uint16_t raw = display_get_battery_raw();
    return (uint16_t)((uint32_t)raw * 9700 / 1024);
}

uint8_t display_get_battery_pct() {
    uint16_t mv = display_get_battery_mv();
    if (mv >= 4200) return 100;
    if (mv <= 3000) return 0;
    // Linear approximation: 3000–4200 mV → 0–100%
    return (uint8_t)((uint32_t)(mv - 3000) * 100 / 1200);
}

bool display_is_charging() {
    uint8_t inputs = ch32_read8(CH32_REG_INPUT);
    return !(inputs & 0x01);   // active-low: bit0=0 → charging
}
