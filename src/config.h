/**
 * config.h — Central configuration for the Smart Display
 * ══════════════════════════════════════════════════════
 * All pin definitions, API keys, and tunables in one place.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ─── Screen ────────────────────────────────────────────
#define SCREEN_W 480
#define SCREEN_H 480

// ─── ST7701 SPI init bus ───────────────────────────────
#define TFT_SPI_CS   42
#define TFT_SPI_SCK   2
#define TFT_SPI_MOSI  1

// ─── RGB parallel data lines ───────────────────────────
#define TFT_DE    40
#define TFT_VSYNC 39
#define TFT_HSYNC 38
#define TFT_PCLK  41

#define TFT_R0 46
#define TFT_R1  3
#define TFT_R2  8
#define TFT_R3 18
#define TFT_R4 17

#define TFT_G0 14
#define TFT_G1 13
#define TFT_G2 12
#define TFT_G3 11
#define TFT_G4 10
#define TFT_G5  9

#define TFT_B0  5
#define TFT_B1 45
#define TFT_B2 48
#define TFT_B3 47
#define TFT_B4 21

// ─── Touch (GT911) ─────────────────────────────────────
#define TOUCH_SDA 15
#define TOUCH_SCL  7

// ─── IO Expander (TCA9554) ─────────────────────────────
#define IO_EXPANDER_ADDR 0x24

// ─── Buzzer ────────────────────────────────────────────
#define BUZZER_PIN 16

// ─── WiFi AP for provisioning ──────────────────────────
#define AP_SSID     "SmartDisplay"
#define AP_PASSWORD "setup1234"
#define AP_CHANNEL  1

// ─── Location (used by weather) ────────────────────────
#define MY_LAT       12.97f
#define MY_LON       77.59f
#define MY_CITY      "Bangalore"
#define MY_TIMEZONE  "Asia/Kolkata"

// ─── Weather (Open-Meteo — free, no key needed) ───────
#define WEATHER_POLL_MS (10 * 60 * 1000) // 10 minutes

// ─── Home Assistant ───────────────────────────────────
#define HA_URL   "http://homeassistant.local:8123"
#define HA_TOKEN ""  // Auto-injected by HACS integration

// ─── LVGL buffer ──────────────────────────────────────
#define LV_BUF_LINES  60               // lines per flush buffer

#endif // CONFIG_H
