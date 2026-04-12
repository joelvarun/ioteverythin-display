"""IoT Everythin Display — Home Assistant Integration.

Registers a custom panel in the HA sidebar to configure
which entities appear on the ESP32 touch display.
Pushes config to the display over local HTTP.
"""

from __future__ import annotations

import logging
import os

from homeassistant.components.frontend import async_register_built_in_panel
from homeassistant.components.http import StaticPathConfig
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant

from .const import DOMAIN, DEFAULT_PORT

_LOGGER = logging.getLogger(__name__)

PANEL_URL = "/api/panel_custom/ioteverythin_display"


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up IoT Everythin Display from a config entry."""
    host = entry.data[CONF_HOST]
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = {
        "host": host,
        "port": DEFAULT_PORT,
    }

    # Register the custom frontend panel
    panel_dir = os.path.join(os.path.dirname(__file__), "www")
    await hass.http.async_register_static_paths(
        [StaticPathConfig(
            "/ioteverythin_display/panel.js",
            os.path.join(panel_dir, "panel.js"),
            False,
        )]
    )

    # Register a sidebar panel (sync despite the async_ prefix)
    async_register_built_in_panel(
        hass,
        component_name="custom",
        sidebar_title="IoT Display",
        sidebar_icon="mdi:tablet-dashboard",
        frontend_url_path="ioteverythin-display",
        config={
            "_panel_custom": {
                "name": "ioteverythin-display-panel",
                "module_url": "/ioteverythin_display/panel.js",
            }
        },
        require_admin=False,
    )

    # Store display endpoint info for the frontend panel to read
    hass.data[DOMAIN]["display_url"] = f"http://{host}:{DEFAULT_PORT}"

    # Register a websocket API for the panel to get display config
    from .websocket_api import async_register_websocket_api
    async_register_websocket_api(hass)

    _LOGGER.info("IoT Everythin Display set up: %s:%s", host, DEFAULT_PORT)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    hass.data[DOMAIN].pop(entry.entry_id, None)
    # Note: panel removal requires HA restart
    return True
