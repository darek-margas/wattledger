"""Config flow for WattLedger."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.selector import (
    TextSelector,
    TextSelectorConfig,
    TextSelectorType,
)

from .api import WattLedgerAuthError, WattLedgerClient, WattLedgerError
from .const import DEFAULT_PORT, DOMAIN

CREDENTIALS = {
    vol.Optional(CONF_USERNAME): str,
    vol.Optional(CONF_PASSWORD): TextSelector(
        TextSelectorConfig(type=TextSelectorType.PASSWORD)
    ),
}

STEP_USER_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST): str,
        vol.Optional(CONF_PORT, default=DEFAULT_PORT): vol.All(
            int, vol.Range(min=1, max=65535)
        ),
        **CREDENTIALS,
    }
)

STEP_REAUTH_SCHEMA = vol.Schema(CREDENTIALS)


class WattLedgerConfigFlow(ConfigFlow, domain=DOMAIN):
    """Adds a WattLedger device by host name or IP address."""

    VERSION = 1

    async def _probe(self, data: Mapping[str, Any]) -> tuple[dict[str, Any] | None, str | None]:
        """Reads the device's info; returns it, or the form error."""
        client = WattLedgerClient(
            async_get_clientsession(self.hass),
            data[CONF_HOST],
            data[CONF_PORT],
            data.get(CONF_USERNAME),
            data.get(CONF_PASSWORD),
        )
        try:
            info = await client.info()
        except WattLedgerAuthError:
            return None, "invalid_auth"
        except WattLedgerError:
            return None, "cannot_connect"
        if "tiers" not in info or not info["tiers"]:
            return None, "no_journal"
        return info, None

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            info, error = await self._probe(user_input)
            if error:
                errors["base"] = error
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

    async def async_step_reauth(
        self, entry_data: Mapping[str, Any]
    ) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            data = {
                CONF_HOST: entry.data[CONF_HOST],
                CONF_PORT: entry.data[CONF_PORT],
                CONF_USERNAME: user_input.get(CONF_USERNAME, ""),
                CONF_PASSWORD: user_input.get(CONF_PASSWORD, ""),
            }
            _, error = await self._probe(data)
            if error:
                errors["base"] = error
            else:
                return self.async_update_reload_and_abort(entry, data_updates=data)
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=self.add_suggested_values_to_schema(
                STEP_REAUTH_SCHEMA,
                user_input or {CONF_USERNAME: entry.data.get(CONF_USERNAME, "")},
            ),
            description_placeholders={"host": entry.data[CONF_HOST]},
            errors=errors,
        )
