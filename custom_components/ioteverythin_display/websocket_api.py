"""Touch-i by IoT Everythin — WebSocket API for frontend panel."""

from __future__ import annotations

from datetime import timedelta
import logging

import aiohttp
import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.network import get_url

from .const import DOMAIN, DEFAULT_PORT

_LOGGER = logging.getLogger(__name__)


@callback
def async_register_websocket_api(hass: HomeAssistant) -> None:
    """Register WebSocket commands for the display panel."""
    websocket_api.async_register_command(hass, ws_get_display_info)
    websocket_api.async_register_command(hass, ws_get_display_config)
    websocket_api.async_register_command(hass, ws_push_display_config)
    websocket_api.async_register_command(hass, ws_save_panel_config)
    websocket_api.async_register_command(hass, ws_load_panel_config)


@websocket_api.websocket_command(
    {
        vol.Required("type"): "ioteverythin_display/get_info",
    }
)
@websocket_api.async_response
async def ws_get_display_info(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
):
    """Get display device info."""
    display_url = hass.data.get(DOMAIN, {}).get("display_url")
    if not display_url:
        connection.send_error(msg["id"], "not_configured", "No display configured")
        return

    try:
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"{display_url}/api/info",
                timeout=aiohttp.ClientTimeout(total=5),
            ) as resp:
                if resp.status == 200:
                    info = await resp.json()
                    connection.send_result(msg["id"], info)
                else:
                    connection.send_error(
                        msg["id"], "device_error", f"HTTP {resp.status}"
                    )
    except (aiohttp.ClientError, TimeoutError) as err:
        connection.send_error(msg["id"], "connection_error", str(err))


@websocket_api.websocket_command(
    {
        vol.Required("type"): "ioteverythin_display/get_config",
    }
)
@websocket_api.async_response
async def ws_get_display_config(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
):
    """Get current display config from the device."""
    display_url = hass.data.get(DOMAIN, {}).get("display_url")
    if not display_url:
        connection.send_error(msg["id"], "not_configured", "No display configured")
        return

    try:
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"{display_url}/api/config",
                timeout=aiohttp.ClientTimeout(total=5),
            ) as resp:
                if resp.status == 200:
                    config = await resp.json()
                    connection.send_result(msg["id"], config)
                else:
                    connection.send_error(
                        msg["id"], "device_error", f"HTTP {resp.status}"
                    )
    except (aiohttp.ClientError, TimeoutError) as err:
        connection.send_error(msg["id"], "connection_error", str(err))


@websocket_api.websocket_command(
    {
        vol.Required("type"): "ioteverythin_display/push_config",
        vol.Required("config"): dict,
    }
)
@websocket_api.async_response
async def ws_push_display_config(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
):
    """Push entity config to the display device."""
    display_url = hass.data.get(DOMAIN, {}).get("display_url")
    if not display_url:
        connection.send_error(msg["id"], "not_configured", "No display configured")
        return

    config = msg["config"]

    # Inject HA URL and access token so the display can poll states
    config["ha_url"] = get_url(hass)

    # Get or create a long-lived access token for the display
    token = hass.data.get(DOMAIN, {}).get("ha_token", "")
    if not token:
        token = await _get_or_create_display_token(hass, connection.user)
        if token:
            hass.data.setdefault(DOMAIN, {})["ha_token"] = token
    config["ha_token"] = token
    _LOGGER.info("Pushing config with token length: %d", len(token))

    last_err = None
    for attempt in range(2):
        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(
                    f"{display_url}/api/config",
                    json=config,
                    timeout=aiohttp.ClientTimeout(total=30),
                ) as resp:
                    if resp.status == 200:
                        result = await resp.json()
                        connection.send_result(msg["id"], result)
                        return
                    else:
                        text = await resp.text()
                        last_err = f"HTTP {resp.status}: {text}"
        except (aiohttp.ClientError, TimeoutError) as err:
            last_err = str(err) or type(err).__name__
            _LOGGER.warning("Push attempt %d failed: %s", attempt + 1, last_err)

    connection.send_error(msg["id"], "connection_error", last_err or "Push failed")


@websocket_api.websocket_command(
    {
        vol.Required("type"): "ioteverythin_display/save_panel_config",
        vol.Required("config"): dict,
    }
)
@websocket_api.async_response
async def ws_save_panel_config(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
):
    """Save panel config to HA storage for persistence."""
    store = hass.data.get(DOMAIN, {}).get("store")
    if not store:
        connection.send_error(msg["id"], "no_store", "Storage not initialized")
        return
    await store.async_save(msg["config"])
    _LOGGER.info("Panel config saved to HA storage")
    connection.send_result(msg["id"], {"saved": True})


@websocket_api.websocket_command(
    {
        vol.Required("type"): "ioteverythin_display/load_panel_config",
    }
)
@websocket_api.async_response
async def ws_load_panel_config(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
):
    """Load panel config from HA storage."""
    store = hass.data.get(DOMAIN, {}).get("store")
    if not store:
        connection.send_error(msg["id"], "no_store", "Storage not initialized")
        return
    data = await store.async_load()
    connection.send_result(msg["id"], data or {})


async def _get_or_create_display_token(hass: HomeAssistant, user) -> str:
    """Create a long-lived access token for the display."""
    try:
        # Check if we already have a refresh token for the display
        for token in user.refresh_tokens.values():
            if token.client_name == "Touch-i Display":
                access_token = hass.auth.async_create_access_token(token)
                _LOGGER.info("Reusing existing display token")
                return access_token

        # Create new refresh + access token
        refresh_token = await hass.auth.async_create_refresh_token(
            user,
            client_name="Touch-i Display",
            token_type="long_lived_access_token",
            access_token_expiration=timedelta(days=3650),
        )
        access_token = hass.auth.async_create_access_token(refresh_token)
        _LOGGER.info("Created new display token for user %s", user.name)
        return access_token
    except Exception:
        _LOGGER.exception("Failed to create display access token")
        return ""
