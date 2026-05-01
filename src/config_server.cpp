/**
 * config_server.cpp — HTTP config endpoint for IoT Everythin Display
 * ══════════════════════════════════════════════════════════════════
 * Endpoints:
 *   GET  /api/info     — device name, IP, firmware version, MAC
 *   GET  /api/config   — returns current stored config JSON
 *   POST /api/config   — receives config JSON from HA integration
 *
 * Config JSON schema:
 * {
 *   "ha_url": "http://homeassistant.local:8123",
 *   "ha_token": "eyJ...",
 *   "lights": [
 *     {"eid":"light.xxx", "label":"Hall\nLights", "icon":"bulb",
 *      "dimmable":true, "cat":"Living Room", "domain":"light"}
 *   ],
 *   "climate": {
 *     "temp_sensor": "sensor.xxx_temperature",
 *     "hum_sensor": "sensor.xxx_humidity",
 *     "acs": [
 *       {"eid":"climate.ac1", "label":"AC1 Panasonic", "min":16, "max":30}
 *     ]
 *   },
 *   "sensors": {
 *     "doors": [
 *       {"eid":"binary_sensor.main_door", "label":"Main\nDoor",
 *        "inverted":true}
 *     ],
 *     "motion": [
 *       {"eid":"binary_sensor.motion_xxx", "label":"Living Room"}
 *     ]
 *   }
 * }
 *
 * Config is stored across multiple Preferences keys (max ~4KB per key):
 *   "cfg" namespace:
 *     "ha_url"   — HA base URL
 *     "ha_token" — HA long-lived token
 *     "lights"   — lights JSON array string
 *     "climate"  — climate JSON object string
 *     "sensors"  — sensors JSON object string
 *     "version"  — config version counter
 */

#include "config_server.h"
#include "config.h"
#include "display_hal.h"
#include "ha.h"
#include "wifi_mgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#define FW_VERSION "1.3.0"
#define DEVICE_NAME "Touch-i"

static WebServer *cfgServer = nullptr;
static bool _configAvailable = false;
static volatile bool _needsRebuild = false;

// ── Check if config exists in Preferences ──────────────
static bool configExists() {
    Preferences p;
    p.begin("cfg", true);
    bool exists = p.isKey("ha_url");
    p.end();
    return exists;
}

// ── Handler: GET /api/info ─────────────────────────────
static void handleInfo() {
    cfgServer->sendHeader("Access-Control-Allow-Origin", "*");
    String mac = WiFi.macAddress();
    String ip = WiFi.localIP().toString();
    String json = "{";
    json += "\"name\":\"" + String(DEVICE_NAME) + "\",";
    json += "\"version\":\"" + String(FW_VERSION) + "\",";
    json += "\"ip\":\"" + ip + "\",";
    json += "\"mac\":\"" + mac + "\",";
    json += "\"screen_w\":" + String(SCREEN_W) + ",";
    json += "\"screen_h\":" + String(SCREEN_H) + ",";
    json += "\"battery_mv\":" + String(display_get_battery_mv()) + ",";
    json += "\"battery_pct\":" + String(display_get_battery_pct()) + ",";
    json += "\"charging\":" + String(display_is_charging() ? "true" : "false") + ",";
    json += "\"brightness\":" + String(display_get_brightness()) + ",";
    json += "\"backlight_on\":" + String(display_is_backlight_on() ? "true" : "false") + ",";
    json += "\"configured\":" + String(_configAvailable ? "true" : "false") + ",";
    json += "\"static_ip\":" + String(wifi_mgr_has_static_ip() ? "true" : "false");
    json += "}";
    cfgServer->send(200, "application/json", json);
}

// ── Handler: GET /api/config ─── (reads from RAM cache, no NVS!)
static void handleGetConfig() {
    cfgServer->sendHeader("Access-Control-Allow-Origin", "*");

    const String& ha_url = ha_get_cached_url();

    // Don't expose the token in GET — just indicate if it's set
    String tokenStr = String(ha_get_token());
    String json = "{";
    json += "\"ha_url\":\"" + ha_url + "\",";
    json += "\"ha_token_set\":" + String(tokenStr.length() > 0 ? "true" : "false") + ",";
    json += "\"ha_token_len\":" + String(tokenStr.length()) + ",";
    json += "\"lights\":" + ha_get_cached_lights() + ",";
    json += "\"climate\":" + ha_get_cached_climate() + ",";
    json += "\"sensors\":" + ha_get_cached_sensors() + ",";
    json += "\"media\":" + ha_get_cached_media() + ",";
    json += "\"persons\":" + ha_get_cached_persons() + ",";
    json += "\"tab3\":\"" + ha_get_cached_tab3() + "\",";
    json += "\"display\":{\"brightness\":" + String(display_get_brightness())
          + ",\"timeout\":" + String(display_get_screen_timeout()) + "}";
    json += "}";
    cfgServer->send(200, "application/json", json);
}

// ── Handler: POST /api/config ──────────────────────────
static void handlePostConfig() {
    cfgServer->sendHeader("Access-Control-Allow-Origin", "*");

    if (!cfgServer->hasArg("plain")) {
        cfgServer->send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    String body = cfgServer->arg("plain");
    if (body.length() > 16000) {
        cfgServer->send(413, "application/json", "{\"error\":\"Config too large\"}");
        return;
    }

    // Parse JSON to validate
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        cfgServer->send(400, "application/json",
            String("{\"error\":\"Invalid JSON: ") + err.c_str() + "\"}");
        return;
    }

    // Extract and store sections — yield between each NVS write
    // to prevent task WDT reset (flash writes block the CPU)
    Preferences p;

    p.begin("cfg", false);
    if (doc["ha_url"].is<const char*>()) {
        p.putString("ha_url", doc["ha_url"].as<String>());
    }
    if (doc["ha_token"].is<const char*>()) {
        p.putString("ha_token", doc["ha_token"].as<String>());
    }
    p.end();
    yield(); esp_task_wdt_reset();

    p.begin("cfg", false);
    if (!doc["lights"].isNull()) {
        String s;
        serializeJson(doc["lights"], s);
        Serial.printf("[CFG] lights serialized: %d bytes\n", s.length());
        size_t written = p.putString("lights", s);
        Serial.printf("[CFG] lights putString returned: %d\n", written);
    } else {
        Serial.println("[CFG] lights key is NULL in JSON");
    }
    p.end();
    yield(); esp_task_wdt_reset();

    p.begin("cfg", false);
    if (!doc["climate"].isNull()) {
        String s;
        serializeJson(doc["climate"], s);
        Serial.printf("[CFG] climate serialized: %d bytes\n", s.length());
        size_t written = p.putString("climate", s);
        Serial.printf("[CFG] climate putString returned: %d\n", written);
    } else {
        Serial.println("[CFG] climate key is NULL in JSON");
    }
    p.end();
    yield(); esp_task_wdt_reset();

    p.begin("cfg", false);
    if (!doc["sensors"].isNull()) {
        String s;
        serializeJson(doc["sensors"], s);
        Serial.printf("[CFG] sensors serialized: %d bytes\n", s.length());
        size_t written = p.putString("sensors", s);
        Serial.printf("[CFG] sensors putString returned: %d\n", written);
    } else {
        Serial.println("[CFG] sensors key is NULL in JSON");
    }
    p.end();
    yield(); esp_task_wdt_reset();

    p.begin("cfg", false);
    if (!doc["media"].isNull()) {
        String s;
        serializeJson(doc["media"], s);
        Serial.printf("[CFG] media serialized: %d bytes\n", s.length());
        p.putString("media", s);
    }
    if (!doc["persons"].isNull()) {
        String s;
        serializeJson(doc["persons"], s);
        Serial.printf("[CFG] persons serialized: %d bytes\n", s.length());
        p.putString("persons", s);
    }
    if (doc["tab3"].is<const char*>()) {
        p.putString("tab3", doc["tab3"].as<String>());
        Serial.printf("[CFG] tab3 set to %s\n", doc["tab3"].as<const char*>());
    }
    p.end();
    yield(); esp_task_wdt_reset();

    // ── Display settings ──────────────────────────────
    p.begin("cfg", false);
    if (!doc["display"].isNull()) {
        if (doc["display"]["brightness"].is<int>()) {
            uint8_t br = doc["display"]["brightness"].as<uint8_t>();
            p.putUChar("scr_bright", br);
            display_set_brightness(br);
            Serial.printf("[CFG] brightness set to %u%%\n", br);
        }
        if (doc["display"]["timeout"].is<int>()) {
            uint16_t to = doc["display"]["timeout"].as<uint16_t>();
            p.putUShort("scr_timeout", to);
            display_set_screen_timeout(to);
            Serial.printf("[CFG] screen timeout set to %u s\n", to);
        }
    }

    // ── Network / Static IP settings ────────────────
    if (!doc["network"].isNull()) {
        const char *sip  = doc["network"]["static_ip"] | "";
        const char *sgw  = doc["network"]["gateway"]   | "";
        const char *ssn  = doc["network"]["subnet"]    | "255.255.255.0";
        const char *sdns = doc["network"]["dns"]       | "";
        wifi_mgr_set_static_ip(sip, sgw, ssn, sdns);
        Serial.printf("[CFG] static IP set to %s (restart to apply)\n",
                      strlen(sip) > 0 ? sip : "DHCP");
    }

    // Bump version
    uint32_t ver = p.getUInt("version", 0) + 1;
    p.putUInt("version", ver);
    p.end();

    Serial.printf("[CFG] Config v%u saved (%d bytes), rebuild requested\n",
                  ver, body.length());

    // Update RAM cache so ha_loop() rebuild never touches NVS
    {
        String cLights, cClimate, cSensors, cMedia, cPersons, cTab3;
        if (!doc["lights"].isNull())  serializeJson(doc["lights"], cLights);
        if (!doc["climate"].isNull()) serializeJson(doc["climate"], cClimate);
        if (!doc["sensors"].isNull()) serializeJson(doc["sensors"], cSensors);
        if (!doc["media"].isNull())   serializeJson(doc["media"], cMedia);
        if (!doc["persons"].isNull()) serializeJson(doc["persons"], cPersons);
        if (doc["tab3"].is<const char*>()) cTab3 = doc["tab3"].as<String>();
        ha_cache_config(
            doc["ha_url"].is<const char*>()   ? doc["ha_url"].as<const char*>()   : nullptr,
            doc["ha_token"].is<const char*>() ? doc["ha_token"].as<const char*>() : nullptr,
            cLights, cClimate, cSensors, cMedia, cPersons, cTab3);
    }

    // Send response BEFORE triggering rebuild (rebuild destroys LVGL objects)
    cfgServer->send(200, "application/json",
        String("{\"ok\":true,\"version\":") + String(ver) + "}");

    _configAvailable = true;
    _needsRebuild = true;
}

// ── Handler: OPTIONS (CORS preflight) ──────────────────
static void handleCORS() {
    cfgServer->sendHeader("Access-Control-Allow-Origin", "*");
    cfgServer->sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    cfgServer->sendHeader("Access-Control-Allow-Headers", "Content-Type");
    cfgServer->send(204);
}

// ═══════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════

void config_server_start() {
    _configAvailable = configExists();

    // Use port 8080 to avoid conflict with wifi_mgr's port 80 AP server
    cfgServer = new WebServer(8080);

    cfgServer->on("/api/info",   HTTP_GET,     handleInfo);
    cfgServer->on("/api/config", HTTP_GET,     handleGetConfig);
    cfgServer->on("/api/config", HTTP_POST,    handlePostConfig);
    cfgServer->on("/api/info",   HTTP_OPTIONS, handleCORS);
    cfgServer->on("/api/config", HTTP_OPTIONS, handleCORS);
    cfgServer->begin();

    Serial.printf("[CFG] Config server on port 8080 (configured=%s)\n",
                  _configAvailable ? "yes" : "no");
}

void config_server_loop() {
    if (cfgServer) cfgServer->handleClient();
}

bool config_available() {
    return _configAvailable;
}

void config_request_rebuild() {
    _needsRebuild = true;
}

bool config_needs_rebuild() {
    if (_needsRebuild) {
        _needsRebuild = false;
        return true;
    }
    return false;
}
