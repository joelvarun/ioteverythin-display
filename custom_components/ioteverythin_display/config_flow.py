"""Touch-i by IoT Everythin — Config flow."""

from __future__ import annotations

import aiohttp
import voluptuous as vol

from homeassistant import config_entries
from homeassistant.const import CONF_HOST

from .const import DOMAIN, DEFAULT_PORT


class IotEverythinDisplayConfigFlow(
    config_entries.ConfigFlow, domain=DOMAIN
):
    """Handle a config flow for IoT Everythin Display."""

    VERSION = 1

    async def async_step_user(self, user_input=None):
        """Handle the initial step — user enters display IP."""
        errors = {}

        if user_input is not None:
            host = user_input[CONF_HOST].strip()

            # Validate by hitting the device's /api/info endpoint
            try:
                async with aiohttp.ClientSession() as session:
                    url = f"http://{host}:{DEFAULT_PORT}/api/info"
                    async with session.get(url, timeout=aiohttp.ClientTimeout(total=5)) as resp:
                        if resp.status == 200:
                            info = await resp.json()
                            device_name = info.get("name", "Touch-i")
                            mac = info.get("mac", host)

                            # Prevent duplicate entries for same device
                            await self.async_set_unique_id(mac)
                            self._abort_if_unique_id_configured()

                            return self.async_create_entry(
                                title=device_name,
                                data={CONF_HOST: host},
                            )
                        errors["base"] = "cannot_connect"
            except (aiohttp.ClientError, TimeoutError):
                errors["base"] = "cannot_connect"

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {vol.Required(CONF_HOST): str}
            ),
            errors=errors,
        )
