/**
 * ha.cpp — Home Assistant control panel  (Black & Gold)
 * ═════════════════════════════════════════════════════
 * FULLY DYNAMIC — all entities loaded from Preferences
 * (pushed via HACS integration panel).
 * No hardcoded entity IDs.
 *
 * FreeRTOS fetch task (core 0) → UI update on core 1.
 * PSRAM JSON allocator + per-entity fetch.
 * Lights: categorised scrollable grid, long-press to dim.
 * Climate: arc gauges + AC cards.   Sensors: door + motion.
 */

#include "ha.h"
#include "config.h"
#include "config_server.h"
#include "display_hal.h"
#include "wifi_mgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Custom FontAwesome icon font (lightbulb + plug + fan)
LV_FONT_DECLARE(fa_icons_16);
#define FA_LIGHTBULB  "\xEF\x83\xAB" /* 0xF0EB */
#define FA_PLUG       "\xEF\x87\xA6" /* 0xF1E6 */
#define FA_FAN        "\xEF\x81\xA9" /* 0xF069 asterisk */

// ── Timing ─────────────────────────────────────────────
#define HA_POLL_MS 5000
#define LIGHT_SKIP_MS 6000

// ── Black & Gold palette ───────────────────────────────
#define C_BG       0x000000
#define C_CARD     0x0D0D0D
#define C_TILE_OFF 0x111111
#define C_GOLD     0xD4A017
#define C_GOLD_B   0xFFD700
#define C_GOLD_D   0x6B5310
#define C_TXT_OFF  0x4A4A4A
#define C_WHITE    0xE8E8E8
#define C_DIM      0x555555
#define C_BTN      0x1A1A0A
#define C_PRESS    0x2A2210

// Low-battery red palette alternatives
#define C_RED      0xE74C3C
#define C_RED_B    0xFF6B6B
#define C_RED_D    0x7B2020

// Runtime accent colors (swap gold→red at ≤10% battery)
static uint32_t accent    = C_GOLD;
static uint32_t accent_b  = C_GOLD_B;
static uint32_t accent_d  = C_GOLD_D;
static bool     _lowBat   = false;
static uint32_t _lastBatCheckMs = 0;

// ── PSRAM-backed ArduinoJson allocator ─────────────────
struct PsAllocator : ArduinoJson::Allocator {
    void* allocate(size_t s) override          { return ps_malloc(s); }
    void  deallocate(void* p) override         { free(p); }
    void* reallocate(void* p, size_t s) override { return ps_realloc(p, s); }
};
static PsAllocator psAlloc;

// ═══════════════════════════════════════════════════════
//  NVS config cache — all NVS is read once before the RGB
//  panel starts; after that we only use cached copies.
//  "Cache disabled but cached memory region accessed" crash
//  happens when NVS flash reads occur while the RGB LCD
//  DMA ISR is active — this avoids that entirely.
// ═══════════════════════════════════════════════════════
static String _nvs_ha_url;
static String _nvs_ha_token;
static String _nvs_lights   = "[]";
static String _nvs_climate  = "{}";
static String _nvs_sensors  = "{}";
static String _nvs_media    = "[]";
static String _nvs_persons  = "[]";
static String _nvs_tab3     = "sensors";
static uint8_t  _nvs_brightness  = 80;
static uint16_t _nvs_timeout     = 30;

void ha_preload_config() {
    Preferences p;
    p.begin("cfg", true);
    _nvs_ha_url    = p.getString("ha_url", "");
    _nvs_ha_token  = p.getString("ha_token", "");
    _nvs_lights    = p.getString("lights", "[]");
    _nvs_climate   = p.getString("climate", "{}");
    _nvs_sensors   = p.getString("sensors", "{}");
    _nvs_media     = p.getString("media", "[]");
    _nvs_persons   = p.getString("persons", "[]");
    _nvs_tab3      = p.getString("tab3", "sensors");
    _nvs_brightness  = p.getUChar("scr_bright", 80);
    _nvs_timeout     = p.getUShort("scr_timeout", 30);
    p.end();
    Serial.println("[HA] NVS config preloaded into RAM cache");
}

void ha_cache_config(const char* url, const char* token,
                     const String& lights, const String& climate,
                     const String& sensors, const String& media,
                     const String& persons, const String& tab3) {
    if (url   && strlen(url) > 0)   _nvs_ha_url   = url;
    if (token && strlen(token) > 0) _nvs_ha_token = token;
    if (lights.length() > 0)  _nvs_lights  = lights;
    if (climate.length() > 0) _nvs_climate = climate;
    if (sensors.length() > 0) _nvs_sensors = sensors;
    if (media.length() > 0)   _nvs_media   = media;
    if (persons.length() > 0) _nvs_persons = persons;
    if (tab3.length() > 0)    _nvs_tab3    = tab3;
}

uint8_t  ha_get_cached_brightness()  { return _nvs_brightness; }
uint16_t ha_get_cached_timeout()     { return _nvs_timeout; }
const String& ha_get_cached_lights()  { return _nvs_lights; }
const String& ha_get_cached_climate() { return _nvs_climate; }
const String& ha_get_cached_sensors() { return _nvs_sensors; }
const String& ha_get_cached_media()   { return _nvs_media; }
const String& ha_get_cached_persons() { return _nvs_persons; }
const String& ha_get_cached_tab3()    { return _nvs_tab3; }
const String& ha_get_cached_url()     { return _nvs_ha_url; }

// ═══════════════════════════════════════════════════════
//  Dynamic config — URL & token (from cache)
// ═══════════════════════════════════════════════════════
static String cfg_ha_url;
static String cfg_ha_token;

static void load_config_credentials() {
    cfg_ha_url   = _nvs_ha_url.length() > 0   ? _nvs_ha_url   : HA_URL;
    cfg_ha_token = _nvs_ha_token.length() > 0 ? _nvs_ha_token : HA_TOKEN;
    Serial.printf("[HA] Using URL: %s\n", cfg_ha_url.c_str());
}

const char* ha_get_url()   { return cfg_ha_url.c_str(); }
const char* ha_get_token() { return cfg_ha_token.c_str(); }

// ═══════════════════════════════════════════════════════
//  Dynamic entity arrays — loaded from Preferences JSON
// ═══════════════════════════════════════════════════════

// Max entity counts (PSRAM-allocated)
#define MAX_LIGHTS 30
#define MAX_ACS     6
#define MAX_DOORS  12
#define MAX_MOTION  6
#define MAX_MEDIA   6
#define MAX_PERSONS 8

struct LightEnt {
    char eid[80];
    char label[24];
    char domain[8];    // "light" or "switch"
    char cat[20];      // category: Hall, Dining, etc.
    char icon[8];      // "bulb","tube","fan","socket","alarm","warm"
    bool dimmable;
    bool rgb;
    bool is_on;
    uint8_t bri;
    unsigned long skip_until;
    lv_obj_t *tile;
    lv_obj_t *lbl;
    lv_obj_t *icon_lbl;
};

struct ACEnt {
    char eid[80];
    char label[24];
    int  min_temp;
    int  max_temp;
    float tgt;
    float room_temp;
    char mode[16];
    char fan[16];
    // UI handles
    lv_obj_t *lbl_mode;
    lv_obj_t *lbl_tgt;
    lv_obj_t *lbl_info;
    lv_obj_t *btn_pwr;
};

struct DoorEnt {
    char eid[80];
    char label[24];
    bool inverted;
    bool is_open;
    lv_obj_t *tile;
    lv_obj_t *lbl;
    lv_obj_t *icon_lbl;
};

struct MotionEnt {
    char eid[80];
    char label[24];
    bool active;
    lv_obj_t *tile;
    lv_obj_t *lbl;
    lv_obj_t *icon_lbl;
};

struct MediaEnt {
    char eid[80];
    char label[24];
    char state[16];       // "playing","paused","idle","off","standby"
    char title[48];       // media_title attribute
    uint8_t volume;       // 0-100
    bool is_volume_muted;
    lv_obj_t *card;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_state;
    lv_obj_t *btn_play;
    lv_obj_t *sld_vol;
};

struct PersonEnt {
    char eid[80];
    char label[24];
    char state[32];       // "home","not_home", or zone name
    lv_obj_t *tile;
    lv_obj_t *lbl;
    lv_obj_t *lbl_state;
};

// Category for grouping lights
struct Category {
    char name[20];
    int start;
    int count;
};
#define MAX_CATS 12

static LightEnt  *lights = nullptr;
static int LC = 0;
static ACEnt     *acs = nullptr;
static int AC_COUNT = 0;
static DoorEnt   *doors = nullptr;
static int DC = 0;
static MotionEnt *motions = nullptr;
static int MC = 0;
static MediaEnt  *media_ents = nullptr;
static int MEDIA_COUNT = 0;
static PersonEnt *person_ents = nullptr;
static int PERSON_COUNT = 0;

static char tab3_type[16] = "sensors";  // "sensors", "media", or "presence"

static Category dyn_cats[MAX_CATS];
static int CAT_COUNT = 0;

static char temp_sensor_eid[80] = "";
static char hum_sensor_eid[80]  = "";
static float indoor_temp = 0, indoor_hum = 0;

static bool cfg_loaded = false;

// ── Load entities from cache (no NVS reads!) ──────────
static bool load_entities_from_prefs() {
    String lightsJson  = _nvs_lights;
    String climateJson = _nvs_climate;
    String sensorsJson = _nvs_sensors;

    Serial.printf("[HA] Loading config: lights=%d climate=%d sensors=%d bytes\n",
                  lightsJson.length(), climateJson.length(), sensorsJson.length());

    // Parse lights
    {
        JsonDocument doc;
        if (deserializeJson(doc, lightsJson) == DeserializationError::Ok) {
            JsonArray arr = doc.as<JsonArray>();
            LC = min((int)arr.size(), MAX_LIGHTS);
            for (int i = 0; i < LC; i++) {
                JsonObject o = arr[i];
                strlcpy(lights[i].eid,    o["eid"] | "", sizeof(lights[i].eid));
                strlcpy(lights[i].label,  o["label"] | "Light", sizeof(lights[i].label));
                strlcpy(lights[i].domain, o["domain"] | "switch", sizeof(lights[i].domain));
                strlcpy(lights[i].cat,    o["cat"] | "Custom", sizeof(lights[i].cat));
                strlcpy(lights[i].icon,   o["icon"] | "bulb", sizeof(lights[i].icon));
                lights[i].dimmable = o["dimmable"] | false;
                lights[i].rgb      = o["rgb"] | false;
                lights[i].is_on = false;
                lights[i].bri = 0;
                lights[i].skip_until = 0;
                lights[i].tile = nullptr;
                lights[i].lbl = nullptr;
                lights[i].icon_lbl = nullptr;
            }
        }
    }

    // Build categories from light data
    // Sort lights by category, build cat array
    // Simple: scan unique cats in order they appear
    CAT_COUNT = 0;
    for (int i = 0; i < LC && CAT_COUNT < MAX_CATS; i++) {
        bool found = false;
        for (int c = 0; c < CAT_COUNT; c++) {
            if (strcmp(dyn_cats[c].name, lights[i].cat) == 0) {
                dyn_cats[c].count++;
                found = true;
                break;
            }
        }
        if (!found) {
            strlcpy(dyn_cats[CAT_COUNT].name, lights[i].cat, sizeof(dyn_cats[CAT_COUNT].name));
            dyn_cats[CAT_COUNT].start = i;
            dyn_cats[CAT_COUNT].count = 1;
            CAT_COUNT++;
        }
    }
    // Fix start indices — since lights may not be contiguous by cat,
    // we reorder lights in-place to group by category
    // (simple stable reorder)
    if (CAT_COUNT > 1 && LC > 0) {
        LightEnt *tmp = (LightEnt*)ps_malloc(LC * sizeof(LightEnt));
        if (tmp) {
            int pos = 0;
            for (int c = 0; c < CAT_COUNT; c++) {
                dyn_cats[c].start = pos;
                dyn_cats[c].count = 0;
                for (int i = 0; i < LC; i++) {
                    if (strcmp(lights[i].cat, dyn_cats[c].name) == 0) {
                        memcpy(&tmp[pos], &lights[i], sizeof(LightEnt));
                        pos++;
                        dyn_cats[c].count++;
                    }
                }
            }
            memcpy(lights, tmp, LC * sizeof(LightEnt));
            free(tmp);
        }
    }

    // Parse climate
    {
        JsonDocument doc;
        if (deserializeJson(doc, climateJson) == DeserializationError::Ok) {
            strlcpy(temp_sensor_eid, doc["temp_sensor"] | "", sizeof(temp_sensor_eid));
            strlcpy(hum_sensor_eid,  doc["hum_sensor"] | "", sizeof(hum_sensor_eid));
            JsonArray arr = doc["acs"].as<JsonArray>();
            AC_COUNT = min((int)arr.size(), MAX_ACS);
            for (int i = 0; i < AC_COUNT; i++) {
                JsonObject o = arr[i];
                strlcpy(acs[i].eid,   o["eid"] | "", sizeof(acs[i].eid));
                strlcpy(acs[i].label, o["label"] | "AC", sizeof(acs[i].label));
                acs[i].min_temp = o["min"] | 16;
                acs[i].max_temp = o["max"] | 30;
                acs[i].tgt = 24;
                acs[i].room_temp = 0;
                strlcpy(acs[i].mode, "off", 16);
                strlcpy(acs[i].fan, "auto", 16);
                acs[i].lbl_mode = nullptr;
                acs[i].lbl_tgt  = nullptr;
                acs[i].lbl_info = nullptr;
                acs[i].btn_pwr  = nullptr;
            }
        }
    }

    // Parse sensors
    {
        JsonDocument doc;
        if (deserializeJson(doc, sensorsJson) == DeserializationError::Ok) {
            JsonArray darr = doc["doors"].as<JsonArray>();
            DC = min((int)darr.size(), MAX_DOORS);
            for (int i = 0; i < DC; i++) {
                JsonObject o = darr[i];
                strlcpy(doors[i].eid,   o["eid"] | "", sizeof(doors[i].eid));
                strlcpy(doors[i].label, o["label"] | "Door", sizeof(doors[i].label));
                doors[i].inverted = o["inverted"] | false;
                doors[i].is_open = false;
                doors[i].tile = nullptr;
                doors[i].lbl = nullptr;
                doors[i].icon_lbl = nullptr;
            }
            JsonArray marr = doc["motion"].as<JsonArray>();
            MC = min((int)marr.size(), MAX_MOTION);
            for (int i = 0; i < MC; i++) {
                JsonObject o = marr[i];
                strlcpy(motions[i].eid,   o["eid"] | "", sizeof(motions[i].eid));
                strlcpy(motions[i].label, o["label"] | "Motion", sizeof(motions[i].label));
                motions[i].active = false;
                motions[i].tile = nullptr;
                motions[i].lbl = nullptr;
                motions[i].icon_lbl = nullptr;
            }
        }
    }

    // Parse media players
    {
        String mediaJson = _nvs_media;
        JsonDocument doc;
        if (deserializeJson(doc, mediaJson) == DeserializationError::Ok) {
            JsonArray arr = doc.as<JsonArray>();
            MEDIA_COUNT = min((int)arr.size(), MAX_MEDIA);
            for (int i = 0; i < MEDIA_COUNT; i++) {
                JsonObject o = arr[i];
                strlcpy(media_ents[i].eid,   o["eid"] | "", sizeof(media_ents[i].eid));
                strlcpy(media_ents[i].label, o["label"] | "Media", sizeof(media_ents[i].label));
                strlcpy(media_ents[i].state, "off", sizeof(media_ents[i].state));
                media_ents[i].title[0] = '\0';
                media_ents[i].volume = 50;
                media_ents[i].is_volume_muted = false;
                media_ents[i].card = nullptr;
                media_ents[i].lbl_title = nullptr;
                media_ents[i].lbl_state = nullptr;
                media_ents[i].btn_play = nullptr;
                media_ents[i].sld_vol = nullptr;
            }
        }
    }

    // Parse persons
    {
        String personsJson = _nvs_persons;
        JsonDocument doc;
        if (deserializeJson(doc, personsJson) == DeserializationError::Ok) {
            JsonArray arr = doc.as<JsonArray>();
            PERSON_COUNT = min((int)arr.size(), MAX_PERSONS);
            for (int i = 0; i < PERSON_COUNT; i++) {
                JsonObject o = arr[i];
                strlcpy(person_ents[i].eid,   o["eid"] | "", sizeof(person_ents[i].eid));
                strlcpy(person_ents[i].label, o["label"] | "Person", sizeof(person_ents[i].label));
                strlcpy(person_ents[i].state, "unknown", sizeof(person_ents[i].state));
                person_ents[i].tile = nullptr;
                person_ents[i].lbl = nullptr;
                person_ents[i].lbl_state = nullptr;
            }
        }
    }

    // Tab3 type
    strlcpy(tab3_type, _nvs_tab3.c_str(), sizeof(tab3_type));

    Serial.printf("[HA] Loaded: %d lights (%d cats), %d ACs, %d doors, %d motion, %d media, %d persons\n",
                  LC, CAT_COUNT, AC_COUNT, DC, MC, MEDIA_COUNT, PERSON_COUNT);
    Serial.printf("[HA] Temp sensor: %s  Hum sensor: %s  Tab3: %s\n", temp_sensor_eid, hum_sensor_eid, tab3_type);

    return (LC > 0 || AC_COUNT > 0 || DC > 0 || MC > 0 || MEDIA_COUNT > 0 || PERSON_COUNT > 0);
}

// ── FreeRTOS ───────────────────────────────────────────
static SemaphoreHandle_t haMutex = nullptr;
static volatile bool     newData = false;
static TaskHandle_t      fetchTaskH = nullptr;

// ═══════════════════════════════════════════════════════
//  LVGL widget handles
// ═══════════════════════════════════════════════════════
static lv_obj_t *arc_temp=nullptr, *lbl_tempv=nullptr;
static lv_obj_t *arc_hum=nullptr,  *lbl_humv=nullptr;
static lv_obj_t *lbl_status=nullptr;

// Dim popup
static lv_obj_t *dim_overlay = nullptr;
static int       dim_idx     = -1;
static lv_obj_t *dim_arc     = nullptr;
static lv_obj_t *dim_pct_lbl = nullptr;
static unsigned long dim_last_send = 0;

// ═══════════════════════════════════════════════════════
//  HA service call
// ═══════════════════════════════════════════════════════
static bool ha_call(const char *domain, const char *svc, const String &body) {
    char url[200];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", cfg_ha_url.c_str(), domain, svc);
    HTTPClient h;
    h.begin(url);
    h.addHeader("Authorization", String("Bearer ") + cfg_ha_token);
    h.addHeader("Content-Type", "application/json");
    h.setTimeout(4000);
    int c = h.POST(body);
    h.end();
    Serial.printf("[HA] %s/%s -> %d\n", domain, svc, c);
    return c == 200;
}

// ═══════════════════════════════════════════════════════
//  Per-entity fetch helper
// ═══════════════════════════════════════════════════════
static bool fetchOne(const char *eid, JsonDocument &doc) {
    HTTPClient h;
    char url[200];
    snprintf(url, sizeof(url), "%s/api/states/%s", cfg_ha_url.c_str(), eid);
    h.begin(url);
    h.addHeader("Authorization", String("Bearer ") + cfg_ha_token);
    h.setTimeout(5000);
    int code = h.GET();
    if (code != 200) { h.end(); return false; }
    String body = h.getString();
    h.end();
    doc.clear();
    return deserializeJson(doc, body) == DeserializationError::Ok;
}

// ═══════════════════════════════════════════════════════
//  Background fetch task (pinned to core 0)
// ═══════════════════════════════════════════════════════
static void fetchTask(void *) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (;;) {
        if (!wifi_mgr_connected() || !cfg_loaded) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        JsonDocument doc;
        unsigned long now = millis();

        // ── Fetch lights ───────────────────────────────
        bool    t_on[MAX_LIGHTS]  = {};
        uint8_t t_bri[MAX_LIGHTS] = {};
        for (int i = 0; i < LC; i++) {
            if (lights[i].skip_until > now) {
                t_on[i]  = lights[i].is_on;
                t_bri[i] = lights[i].bri;
                continue;
            }
            if (fetchOne(lights[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) t_on[i] = (strcmp(st, "on") == 0);
                t_bri[i] = doc["attributes"]["brightness"] | 0;
            }
        }

        // ── Fetch temp & humidity ──────────────────────
        float t_itemp = indoor_temp, t_ihum = indoor_hum;
        if (temp_sensor_eid[0] && fetchOne(temp_sensor_eid, doc))
            t_itemp = atof(doc["state"] | "0");
        if (hum_sensor_eid[0] && fetchOne(hum_sensor_eid, doc))
            t_ihum = atof(doc["state"] | "0");

        // ── Fetch ACs ──────────────────────────────────
        float t_ac_tgt[MAX_ACS] = {};
        float t_ac_rm[MAX_ACS]  = {};
        char  t_ac_mode[MAX_ACS][16] = {};
        char  t_ac_fan[MAX_ACS][16]  = {};
        for (int i = 0; i < AC_COUNT; i++) {
            strlcpy(t_ac_mode[i], acs[i].mode, 16);
            strlcpy(t_ac_fan[i], acs[i].fan, 16);
            t_ac_tgt[i] = acs[i].tgt;
            t_ac_rm[i]  = acs[i].room_temp;
            if (fetchOne(acs[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) strlcpy(t_ac_mode[i], st, 16);
                t_ac_tgt[i] = doc["attributes"]["temperature"] | 24.0f;
                t_ac_rm[i]  = doc["attributes"]["current_temperature"] | 0.0f;
                const char *fm = doc["attributes"]["fan_mode"];
                if (fm) strlcpy(t_ac_fan[i], fm, 16);
            }
        }

        // ── Fetch doors ────────────────────────────────
        bool t_door[MAX_DOORS] = {};
        for (int i = 0; i < DC; i++) {
            if (fetchOne(doors[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) {
                    bool raw = (strcmp(st, "on") == 0);
                    t_door[i] = doors[i].inverted ? !raw : raw;
                }
            }
        }

        // ── Fetch motion ───────────────────────────────
        bool t_motion[MAX_MOTION] = {};
        for (int i = 0; i < MC; i++) {
            if (fetchOne(motions[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) t_motion[i] = (strcmp(st, "on") == 0);
            }
        }

        // ── Fetch media players ────────────────────────
        char  t_media_state[MAX_MEDIA][16] = {};
        char  t_media_title[MAX_MEDIA][48] = {};
        uint8_t t_media_vol[MAX_MEDIA] = {};
        bool  t_media_muted[MAX_MEDIA] = {};
        for (int i = 0; i < MEDIA_COUNT; i++) {
            strlcpy(t_media_state[i], media_ents[i].state, 16);
            strlcpy(t_media_title[i], media_ents[i].title, 48);
            t_media_vol[i] = media_ents[i].volume;
            t_media_muted[i] = media_ents[i].is_volume_muted;
            if (fetchOne(media_ents[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) strlcpy(t_media_state[i], st, 16);
                const char *mt = doc["attributes"]["media_title"];
                if (mt) strlcpy(t_media_title[i], mt, 48);
                else t_media_title[i][0] = '\0';
                float vl = doc["attributes"]["volume_level"] | -1.0f;
                if (vl >= 0) t_media_vol[i] = (uint8_t)(vl * 100);
                t_media_muted[i] = doc["attributes"]["is_volume_muted"] | false;
            }
        }

        // ── Fetch persons ──────────────────────────────
        char t_person_state[MAX_PERSONS][32] = {};
        for (int i = 0; i < PERSON_COUNT; i++) {
            strlcpy(t_person_state[i], person_ents[i].state, 32);
            if (fetchOne(person_ents[i].eid, doc)) {
                const char *st = doc["state"];
                if (st) strlcpy(t_person_state[i], st, 32);
            }
        }

        // ── Copy under mutex ───────────────────────────
        xSemaphoreTake(haMutex, portMAX_DELAY);
        for (int i = 0; i < LC; i++) {
            lights[i].is_on = t_on[i];
            lights[i].bri   = t_bri[i];
        }
        indoor_temp = t_itemp;
        indoor_hum  = t_ihum;
        for (int i = 0; i < AC_COUNT; i++) {
            acs[i].tgt = t_ac_tgt[i];
            acs[i].room_temp = t_ac_rm[i];
            strlcpy(acs[i].mode, t_ac_mode[i], 16);
            strlcpy(acs[i].fan, t_ac_fan[i], 16);
        }
        for (int i = 0; i < DC; i++)
            doors[i].is_open = t_door[i];
        for (int i = 0; i < MC; i++)
            motions[i].active = t_motion[i];
        for (int i = 0; i < MEDIA_COUNT; i++) {
            strlcpy(media_ents[i].state, t_media_state[i], 16);
            strlcpy(media_ents[i].title, t_media_title[i], 48);
            media_ents[i].volume = t_media_vol[i];
            media_ents[i].is_volume_muted = t_media_muted[i];
        }
        for (int i = 0; i < PERSON_COUNT; i++)
            strlcpy(person_ents[i].state, t_person_state[i], 32);
        newData = true;
        xSemaphoreGive(haMutex);

        Serial.printf("[HA] OK  T=%d.%d H=%d  L=%d/%d  AC=%d  D=%d  M=%d\n",
                      (int)t_itemp, ((int)(t_itemp*10))%10, (int)t_ihum,
                      LC, LC, AC_COUNT, DC, MC);

        vTaskDelay(pdMS_TO_TICKS(HA_POLL_MS));
    }
}

// ═══════════════════════════════════════════════════════
//  UI update  (called from main loop, same core as LVGL)
// ═══════════════════════════════════════════════════════
static void update_ui() {
    if (!newData) return;
    xSemaphoreTake(haMutex, portMAX_DELAY);
    newData = false;

    for (int i = 0; i < LC; i++) {
        if (!lights[i].tile) continue;
        bool on = lights[i].is_on;
        lv_obj_set_style_bg_color(lights[i].tile,
            lv_color_hex(on ? accent : C_TILE_OFF), 0);
        if (lights[i].lbl)
            lv_obj_set_style_text_color(lights[i].lbl,
                lv_color_hex(on ? 0x000000 : C_TXT_OFF), 0);
        if (lights[i].icon_lbl)
            lv_obj_set_style_text_color(lights[i].icon_lbl,
                lv_color_hex(on ? 0x000000 : accent_d), 0);
    }

    for (int i = 0; i < DC; i++) {
        if (!doors[i].tile) continue;
        bool open = doors[i].is_open;
        lv_obj_set_style_bg_color(doors[i].tile,
            lv_color_hex(open ? accent : C_TILE_OFF), 0);
        if (doors[i].lbl)
            lv_obj_set_style_text_color(doors[i].lbl,
                lv_color_hex(open ? 0x000000 : C_TXT_OFF), 0);
        if (doors[i].icon_lbl)
            lv_obj_set_style_text_color(doors[i].icon_lbl,
                lv_color_hex(open ? 0x000000 : accent_d), 0);
    }

    for (int i = 0; i < MC; i++) {
        if (!motions[i].tile) continue;
        bool on = motions[i].active;
        lv_obj_set_style_bg_color(motions[i].tile,
            lv_color_hex(on ? accent : C_TILE_OFF), 0);
        if (motions[i].lbl)
            lv_obj_set_style_text_color(motions[i].lbl,
                lv_color_hex(on ? 0x000000 : C_TXT_OFF), 0);
        if (motions[i].icon_lbl)
            lv_obj_set_style_text_color(motions[i].icon_lbl,
                lv_color_hex(on ? 0x000000 : accent_d), 0);
    }

    // Climate arcs
    if (arc_temp) lv_arc_set_value(arc_temp,
        constrain((int)(indoor_temp * 10), 150, 500));
    if (lbl_tempv) {
        int t10 = (int)(indoor_temp * 10);
        lv_label_set_text_fmt(lbl_tempv, "%d.%dC", t10/10, abs(t10%10));
    }
    if (arc_hum) lv_arc_set_value(arc_hum,
        constrain((int)indoor_hum, 0, 100));
    if (lbl_humv) lv_label_set_text_fmt(lbl_humv, "%d%%", (int)indoor_hum);

    // AC cards
    char buf[32];
    for (int i = 0; i < AC_COUNT; i++) {
        if (acs[i].lbl_mode) {
            strlcpy(buf, acs[i].mode, 16);
            for (char *c = buf; *c; c++) *c = toupper(*c);
            lv_label_set_text(acs[i].lbl_mode, buf);
        }
        if (acs[i].lbl_tgt)
            lv_label_set_text_fmt(acs[i].lbl_tgt, "%d", (int)acs[i].tgt);
        if (acs[i].lbl_info)
            lv_label_set_text_fmt(acs[i].lbl_info, "Room %dC   Fan: %s",
                                  (int)acs[i].room_temp, acs[i].fan);
        if (acs[i].btn_pwr) {
            bool on = (strcmp(acs[i].mode, "off") != 0);
            lv_obj_set_style_bg_color(acs[i].btn_pwr,
                lv_color_hex(on ? accent : C_BTN), 0);
        }
    }

    // Media players
    for (int i = 0; i < MEDIA_COUNT; i++) {
        if (media_ents[i].lbl_state) {
            char upper[16];
            strlcpy(upper, media_ents[i].state, 16);
            for (char *c = upper; *c; c++) *c = toupper(*c);
            lv_label_set_text(media_ents[i].lbl_state, upper);
        }
        if (media_ents[i].lbl_title) {
            lv_label_set_text(media_ents[i].lbl_title,
                media_ents[i].title[0] ? media_ents[i].title : "—");
        }
        if (media_ents[i].btn_play) {
            bool playing = (strcmp(media_ents[i].state, "playing") == 0);
            lv_obj_set_style_bg_color(media_ents[i].btn_play,
                lv_color_hex(playing ? accent : C_BTN), 0);
        }
        if (media_ents[i].sld_vol && !media_ents[i].is_volume_muted) {
            lv_slider_set_value(media_ents[i].sld_vol, media_ents[i].volume, LV_ANIM_ON);
        }
    }

    // Persons
    for (int i = 0; i < PERSON_COUNT; i++) {
        if (person_ents[i].tile) {
            bool home = (strcmp(person_ents[i].state, "home") == 0);
            lv_obj_set_style_bg_color(person_ents[i].tile,
                lv_color_hex(home ? accent : C_TILE_OFF), 0);
        }
        if (person_ents[i].lbl) {
            bool home = (strcmp(person_ents[i].state, "home") == 0);
            lv_obj_set_style_text_color(person_ents[i].lbl,
                lv_color_hex(home ? 0x000000 : C_TXT_OFF), 0);
        }
        if (person_ents[i].lbl_state) {
            const char *s = person_ents[i].state;
            const char *display = strcmp(s, "home") == 0 ? "Home" :
                                  strcmp(s, "not_home") == 0 ? "Away" : s;
            lv_label_set_text(person_ents[i].lbl_state, display);
        }
    }

    if (lbl_status) lv_label_set_text(lbl_status, "");
    xSemaphoreGive(haMutex);
}

// ═══════════════════════════════════════════════════════
//  Callbacks
// ═══════════════════════════════════════════════════════

static void close_dim(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    if (dim_overlay) { lv_obj_del(dim_overlay); dim_overlay = nullptr; dim_idx = -1; }
}

static void dim_arc_cb(lv_event_t *e) {
    lv_obj_t *a = lv_event_get_target(e);
    int val = lv_arc_get_value(a);
    if (dim_pct_lbl) lv_label_set_text_fmt(dim_pct_lbl, "%d%%", val);
    if (dim_idx < 0 || dim_idx >= LC) return;
    if (millis() - dim_last_send < 400) return;
    dim_last_send = millis();
    int bri = val * 255 / 100;
    String body = String("{\"entity_id\":\"") + lights[dim_idx].eid +
                  "\",\"brightness\":" + String(bri) + "}";
    ha_call("light", "turn_on", body);
}

static void show_dim_popup(int idx) {
    dim_idx = idx;
    dim_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(dim_overlay, 480, 480);
    lv_obj_set_pos(dim_overlay, 0, 0);
    lv_obj_set_style_bg_color(dim_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dim_overlay, 0, 0);
    lv_obj_set_style_radius(dim_overlay, 0, 0);
    lv_obj_clear_flag(dim_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dim_overlay, close_dim, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(dim_overlay);
    lv_obj_set_size(card, 240, 300);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_GOLD_D), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, lights[idx].label);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    dim_arc = lv_arc_create(card);
    lv_obj_set_size(dim_arc, 170, 170);
    lv_arc_set_rotation(dim_arc, 135);
    lv_arc_set_bg_angles(dim_arc, 0, 270);
    lv_arc_set_range(dim_arc, 1, 100);
    int cur = lights[idx].bri * 100 / 255;
    if (cur < 1) cur = 50;
    lv_arc_set_value(dim_arc, cur);
    lv_obj_set_style_arc_width(dim_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dim_arc, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(dim_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(dim_arc, lv_color_hex(C_GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dim_arc, lv_color_hex(C_GOLD_B), LV_PART_KNOB);
    lv_obj_set_style_pad_all(dim_arc, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(dim_arc, dim_arc_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    dim_pct_lbl = lv_label_create(dim_arc);
    lv_label_set_text_fmt(dim_pct_lbl, "%d%%", cur);
    lv_obj_set_style_text_font(dim_pct_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dim_pct_lbl, lv_color_hex(C_GOLD_B), 0);
    lv_obj_center(dim_pct_lbl);
}

// ── Light tile callbacks ───────────────────────────────
static void light_tap_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= LC) return;
    lights[idx].is_on = !lights[idx].is_on;
    lights[idx].skip_until = millis() + LIGHT_SKIP_MS;
    bool on = lights[idx].is_on;
    if (lights[idx].tile)
        lv_obj_set_style_bg_color(lights[idx].tile,
            lv_color_hex(on ? accent : C_TILE_OFF), 0);
    if (lights[idx].lbl)
        lv_obj_set_style_text_color(lights[idx].lbl,
            lv_color_hex(on ? 0x000000 : C_TXT_OFF), 0);
    if (lights[idx].icon_lbl)
        lv_obj_set_style_text_color(lights[idx].icon_lbl,
            lv_color_hex(on ? 0x000000 : accent_d), 0);
    String body = String("{\"entity_id\":\"") + lights[idx].eid + "\"}";
    ha_call(lights[idx].domain, on ? "turn_on" : "turn_off", body);
}

static void light_long_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= LC || !lights[idx].dimmable) return;
    show_dim_popup(idx);
}

// ── AC callbacks (generic, index passed via user_data) ─
static void ac_pwr_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= AC_COUNT) return;
    bool off = (strcmp(acs[idx].mode, "off") == 0);
    String body = String("{\"entity_id\":\"") + acs[idx].eid + "\"}";
    ha_call("climate", off ? "turn_on" : "turn_off", body);
}

static void ac_up_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= AC_COUNT) return;
    if (acs[idx].tgt < acs[idx].max_temp) acs[idx].tgt++;
    if (acs[idx].lbl_tgt) lv_label_set_text_fmt(acs[idx].lbl_tgt, "%d", (int)acs[idx].tgt);
    String body = String("{\"entity_id\":\"") + acs[idx].eid +
                  "\",\"temperature\":" + String((int)acs[idx].tgt) + "}";
    ha_call("climate", "set_temperature", body);
}

static void ac_dn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= AC_COUNT) return;
    if (acs[idx].tgt > acs[idx].min_temp) acs[idx].tgt--;
    if (acs[idx].lbl_tgt) lv_label_set_text_fmt(acs[idx].lbl_tgt, "%d", (int)acs[idx].tgt);
    String body = String("{\"entity_id\":\"") + acs[idx].eid +
                  "\",\"temperature\":" + String((int)acs[idx].tgt) + "}";
    ha_call("climate", "set_temperature", body);
}

// ═══════════════════════════════════════════════════════
//  UI helpers
// ═══════════════════════════════════════════════════════
static lv_obj_t *mkbtn(lv_obj_t *p, const char *txt, lv_coord_t w,
                        lv_coord_t h, lv_event_cb_t cb, void *ud=nullptr,
                        uint32_t bg=C_BTN) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD), 0);
    lv_obj_center(l);
    return b;
}

// ═══════════════════════════════════════════════════════
//  PAGE 1 — Lights  (scrollable, categorised)
// ═══════════════════════════════════════════════════════
static void build_lights_page(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 4, 0);

    if (LC == 0) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No lights configured.\nUse HACS panel to add entities.");
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    for (int c = 0; c < CAT_COUNT; c++) {
        // Category header
        lv_obj_t *hdr = lv_label_create(parent);
        lv_label_set_text(hdr, dyn_cats[c].name);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(C_GOLD_D), 0);
        lv_obj_set_style_pad_left(hdr, 4, 0);

        // Row container
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, 5, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        for (int j = 0; j < dyn_cats[c].count; j++) {
            int i = dyn_cats[c].start + j;
            lv_obj_t *tile = lv_obj_create(row);
            lv_obj_set_size(tile, 110, 72);
            lv_obj_set_style_bg_color(tile, lv_color_hex(C_TILE_OFF), 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(C_PRESS), LV_STATE_PRESSED);
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 1, 0);
            lv_obj_set_style_border_color(tile, lv_color_hex(C_GOLD_D), 0);
            lv_obj_set_style_shadow_width(tile, 0, 0);
            lv_obj_set_style_pad_all(tile, 6, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_add_event_cb(tile, light_tap_cb, LV_EVENT_SHORT_CLICKED,
                                (void*)(intptr_t)i);
            lv_obj_add_event_cb(tile, light_long_cb, LV_EVENT_LONG_PRESSED,
                                (void*)(intptr_t)i);

            lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            // Icon
            lv_obj_t *ic = lv_label_create(tile);
            bool use_fa = false;
            if (strcmp(lights[i].icon, "fan") == 0)         { lv_label_set_text(ic, FA_FAN); use_fa=true; }
            else if (strcmp(lights[i].icon, "alarm") == 0)  { lv_label_set_text(ic, LV_SYMBOL_BELL); }
            else if (strcmp(lights[i].icon, "socket") == 0) { lv_label_set_text(ic, FA_PLUG); use_fa=true; }
            else if (strcmp(lights[i].icon, "tube") == 0)   { lv_label_set_text(ic, LV_SYMBOL_BARS); }
            else if (strcmp(lights[i].icon, "warm") == 0)   { lv_label_set_text(ic, LV_SYMBOL_TINT); }
            else                                            { lv_label_set_text(ic, FA_LIGHTBULB); use_fa=true; }
            lv_obj_set_style_text_font(ic, use_fa ? &fa_icons_16 : &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(C_GOLD_D), 0);

            // Name
            lv_obj_t *nm = lv_label_create(tile);
            lv_label_set_text(nm, lights[i].label);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT_OFF), 0);

            lights[i].tile = tile;
            lights[i].lbl  = nm;
            lights[i].icon_lbl = ic;
        }
    }
}

// ═══════════════════════════════════════════════════════
//  PAGE 2 — Climate
// ═══════════════════════════════════════════════════════
static void build_climate_page(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    // ── Temp/Humidity arcs ─────────────────────────────
    if (temp_sensor_eid[0] || hum_sensor_eid[0]) {
        lv_obj_t *ar = lv_obj_create(parent);
        lv_obj_set_size(ar, lv_pct(100), 150);
        lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(ar, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_border_width(ar, 0, 0);
        lv_obj_set_style_radius(ar, 14, 0);
        lv_obj_clear_flag(ar, LV_OBJ_FLAG_SCROLLABLE);

        if (temp_sensor_eid[0]) {
            arc_temp = lv_arc_create(ar);
            lv_obj_set_size(arc_temp, 120, 120);
            lv_arc_set_rotation(arc_temp, 135);
            lv_arc_set_bg_angles(arc_temp, 0, 270);
            lv_arc_set_range(arc_temp, 150, 500);
            lv_arc_set_value(arc_temp, 300);
            lv_obj_remove_style(arc_temp, NULL, LV_PART_KNOB);
            lv_obj_clear_flag(arc_temp, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(arc_temp, 8, LV_PART_MAIN);
            lv_obj_set_style_arc_color(arc_temp, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc_temp, 8, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc_temp, lv_color_hex(C_GOLD), LV_PART_INDICATOR);

            lbl_tempv = lv_label_create(arc_temp);
            lv_label_set_text(lbl_tempv, "--");
            lv_obj_set_style_text_font(lbl_tempv, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(lbl_tempv, lv_color_hex(C_GOLD_B), 0);
            lv_obj_center(lbl_tempv);
            lv_obj_t *tl = lv_label_create(arc_temp);
            lv_label_set_text(tl, "TEMP");
            lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(tl, lv_color_hex(C_DIM), 0);
            lv_obj_align(tl, LV_ALIGN_CENTER, 0, 16);
        }

        if (hum_sensor_eid[0]) {
            arc_hum = lv_arc_create(ar);
            lv_obj_set_size(arc_hum, 120, 120);
            lv_arc_set_rotation(arc_hum, 135);
            lv_arc_set_bg_angles(arc_hum, 0, 270);
            lv_arc_set_range(arc_hum, 0, 100);
            lv_arc_set_value(arc_hum, 50);
            lv_obj_remove_style(arc_hum, NULL, LV_PART_KNOB);
            lv_obj_clear_flag(arc_hum, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(arc_hum, 8, LV_PART_MAIN);
            lv_obj_set_style_arc_color(arc_hum, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc_hum, 8, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc_hum, lv_color_hex(C_GOLD_D), LV_PART_INDICATOR);

            lbl_humv = lv_label_create(arc_hum);
            lv_label_set_text(lbl_humv, "--%");
            lv_obj_set_style_text_font(lbl_humv, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(lbl_humv, lv_color_hex(C_GOLD), 0);
            lv_obj_center(lbl_humv);
            lv_obj_t *hl = lv_label_create(arc_hum);
            lv_label_set_text(hl, "HUMID");
            lv_obj_set_style_text_font(hl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(hl, lv_color_hex(C_DIM), 0);
            lv_obj_align(hl, LV_ALIGN_CENTER, 0, 16);
        }
    }

    // ── AC cards (dynamic) ─────────────────────────────
    if (AC_COUNT == 0) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No ACs configured.\nUse HACS panel to add.");
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < AC_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, lv_pct(100), 100);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Name
        lv_obj_t *nm = lv_label_create(card);
        // Convert label to uppercase for display
        char upper[24];
        strlcpy(upper, acs[i].label, sizeof(upper));
        for (char *c = upper; *c; c++) *c = toupper(*c);
        lv_label_set_text(nm, upper);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_GOLD), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);

        // Power button
        void *ud = (void*)(intptr_t)i;
        lv_obj_t *pwr = mkbtn(card, LV_SYMBOL_POWER, 56, 42, ac_pwr_cb, ud);
        lv_obj_align(pwr, LV_ALIGN_TOP_RIGHT, 0, -6);
        acs[i].btn_pwr = pwr;

        // Mode label
        acs[i].lbl_mode = lv_label_create(card);
        lv_label_set_text(acs[i].lbl_mode, "--");
        lv_obj_set_style_text_font(acs[i].lbl_mode, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(acs[i].lbl_mode, lv_color_hex(C_GOLD_B), 0);
        lv_obj_align(acs[i].lbl_mode, LV_ALIGN_LEFT_MID, 0, 2);

        // Target temp
        acs[i].lbl_tgt = lv_label_create(card);
        lv_label_set_text(acs[i].lbl_tgt, "--");
        lv_obj_set_style_text_font(acs[i].lbl_tgt, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(acs[i].lbl_tgt, lv_color_hex(C_WHITE), 0);
        lv_obj_align(acs[i].lbl_tgt, LV_ALIGN_CENTER, 0, 2);

        // Temp +/- buttons
        lv_obj_t *bdn = mkbtn(card, LV_SYMBOL_MINUS, 42, 34, ac_dn_cb, ud);
        lv_obj_align(bdn, LV_ALIGN_CENTER, -60, 2);
        lv_obj_t *bup = mkbtn(card, LV_SYMBOL_PLUS, 42, 34, ac_up_cb, ud);
        lv_obj_align(bup, LV_ALIGN_CENTER, 60, 2);

        // Info line
        acs[i].lbl_info = lv_label_create(card);
        lv_label_set_text(acs[i].lbl_info, "");
        lv_obj_set_style_text_font(acs[i].lbl_info, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(acs[i].lbl_info, lv_color_hex(C_DIM), 0);
        lv_obj_align(acs[i].lbl_info, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

// ═══════════════════════════════════════════════════════
//  PAGE — Media  (media player cards)
// ═══════════════════════════════════════════════════════

static void media_play_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MEDIA_COUNT) return;
    bool playing = (strcmp(media_ents[idx].state, "playing") == 0);
    String body = String("{\"entity_id\":\"") + media_ents[idx].eid + "\"}";
    ha_call("media_player", playing ? "media_pause" : "media_play", body);
}

static void media_prev_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MEDIA_COUNT) return;
    String body = String("{\"entity_id\":\"") + media_ents[idx].eid + "\"}";
    ha_call("media_player", "media_previous_track", body);
}

static void media_next_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MEDIA_COUNT) return;
    String body = String("{\"entity_id\":\"") + media_ents[idx].eid + "\"}";
    ha_call("media_player", "media_next_track", body);
}

static void media_vol_cb(lv_event_t *e) {
    lv_obj_t *sld = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MEDIA_COUNT) return;
    int vol = lv_slider_get_value(sld);
    media_ents[idx].volume = vol;
    float vf = (float)vol / 100.0f;
    char body[128];
    snprintf(body, sizeof(body),
             "{\"entity_id\":\"%s\",\"volume_level\":%.2f}",
             media_ents[idx].eid, vf);
    ha_call("media_player", "volume_set", String(body));
}

static void build_media_page(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 8, 0);

    if (MEDIA_COUNT == 0) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No media players configured.\nUse HACS panel to add.");
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    for (int i = 0; i < MEDIA_COUNT; i++) {
        // Card container
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(accent_d), 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 6, 0);

        // Name + state row
        lv_obj_t *hdr = lv_obj_create(card);
        lv_obj_set_width(hdr, lv_pct(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(hdr);
        lv_label_set_text(name, media_ents[i].label);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(accent), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *st = lv_label_create(hdr);
        lv_label_set_text(st, "OFF");
        lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(st, lv_color_hex(C_DIM), 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
        media_ents[i].lbl_state = st;

        // Title
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, "\xE2\x80\x94");  // em-dash
        lv_obj_set_width(title, lv_pct(100));
        lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(C_WHITE), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        media_ents[i].lbl_title = title;

        // Controls row: prev | play/pause | next
        lv_obj_t *ctrls = lv_obj_create(card);
        lv_obj_set_width(ctrls, lv_pct(100));
        lv_obj_set_height(ctrls, 40);
        lv_obj_set_style_bg_opa(ctrls, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ctrls, 0, 0);
        lv_obj_set_style_pad_all(ctrls, 0, 0);
        lv_obj_clear_flag(ctrls, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(ctrls, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ctrls, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ctrls, 12, 0);

        mkbtn(ctrls, LV_SYMBOL_PREV, 50, 34, media_prev_cb, (void*)(intptr_t)i);
        lv_obj_t *bp = mkbtn(ctrls, LV_SYMBOL_PLAY, 60, 34, media_play_cb, (void*)(intptr_t)i);
        media_ents[i].btn_play = bp;
        mkbtn(ctrls, LV_SYMBOL_NEXT, 50, 34, media_next_cb, (void*)(intptr_t)i);

        // Volume slider
        lv_obj_t *sld = lv_slider_create(card);
        lv_obj_set_width(sld, lv_pct(90));
        lv_obj_set_height(sld, 8);
        lv_slider_set_range(sld, 0, 100);
        lv_slider_set_value(sld, 50, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sld, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sld, lv_color_hex(accent), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sld, lv_color_hex(accent_b), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sld, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(sld, media_vol_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
        media_ents[i].sld_vol = sld;

        media_ents[i].card = card;
    }
}

// ═══════════════════════════════════════════════════════
//  PAGE — Presence  (person tiles: home / away)
// ═══════════════════════════════════════════════════════
static void build_presence_page(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    if (PERSON_COUNT == 0) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No persons configured.\nUse HACS panel to add.");
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, "WHO'S HOME");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(accent_d), 0);
    lv_obj_set_style_pad_left(hdr, 4, 0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 5, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < PERSON_COUNT; i++) {
        lv_obj_t *tile = lv_obj_create(row);
        lv_obj_set_size(tile, 110, 72);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_TILE_OFF), 0);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(accent_d), 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t *nm = lv_label_create(tile);
        lv_label_set_text(nm, person_ents[i].label);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT_OFF), 0);

        lv_obj_t *st = lv_label_create(tile);
        lv_label_set_text(st, "...");
        lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(st, lv_color_hex(C_DIM), 0);

        person_ents[i].tile = tile;
        person_ents[i].lbl  = nm;
        person_ents[i].lbl_state = st;
    }
}

// ═══════════════════════════════════════════════════════
//  PAGE 3 — Sensors
// ═══════════════════════════════════════════════════════
static void build_sensors_page(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    if (DC == 0 && MC == 0) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No sensors configured.\nUse HACS panel to add.");
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    // ── Door sensors ───────────────────────────────────
    if (DC > 0) {
        lv_obj_t *hdr = lv_label_create(parent);
        lv_label_set_text(hdr, "DOOR SENSORS");
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(C_GOLD_D), 0);
        lv_obj_set_style_pad_left(hdr, 4, 0);

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, 5, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < DC; i++) {
            lv_obj_t *tile = lv_obj_create(row);
            lv_obj_set_size(tile, 110, 72);
            lv_obj_set_style_bg_color(tile, lv_color_hex(C_TILE_OFF), 0);
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 1, 0);
            lv_obj_set_style_border_color(tile, lv_color_hex(C_GOLD_D), 0);
            lv_obj_set_style_shadow_width(tile, 0, 0);
            lv_obj_set_style_pad_all(tile, 6, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            lv_obj_t *ic = lv_label_create(tile);
            lv_label_set_text(ic, LV_SYMBOL_HOME);
            lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(C_GOLD_D), 0);

            lv_obj_t *nm = lv_label_create(tile);
            lv_label_set_text(nm, doors[i].label);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT_OFF), 0);

            doors[i].tile = tile;
            doors[i].lbl  = nm;
            doors[i].icon_lbl = ic;
        }
    }

    // ── Motion sensors ─────────────────────────────────
    if (MC > 0) {
        lv_obj_t *mhdr = lv_label_create(parent);
        lv_label_set_text(mhdr, "MOTION SENSORS");
        lv_obj_set_style_text_font(mhdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(mhdr, lv_color_hex(C_GOLD_D), 0);
        lv_obj_set_style_pad_left(mhdr, 4, 0);

        lv_obj_t *mrow = lv_obj_create(parent);
        lv_obj_set_width(mrow, lv_pct(100));
        lv_obj_set_height(mrow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(mrow, 0, 0);
        lv_obj_set_style_pad_gap(mrow, 5, 0);
        lv_obj_set_style_bg_opa(mrow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mrow, 0, 0);
        lv_obj_clear_flag(mrow, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < MC; i++) {
            lv_obj_t *tile = lv_obj_create(mrow);
            lv_obj_set_size(tile, 110, 72);
            lv_obj_set_style_bg_color(tile, lv_color_hex(C_TILE_OFF), 0);
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 1, 0);
            lv_obj_set_style_border_color(tile, lv_color_hex(C_GOLD_D), 0);
            lv_obj_set_style_shadow_width(tile, 0, 0);
            lv_obj_set_style_pad_all(tile, 6, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            lv_obj_t *ic = lv_label_create(tile);
            lv_label_set_text(ic, LV_SYMBOL_EYE_OPEN);
            lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(C_GOLD_D), 0);

            lv_obj_t *nm = lv_label_create(tile);
            lv_label_set_text(nm, motions[i].label);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT_OFF), 0);

            motions[i].tile = tile;
            motions[i].lbl  = nm;
            motions[i].icon_lbl = ic;
        }
    }
}

// ═══════════════════════════════════════════════════════
//  "Waiting for config" screen
// ═══════════════════════════════════════════════════════
static lv_obj_t *waitScr = nullptr;

static void build_wait_screen() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    waitScr = lv_obj_create(scr);
    lv_obj_set_size(waitScr, 480, 480);
    lv_obj_set_pos(waitScr, 0, 0);
    lv_obj_set_style_bg_color(waitScr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(waitScr, 0, 0);
    lv_obj_clear_flag(waitScr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(waitScr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(waitScr, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *t1 = lv_label_create(waitScr);
    lv_label_set_text(t1, "IOT EVERYTHIN");
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(C_GOLD), 0);

    lv_obj_t *t2 = lv_label_create(waitScr);
    lv_label_set_text(t2, "Waiting for configuration...");
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_pad_top(t2, 16, 0);

    lv_obj_t *t3 = lv_label_create(waitScr);
    String ip = WiFi.localIP().toString();
    String msg = "Open HACS panel in HA\nDisplay IP: " + ip + ":8080";
    lv_label_set_text(t3, msg.c_str());
    lv_obj_set_style_text_font(t3, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t3, lv_color_hex(C_GOLD_D), 0);
    lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(t3, 16, 0);
}

// ═══════════════════════════════════════════════════════
//  Build the full tabview UI from loaded config
// ═══════════════════════════════════════════════════════
static lv_obj_t *mainTV = nullptr;
static lv_obj_t *tabBtns = nullptr;  // tab button strip for accent recolor

static void build_main_ui() {
    // Remove the wait screen if present
    if (waitScr) { lv_obj_del(waitScr); waitScr = nullptr; }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    mainTV = lv_tabview_create(scr, LV_DIR_TOP, 38);
    lv_obj_set_style_bg_color(mainTV, lv_color_hex(C_BG), 0);

    tabBtns = lv_tabview_get_tab_btns(mainTV);
    lv_obj_set_style_text_font(tabBtns, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(tabBtns, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(tabBtns, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_color(tabBtns, lv_color_hex(accent),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tabBtns, lv_color_hex(accent),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *p1 = lv_tabview_add_tab(mainTV, "Switches");
    lv_obj_t *p2 = lv_tabview_add_tab(mainTV, "Climate");

    // Tab 3 — configurable: sensors (default), media, or presence
    const char *t3_label = "Sensors";
    if (strcmp(tab3_type, "media") == 0) t3_label = "Media";
    else if (strcmp(tab3_type, "presence") == 0) t3_label = "Presence";
    lv_obj_t *p3 = lv_tabview_add_tab(mainTV, t3_label);

    lv_obj_set_style_bg_color(p1, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_color(p2, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_color(p3, lv_color_hex(C_BG), 0);

    build_lights_page(p1);
    build_climate_page(p2);
    if (strcmp(tab3_type, "media") == 0) build_media_page(p3);
    else if (strcmp(tab3_type, "presence") == 0) build_presence_page(p3);
    else build_sensors_page(p3);

    lbl_status = lv_label_create(scr);
    lv_label_set_text(lbl_status, "Loading...");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(C_DIM), 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -2);

    Serial.println("[HA] Main UI built from config");
}

// ═══════════════════════════════════════════════════════
//  Low-battery accent recolor (gold → red at ≤10%)
// ═══════════════════════════════════════════════════════
static void apply_accent_colors() {
    // Tab bar
    if (tabBtns) {
        lv_obj_set_style_text_color(tabBtns, lv_color_hex(accent),
                                    LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(tabBtns, lv_color_hex(accent),
                                      LV_PART_ITEMS | LV_STATE_CHECKED);
    }
    // Light tiles — borders + "on" state colors
    for (int i = 0; i < LC; i++) {
        if (lights[i].tile) {
            lv_obj_set_style_border_color(lights[i].tile,
                lv_color_hex(accent_d), 0);
            bool on = lights[i].is_on;
            lv_obj_set_style_bg_color(lights[i].tile,
                lv_color_hex(on ? accent : C_TILE_OFF), 0);
        }
        if (lights[i].icon_lbl) {
            bool on = lights[i].is_on;
            lv_obj_set_style_text_color(lights[i].icon_lbl,
                lv_color_hex(on ? 0x000000 : accent_d), 0);
        }
    }
    // Door tiles
    for (int i = 0; i < DC; i++) {
        if (doors[i].tile) {
            lv_obj_set_style_border_color(doors[i].tile,
                lv_color_hex(accent_d), 0);
            bool open = doors[i].is_open;
            lv_obj_set_style_bg_color(doors[i].tile,
                lv_color_hex(open ? accent : C_TILE_OFF), 0);
        }
        if (doors[i].icon_lbl) {
            bool open = doors[i].is_open;
            lv_obj_set_style_text_color(doors[i].icon_lbl,
                lv_color_hex(open ? 0x000000 : accent_d), 0);
        }
    }
    // Motion tiles
    for (int i = 0; i < MC; i++) {
        if (motions[i].tile) {
            lv_obj_set_style_border_color(motions[i].tile,
                lv_color_hex(accent_d), 0);
            bool on = motions[i].active;
            lv_obj_set_style_bg_color(motions[i].tile,
                lv_color_hex(on ? accent : C_TILE_OFF), 0);
        }
        if (motions[i].icon_lbl) {
            bool on = motions[i].active;
            lv_obj_set_style_text_color(motions[i].icon_lbl,
                lv_color_hex(on ? 0x000000 : accent_d), 0);
        }
    }
    // Climate arcs
    if (arc_temp)
        lv_obj_set_style_arc_color(arc_temp, lv_color_hex(accent), LV_PART_INDICATOR);
    if (lbl_tempv)
        lv_obj_set_style_text_color(lbl_tempv, lv_color_hex(accent_b), 0);
    if (arc_hum)
        lv_obj_set_style_arc_color(arc_hum, lv_color_hex(accent_d), LV_PART_INDICATOR);
    if (lbl_humv)
        lv_obj_set_style_text_color(lbl_humv, lv_color_hex(accent), 0);
    // AC power buttons
    for (int i = 0; i < AC_COUNT; i++) {
        if (acs[i].btn_pwr) {
            bool on = (strcmp(acs[i].mode, "off") != 0);
            lv_obj_set_style_bg_color(acs[i].btn_pwr,
                lv_color_hex(on ? accent : C_BTN), 0);
        }
    }
    Serial.printf("[HA] Accent recolored: %s\n", _lowBat ? "RED (low bat)" : "GOLD");
}

static void check_low_battery() {
    uint32_t now = millis();
    if (now - _lastBatCheckMs < 5000) return;  // check every 5s
    _lastBatCheckMs = now;

    uint8_t pct = display_get_battery_pct();
    bool charging = display_is_charging();
    bool wasLow = _lowBat;

    // Go red at ≤10% on battery; restore gold when charging or >15% (hysteresis)
    if (!charging && pct <= 10) {
        _lowBat = true;
    } else if (charging || pct > 15) {
        _lowBat = false;
    }

    if (_lowBat != wasLow) {
        accent   = _lowBat ? C_RED   : C_GOLD;
        accent_b = _lowBat ? C_RED_B : C_GOLD_B;
        accent_d = _lowBat ? C_RED_D : C_GOLD_D;
        apply_accent_colors();
    }
}

// ═══════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════
void ha_create_ui() {
    load_config_credentials();

    // Allocate entity arrays in PSRAM
    lights      = (LightEnt*)ps_calloc(MAX_LIGHTS, sizeof(LightEnt));
    acs         = (ACEnt*)ps_calloc(MAX_ACS, sizeof(ACEnt));
    doors       = (DoorEnt*)ps_calloc(MAX_DOORS, sizeof(DoorEnt));
    motions     = (MotionEnt*)ps_calloc(MAX_MOTION, sizeof(MotionEnt));
    media_ents  = (MediaEnt*)ps_calloc(MAX_MEDIA, sizeof(MediaEnt));
    person_ents = (PersonEnt*)ps_calloc(MAX_PERSONS, sizeof(PersonEnt));

    // Try to load config from Preferences
    cfg_loaded = load_entities_from_prefs();

    // Create mutex
    haMutex = xSemaphoreCreateMutex();

    if (cfg_loaded) {
        build_main_ui();
        // Start background fetch
        xTaskCreatePinnedToCore(fetchTask, "ha_fetch", 16384, nullptr, 1,
                                &fetchTaskH, 0);
    } else {
        build_wait_screen();
        // Fetch task still created — will start once cfg_loaded becomes true
        xTaskCreatePinnedToCore(fetchTask, "ha_fetch", 16384, nullptr, 1,
                                &fetchTaskH, 0);
    }

    Serial.printf("[HA] UI created (configured=%s)\n", cfg_loaded ? "yes" : "no");
}

void ha_loop() {
    update_ui();
    check_low_battery();

    // Check if config was pushed while we were showing wait screen
    if (!cfg_loaded && config_needs_rebuild()) {
        Serial.println("[HA] Config received! Building UI...");
        load_config_credentials();
        cfg_loaded = load_entities_from_prefs();
        if (cfg_loaded) {
            build_main_ui();
        }
    }
    // Check if config was updated while UI is already showing
    else if (cfg_loaded && config_needs_rebuild()) {
        Serial.println("[HA] Config updated! Rebuilding UI...");
        // Clean up old UI
        if (mainTV) { lv_obj_del(mainTV); mainTV = nullptr; }
        if (lbl_status) { lv_obj_del(lbl_status); lbl_status = nullptr; }
        // Reset widget pointers
        arc_temp = arc_hum = nullptr;
        lbl_tempv = lbl_humv = nullptr;
        // Reload config and rebuild
        load_config_credentials();
        load_entities_from_prefs();
        build_main_ui();
    }
}
