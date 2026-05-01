/**
 * wifi_mgr.h — WiFi Manager with QR-code provisioning
 */

#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <lvgl.h>

/// Possible WiFi states
enum wifi_state_t {
    WIFI_ST_DISCONNECTED,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_AP_MODE       // provisioning AP active
};

/// Initialise WiFi — tries stored credentials, else starts AP + QR code.
void wifi_mgr_init();

/// Call from loop() — handles reconnection, provisioning web server.
void wifi_mgr_loop();

/// Create the WiFi settings tab content inside a parent container.
void wifi_mgr_create_tab(lv_obj_t *parent);

/// Get current state.
wifi_state_t wifi_mgr_state();

/// Is connected to a station?
bool wifi_mgr_connected();

/// Set static IP config and save to NVS. Pass empty ip to revert to DHCP.
void wifi_mgr_set_static_ip(const char *ip, const char *gateway,
                             const char *subnet, const char *dns);

/// Returns true if a static IP is configured.
bool wifi_mgr_has_static_ip();

#endif // WIFI_MGR_H
