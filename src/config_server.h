/**
 * config_server.h — HTTP config endpoint for IoT Everythin Display
 * ════════════════════════════════════════════════════════════════
 * Receives entity config from HA integration, stores in Preferences.
 * Also serves device info for discovery.
 */

#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

/// Start the config HTTP server on port 80 (call after WiFi connected).
void config_server_start();

/// Call from loop() to handle incoming requests.
void config_server_loop();

/// Returns true if a valid config has been received and stored.
bool config_available();

/// Trigger a UI rebuild from the stored config (sets a flag).
void config_request_rebuild();

/// Check and clear the rebuild flag.
bool config_needs_rebuild();

#endif // CONFIG_SERVER_H
