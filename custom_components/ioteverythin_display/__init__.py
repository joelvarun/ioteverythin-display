"""IoT Everythin Display — Home Assistant Integration.

Registers a custom panel in the HA sidebar to configure
which entities appear on the ESP32 touch display.
Pushes config to the display over local HTTP.
"""

from __future__ import annotations

from datetime import timedelta
import logging
import os

from homeassistant.auth.const import TOKEN_TYPE_LONG_LIVED_ACCESS_TOKEN
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

    # Register the custom frontend panel (skip if already registered)
    panel_dir = os.path.join(os.path.dirname(__file__), "www")
    try:
        await hass.http.async_register_static_paths(
            [StaticPathConfig(
                "/ioteverythin_display/panel.js",
                os.path.join(panel_dir, "panel.js"),
                False,
            )]
        )
    except RuntimeError:
        _LOGGER.debug("Static path already registered, skipping")

    # Register a sidebar panel
    try:
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
    except ValueError:
        _LOGGER.debug("Panel already registered, skipping")

    # Store display endpoint info for the frontend panel to read
    hass.data[DOMAIN]["display_url"] = f"http://{host}:{DEFAULT_PORT}"

    # Auto-create a long-lived access token for the display
    await _ensure_access_token(hass, entry)

    # Register a websocket API for the panel to get display config
    from .websocket_api import async_register_websocket_api
    async_register_websocket_api(hass)

    _LOGGER.info("IoT Everythin Display set up: %s:%s", host, DEFAULT_PORT)
    return True


async def _ensure_access_token(
    hass: HomeAssistant, entry: ConfigEntry
) -> None:
    """Create or retrieve a long-lived access token for the display."""
    refresh_token_id = entry.data.get("refresh_token_id")
    refresh_token = None

    if refresh_token_id:
        refresh_token = hass.auth.async_get_refresh_token(refresh_token_id)

    if refresh_token is None:
        # Find owner or first active admin
        owner = await hass.auth.async_get_owner()
        if owner is None:
            users = await hass.auth.async_get_users()
            owner = next(
                (u for u in users if u.is_admin and u.is_active), None
            )
        if owner is None:
            _LOGGER.error("No admin user found — cannot create access token")
            return

        refresh_token = await hass.auth.async_create_refresh_token(
            owner,
            client_name="IoT Everythin Display",
            token_type=TOKEN_TYPE_LONG_LIVED_ACCESS_TOKEN,
            access_token_expiration=timedelta(days=3650),
        )
        # Persist refresh token ID so we can reuse / clean up later
        hass.config_entries.async_update_entry(
            entry, data={**entry.data, "refresh_token_id": refresh_token.id}
        )
        _LOGGER.info("Created long-lived token for IoT Everythin Display")

    # Generate a JWT access token and store it for the WS API
    access_token = hass.auth.async_create_access_token(refresh_token)
    hass.data[DOMAIN]["ha_token"] = access_token


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    # Clean up the long-lived token
    refresh_token_id = entry.data.get("refresh_token_id")
    if refresh_token_id:
        refresh_token = hass.auth.async_get_refresh_token(refresh_token_id)
        if refresh_token:
            hass.auth.async_remove_refresh_token(refresh_token)
            _LOGGER.info("Removed long-lived token for IoT Everythin Display")
    hass.data[DOMAIN].pop(entry.entry_id, None)
    # Note: panel removal requires HA restart
    return True
