"""WattLedger: energy history from an ESPHome WattLedger device."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.storage import Store

from .api import WattLedgerClient
from .const import DOMAIN
from .coordinator import STORAGE_VERSION, WattLedgerCoordinator

PLATFORMS = [Platform.SENSOR]

type WattLedgerConfigEntry = ConfigEntry[WattLedgerCoordinator]


async def async_setup_entry(hass: HomeAssistant, entry: WattLedgerConfigEntry) -> bool:
    client = WattLedgerClient(
        async_get_clientsession(hass), entry.data[CONF_HOST], entry.data[CONF_PORT]
    )
    coordinator = WattLedgerCoordinator(hass, entry, client)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: WattLedgerConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def async_remove_entry(hass: HomeAssistant, entry: WattLedgerConfigEntry) -> None:
    # The imported statistics stay; adding the device again continues them.
    await Store(hass, STORAGE_VERSION, f"{DOMAIN}.{entry.entry_id}").async_remove()
