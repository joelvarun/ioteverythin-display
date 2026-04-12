"""IoT Everythin Display — WebSocket API for frontend panel."""

from __future__ import annotations

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

    # Inject HA URL and auto-generated token so the display can poll states
    config["ha_url"] = get_url(hass)
    config["ha_token"] = hass.data.get(DOMAIN, {}).get("ha_token", "")

    try:
        async with aiohttp.ClientSession() as session:
            async with session.post(
                f"{display_url}/api/config",
                json=config,
                timeout=aiohttp.ClientTimeout(total=10),
            ) as resp:
                if resp.status == 200:
                    result = await resp.json()
                    connection.send_result(msg["id"], result)
                else:
                    text = await resp.text()
                    connection.send_error(
                        msg["id"], "device_error", f"HTTP {resp.status}: {text}"
                    )
    except (aiohttp.ClientError, TimeoutError) as err:
        connection.send_error(msg["id"], "connection_error", str(err))
