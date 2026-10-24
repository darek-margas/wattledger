"""Diagnostics download: device info and import state."""

from __future__ import annotations

from typing import Any

from homeassistant.core import HomeAssistant

from . import WattLedgerConfigEntry


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: WattLedgerConfigEntry
) -> dict[str, Any]:
    coordinator = entry.runtime_data
    ledger = coordinator.ledger
    return {
        "info": coordinator.data.info if coordinator.data else None,
        "import_state": ledger.to_state() if ledger else None,
    }
