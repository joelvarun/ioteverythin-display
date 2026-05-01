/**
 * wifi_mgr.cpp — WiFi Manager with QR-code provisioning
 * ══════════════════════════════════════════════════════
 * On boot:
 *   1. Try stored SSID/pass from NVS (Preferences).
 *   2. If that fails → start SoftAP + captive-portal web page.
 *   3. Display QR code on screen to connect phone to the AP.
 *   4. User opens 192.168.4.1, picks SSID, enters password.
 *   5. Credentials saved to NVS → reboot → connects.
 *
 * The QR code encodes: WIFI:T:WPA;S:<AP_SSID>;P:<AP_PASSWORD>;;
 * so you scan it with your phone camera to join the setup AP.
 */

#include "wifi_mgr.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>

// ── State ──────────────────────────────────────────────
static wifi_state_t _state = WIFI_ST_DISCONNECTED;
static Preferences prefs;
static WebServer *server = nullptr;
static DNSServer *dns    = nullptr;

static String storedSSID;
static String storedPass;

// ── Static IP config ─────────────────────────────────
static String staticIP;
static String staticGW;
static String staticSN;
static String staticDNS;

static void load_static_ip() {
    prefs.begin("wifi", true);
    staticIP  = prefs.getString("s_ip",  "");
    staticGW  = prefs.getString("s_gw",  "");
    staticSN  = prefs.getString("s_sn",  "255.255.255.0");
    staticDNS = prefs.getString("s_dns", "");
    prefs.end();
}

static bool apply_static_ip() {
    if (staticIP.length() == 0) return false;

    IPAddress ip, gw, sn, d;
    if (!ip.fromString(staticIP))  return false;
    if (!gw.fromString(staticGW)) return false;
    if (!sn.fromString(staticSN)) sn = IPAddress(255,255,255,0);
    if (!d.fromString(staticDNS)) d  = gw;  // fallback DNS = gateway

    WiFi.config(ip, gw, sn, d);
    Serial.printf("[WIFI] Static IP configured: %s  GW: %s\n",
                  staticIP.c_str(), staticGW.c_str());
    return true;
}

// ── LVGL widgets ───────────────────────────────────────
static lv_obj_t *lbl_status  = nullptr;
static lv_obj_t *lbl_ip      = nullptr;
static lv_obj_t *qr_code     = nullptr;
static lv_obj_t *btn_reset   = nullptr;
static lv_obj_t *wifi_parent = nullptr;

// ── Forward declarations ───────────────────────────────
static void start_ap_mode();
static void stop_ap_mode();
static void update_ui();

// ── QR helper ──────────────────────────────────────────
static void show_qr(lv_obj_t *parent) {
    if (qr_code) { lv_obj_del(qr_code); qr_code = nullptr; }

    String qrData = String("WIFI:T:WPA;S:") + AP_SSID + ";P:" + AP_PASSWORD + ";;";

    qr_code = lv_qrcode_create(parent, 160,
                                lv_color_black(), lv_color_white());
    if (!qr_code) {
        Serial.println("[WIFI] QR code create FAILED");
        return;
    }
    lv_res_t res = lv_qrcode_update(qr_code, qrData.c_str(), qrData.length());
    if (res != LV_RES_OK) {
        Serial.println("[WIFI] QR code update FAILED");
    }
    lv_obj_set_style_border_color(qr_code, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr_code, 5, 0);
}

// ── Captive portal HTML ────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Touch-i WiFi Setup</title>
<style>
body{font-family:sans-serif;background:#000;color:#E8E8E8;margin:0;padding:20px;text-align:center}
h2{color:#D4A017}
input,select{width:90%;padding:12px;margin:8px 0;border:none;border-radius:8px;font-size:16px;background:#1A1A0A;color:#E8E8E8}
button{background:#D4A017;color:#000;padding:14px 40px;border:none;border-radius:8px;font-size:18px;cursor:pointer;margin-top:12px;font-weight:bold}
button:hover{background:#FFD700}
.card{background:#0D0D0D;border:1px solid #6B5310;border-radius:12px;padding:20px;max-width:400px;margin:auto}
label{color:#D4A017}
</style></head><body>
<div class="card">
<h2>Touch-i WiFi Setup</h2>
<form action="/save" method="POST">
<label>Network:</label><br>
<select name="ssid" id="ssid"></select><br>
<label>Password:</label><br>
<input type="password" name="pass" placeholder="WiFi Password"><br>
<hr style="border-color:#333;margin:16px 0">
<label>Static IP (leave blank for DHCP):</label><br>
<input type="text" name="sip" placeholder="e.g. 192.168.68.105"><br>
<input type="text" name="sgw" placeholder="Gateway (e.g. 192.168.68.1)"><br>
<input type="text" name="ssn" placeholder="Subnet (default 255.255.255.0)"><br>
<input type="text" name="sdns" placeholder="DNS (default = gateway)"><br>
<button type="submit">Connect</button>
</form>
</div>
<script>
fetch('/scan').then(r=>r.json()).then(nets=>{
  let s=document.getElementById('ssid');
  nets.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';s.appendChild(o);});
});
</script>
</body></html>
)rawliteral";

// ── Web server handlers ────────────────────────────────
static void handleRoot() {
    server->send(200, "text/html", PORTAL_HTML);
}

static void handleScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server->send(200, "application/json", json);
}

static void handleSave() {
    storedSSID = server->arg("ssid");
    storedPass = server->arg("pass");

    // Save to NVS
    prefs.begin("wifi", false);
    prefs.putString("ssid", storedSSID);
    prefs.putString("pass", storedPass);
    prefs.end();

    // Save static IP settings
    String sip  = server->arg("sip");
    String sgw  = server->arg("sgw");
    String ssn  = server->arg("ssn");
    String sdns = server->arg("sdns");
    wifi_mgr_set_static_ip(
        sip.c_str(), sgw.c_str(),
        ssn.length() > 0 ? ssn.c_str() : "255.255.255.0",
        sdns.c_str());

    server->send(200, "text/html",
        "<html><body style='background:#000;color:#E8E8E8;text-align:center;padding:40px'>"
        "<h2 style='color:#D4A017'>Saved!</h2>"
        "<p>Connecting to " + storedSSID + "...</p>"
        "<p>Display will restart.</p></body></html>");

    delay(2000);
    ESP.restart();
}

static void handleNotFound() {
    // Captive portal — serve portal page directly (no redirect)
    // iOS captive portal sheet doesn't follow redirects reliably
    server->send(200, "text/html", PORTAL_HTML);
}

// ── AP mode ────────────────────────────────────────────
static void start_ap_mode() {
    Serial.println("[WIFI] Starting AP mode for provisioning");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
    delay(100);

    // DNS redirect all to us (captive portal)
    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());

    // Web server
    server = new WebServer(80);
    server->on("/",     HTTP_GET,  handleRoot);
    server->on("/scan", HTTP_GET,  handleScan);
    server->on("/save", HTTP_POST, handleSave);
    // Apple / Google / Microsoft captive-portal detection URLs
    // Serve our portal HTML so the phone opens the setup page
    server->on("/hotspot-detect.html",        HTTP_GET, handleRoot);
    server->on("/library/test/success.html",  HTTP_GET, handleRoot);
    server->on("/generate_204",               HTTP_GET, handleRoot);
    server->on("/gen_204",                    HTTP_GET, handleRoot);
    server->on("/connecttest.txt",            HTTP_GET, handleRoot);
    server->on("/redirect",                   HTTP_GET, handleRoot);
    server->on("/ncsi.txt",                   HTTP_GET, handleRoot);
    server->onNotFound(handleNotFound);
    server->begin();

    _state = WIFI_ST_AP_MODE;
    Serial.printf("[WIFI] AP active: %s / %s\n", AP_SSID, AP_PASSWORD);
    Serial.printf("[WIFI] Portal at http://%s/\n",
                  WiFi.softAPIP().toString().c_str());
}

static void stop_ap_mode() {
    if (server) { server->stop(); delete server; server = nullptr; }
    if (dns)    { dns->stop();    delete dns;    dns = nullptr; }
    WiFi.softAPdisconnect(true);
}

// ── Reset button callback ──────────────────────────────
static void btn_reset_cb(lv_event_t *e) {
    Serial.println("[WIFI] User pressed reset — clearing credentials");
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
}

// ── UI update ──────────────────────────────────────────
static void update_ui() {
    if (!lbl_status) return;

    switch (_state) {
        case WIFI_ST_DISCONNECTED:
            lv_label_set_text(lbl_status, LV_SYMBOL_WARNING " Disconnected");
            lv_label_set_text(lbl_ip, "");
            break;
        case WIFI_ST_CONNECTING:
            lv_label_set_text(lbl_status, LV_SYMBOL_REFRESH " Connecting...");
            lv_label_set_text(lbl_ip, storedSSID.c_str());
            break;
        case WIFI_ST_CONNECTED:
            lv_label_set_text(lbl_status, LV_SYMBOL_WIFI " Connected");
            lv_label_set_text_fmt(lbl_ip, "%s\n%s",
                                  WiFi.SSID().c_str(),
                                  WiFi.localIP().toString().c_str());
            if (qr_code) { lv_obj_add_flag(qr_code, LV_OBJ_FLAG_HIDDEN); }
            break;
        case WIFI_ST_AP_MODE:
            lv_label_set_text(lbl_status, LV_SYMBOL_WIFI " Setup Mode");
            lv_label_set_text_fmt(lbl_ip,
                "Scan QR to join \"%s\"\nThen open 192.168.4.1", AP_SSID);
            if (qr_code) { lv_obj_clear_flag(qr_code, LV_OBJ_FLAG_HIDDEN); }
            break;
    }
}

// ── Public API ────────────────────────────────────────
void wifi_mgr_init() {
    // Load stored credentials
    prefs.begin("wifi", true);
    storedSSID = prefs.getString("ssid", "");
    storedPass = prefs.getString("pass", "");
    prefs.end();

    // Load static IP config
    load_static_ip();

    if (storedSSID.length() > 0) {
        Serial.printf("[WIFI] Stored SSID: %s — connecting...\n", storedSSID.c_str());
        WiFi.mode(WIFI_STA);
        apply_static_ip();
        WiFi.begin(storedSSID.c_str(), storedPass.c_str());
        _state = WIFI_ST_CONNECTING;

        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            _state = WIFI_ST_CONNECTED;
            Serial.printf("[WIFI] Connected! IP: %s\n",
                          WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[WIFI] Stored credentials failed → AP mode");
            WiFi.disconnect();
            start_ap_mode();
        }
    } else {
        Serial.println("[WIFI] No stored credentials → AP mode");
        start_ap_mode();
    }
}

void wifi_mgr_loop() {
    if (_state == WIFI_ST_AP_MODE) {
        if (dns)    dns->processNextRequest();
        if (server) server->handleClient();
    }

    // Auto-reconnect
    if (_state == WIFI_ST_CONNECTED && WiFi.status() != WL_CONNECTED) {
        _state = WIFI_ST_CONNECTING;
        WiFi.reconnect();
    }
    if (_state == WIFI_ST_CONNECTING && WiFi.status() == WL_CONNECTED) {
        _state = WIFI_ST_CONNECTED;
    }

    update_ui();
}

void wifi_mgr_create_tab(lv_obj_t *parent) {
    wifi_parent = parent;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(parent, 12, 0);
    lv_obj_set_style_pad_gap(parent, 10, 0);

    // Status card
    lv_obj_t *status_card = lv_obj_create(parent);
    lv_obj_set_size(status_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_color(status_card, lv_color_hex(0x6B5310), 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_radius(status_card, 12, 0);
    lv_obj_set_style_pad_all(status_card, 16, 0);
    lv_obj_set_style_pad_gap(status_card, 6, 0);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_status = lv_label_create(status_card);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);

    lbl_ip = lv_label_create(status_card);
    lv_obj_set_style_text_align(lbl_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ip, lv_color_hex(0xD4A017), 0);

    // QR code (shown in AP mode)
    show_qr(parent);
    if (_state != WIFI_ST_AP_MODE && qr_code) {
        lv_obj_add_flag(qr_code, LV_OBJ_FLAG_HIDDEN);
    }

    // Reset WiFi button
    btn_reset = lv_btn_create(parent);
    lv_obj_set_size(btn_reset, 200, 48);
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0x1A1A0A), 0);
    lv_obj_set_style_radius(btn_reset, 10, 0);
    lv_obj_add_event_cb(btn_reset, btn_reset_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn_reset);
    lv_label_set_text(bl, LV_SYMBOL_TRASH " Reset WiFi");
    lv_obj_center(bl);

    update_ui();
}

wifi_state_t wifi_mgr_state() { return _state; }
bool wifi_mgr_connected()     { return _state == WIFI_ST_CONNECTED; }

void wifi_mgr_set_static_ip(const char *ip, const char *gateway,
                             const char *subnet, const char *dns_addr) {
    Preferences p;
    p.begin("wifi", false);
    p.putString("s_ip",  ip      ? ip      : "");
    p.putString("s_gw",  gateway ? gateway : "");
    p.putString("s_sn",  subnet  ? subnet  : "255.255.255.0");
    p.putString("s_dns", dns_addr? dns_addr: "");
    p.end();

    staticIP  = ip       ? ip       : "";
    staticGW  = gateway  ? gateway  : "";
    staticSN  = subnet   ? subnet   : "255.255.255.0";
    staticDNS = dns_addr ? dns_addr : "";

    Serial.printf("[WIFI] Static IP saved: %s (restart to apply)\n",
                  staticIP.length() > 0 ? staticIP.c_str() : "DHCP");
}

bool wifi_mgr_has_static_ip() {
    return staticIP.length() > 0;
}
