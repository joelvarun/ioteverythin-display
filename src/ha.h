/**
 * ha.h — Home Assistant full-screen control panel
 */

#ifndef HA_H
#define HA_H

#include <Arduino.h>
#include <lvgl.h>

/// Pre-read ALL NVS config into RAM cache.  MUST be called before display_init()
/// to avoid "Cache disabled but cached memory region accessed" crash (RGB LCD
/// DMA ISR conflicts with SPI flash reads).
void ha_preload_config();

/// Update the RAM cache with new config (called by config_server after POST).
/// Avoids NVS reads while the RGB panel is running.
void ha_cache_config(const char* url, const char* token,
                     const String& lights, const String& climate,
                     const String& sensors, const String& media,
                     const String& persons, const String& tab3);

/// Build the full HA UI (3 tabs: Lights, Climate, Sensors).
void ha_create_ui();

/// Call from loop() — polls HA state periodically.
void ha_loop();

const char* ha_get_url();
const char* ha_get_token();

uint8_t  ha_get_cached_brightness();
uint16_t ha_get_cached_timeout();
const String& ha_get_cached_lights();
const String& ha_get_cached_climate();
const String& ha_get_cached_sensors();
const String& ha_get_cached_media();
const String& ha_get_cached_persons();
const String& ha_get_cached_tab3();
const String& ha_get_cached_url();

#endif // HA_H
