"""Diagnostic sensors for the statistics import."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import CONF_HOST, CONF_PORT, EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo, format_mac
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import WattLedgerConfigEntry
from .const import DOMAIN
from .coordinator import LedgerStatus, WattLedgerCoordinator


@dataclass(frozen=True, kw_only=True)
class LedgerSensorDescription(SensorEntityDescription):
    value: Callable[[LedgerStatus], Any]


SENSORS = (
    LedgerSensorDescription(
        key="imported_until",
        translation_key="imported_until",
        device_class=SensorDeviceClass.TIMESTAMP,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s: s.imported_until,
    ),
    LedgerSensorDescription(
        key="backlog",
        translation_key="backlog",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s: s.backlog,
    ),
    LedgerSensorDescription(
        key="unplaced",
        translation_key="unplaced",
        state_class=SensorStateClass.TOTAL_INCREASING,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda s: s.unplaced,
    ),
    LedgerSensorDescription(
        key="boot_id",
        translation_key="boot_id",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda s: s.info.get("boot_id"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: WattLedgerConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(LedgerSensor(coordinator, d) for d in SENSORS)


class LedgerSensor(CoordinatorEntity[WattLedgerCoordinator], SensorEntity):
    _attr_has_entity_name = True
    entity_description: LedgerSensorDescription

    def __init__(
        self, coordinator: WattLedgerCoordinator, description: LedgerSensorDescription
    ) -> None:
        super().__init__(coordinator)
        self.entity_description = description
        entry = coordinator.config_entry
        info = coordinator.data.info
        mac = format_mac(info["mac"])
        self._attr_unique_id = f"{mac}_{description.key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, mac)},
            name=entry.title,
            manufacturer="WattLedger",
            model="Energy ledger",
            sw_version=f"record format {info.get('version')}",
            configuration_url=f"http://{entry.data[CONF_HOST]}:{entry.data[CONF_PORT]}/",
        )

    @property
    def native_value(self) -> Any:
        return self.entity_description.value(self.coordinator.data)
