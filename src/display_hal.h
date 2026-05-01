/**
 * display_hal.h — Display + Touch + Backlight + Battery abstraction
 */

#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include <Arduino.h>
#include <lvgl.h>

/// Latch battery power ASAP — call before anything else in setup().
void display_power_latch();

/// Initialize display hardware, touch, CH32V003 IO expander, LVGL drivers.
void display_init();

/// Must be called from loop() — handles LVGL tick + screen timeout.
void display_loop();

/// Get the LVGL display object.
lv_disp_t *display_get();

// ── Backlight / screen dimmer ─────────────────────────
void     display_set_brightness(uint8_t percent);   // 0-100 (0 = off)
uint8_t  display_get_brightness();
bool     display_is_backlight_on();
void     display_set_screen_timeout(uint16_t secs);  // 0 = disabled
uint16_t display_get_screen_timeout();

// ── Battery / power ───────────────────────────────────
uint16_t display_get_battery_raw();     // 10-bit ADC raw value
uint16_t display_get_battery_mv();      // approximate millivolts
uint8_t  display_get_battery_pct();     // approximate 0-100 %
bool     display_is_charging();         // charger / USB connected

#endif // DISPLAY_HAL_H
