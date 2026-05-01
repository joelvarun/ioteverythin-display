/*
 * Waveshare ESP32-S3-Touch-LCD-4 — Home Assistant Panel
 * ══════════════════════════════════════════════════════
 * Full-screen HA control:
 *   Tab 1: Lights    — toggle all lights & switches
 *   Tab 2: Climate   — indoor temp/humidity, AC1 & AC2 control
 *   Tab 3: Music     — media player (BRAVIA TV)
 */

#include <Arduino.h>
#include <lvgl.h>

#include "config.h"
#include "display_hal.h"
#include "wifi_mgr.h"
#include "ha.h"
#include "config_server.h"
#include "logo_img.h"

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

    // Latch battery power IMMEDIATELY — must happen before any delay
    display_power_latch();

    delay(1500);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  Home Assistant Panel");
    Serial.println("  Waveshare ESP32-S3-Touch-LCD-4");
    Serial.println("========================================");

    // 0. Pre-read ALL NVS config into RAM — MUST happen before display_init()
    //    to avoid "Cache disabled" crash (RGB LCD DMA vs SPI flash conflict)
    ha_preload_config();

    // 1. Display + touch + LVGL
    display_init();

    // ── Splash screen with logo ──
    lv_obj_t *splash = lv_scr_act();
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x000000), 0);

    lv_obj_t *logo = lv_img_create(splash);
    lv_img_set_src(logo, &logo_dsc);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *splash_lbl = lv_label_create(splash);
    lv_label_set_text(splash_lbl, "IOT EVERYTHIN");
    lv_obj_set_style_text_font(splash_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(splash_lbl, lv_color_hex(0xD4A017), 0);
    lv_obj_align(splash_lbl, LV_ALIGN_CENTER, 0, 120);

    lv_timer_handler();
    delay(100);
    lv_timer_handler();

    // 2. WiFi (splash stays visible during connection)
    wifi_mgr_init();

    // Remove splash elements
    lv_obj_clean(splash);
    lv_timer_handler();

    // 3. Start config server (if connected)
    if (wifi_mgr_connected()) {
        config_server_start();
    }

    // 4. Restore display settings from pre-loaded cache (no NVS reads!)
    if (wifi_mgr_connected()) {
        display_set_brightness(ha_get_cached_brightness());
        display_set_screen_timeout(ha_get_cached_timeout());
    } else {
        display_set_screen_timeout(0);  // no timeout in AP mode
        display_set_brightness(100);     // full brightness for setup
    }

    // 5. Build HA UI (if connected) or WiFi QR screen
    if (wifi_mgr_connected()) {
        ha_create_ui();
    } else {
        // Show WiFi provisioning tab until connected
        lv_obj_t *tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 42);
        lv_obj_t *tab = lv_tabview_add_tab(tabview, LV_SYMBOL_WIFI " WiFi Setup");
        wifi_mgr_create_tab(tab);
    }

    Serial.println("[OK] Setup complete");
}

// ═══════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════

void loop() {
    display_loop();       // LVGL tick
    wifi_mgr_loop();      // handle AP portal / reconnects
    config_server_loop(); // handle config HTTP requests
    ha_loop();            // poll HA API
    delay(5);
}
