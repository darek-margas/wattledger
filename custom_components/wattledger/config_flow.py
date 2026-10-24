"""Config flow for WattLedger."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import format_mac

from .api import WattLedgerClient, WattLedgerError
from .const import DEFAULT_PORT, DOMAIN

STEP_USER_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST): str,
        vol.Optional(CONF_PORT, default=DEFAULT_PORT): vol.All(
            int, vol.Range(min=1, max=65535)
        ),
    }
)


class WattLedgerConfigFlow(ConfigFlow, domain=DOMAIN):
    """Adds a WattLedger device by host name or IP address."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            client = WattLedgerClient(
                async_get_clientsession(self.hass),
                user_input[CONF_HOST],
                user_input[CONF_PORT],
            )
            try:
                info = await client.info()
            except WattLedgerError:
                errors["base"] = "cannot_connect"
            else:
                if "tiers" not in info or not info["tiers"]:
                    errors["base"] = "no_journal"
                else:
                    await self.async_set_unique_id(format_mac(info["mac"]))
                    self._abort_if_unique_id_configured(updates=user_input)
                    return self.async_create_entry(
                        title=info.get("friendly_name") or info["name"],
                        data=user_input,
                    )
        return self.async_show_form(
            step_id="user",
            data_schema=self.add_suggested_values_to_schema(
                STEP_USER_SCHEMA, user_input
            ),
            errors=errors,
        )
