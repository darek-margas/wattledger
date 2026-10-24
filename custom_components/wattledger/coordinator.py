"""Pulls journal rows from the device and imports hourly energy statistics."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
import logging
from typing import Any

from homeassistant.components.recorder import get_instance
from homeassistant.components.recorder.models import (
    StatisticData,
    StatisticMeanType,
    StatisticMetaData,
)
from homeassistant.components.recorder.statistics import (
    async_add_external_statistics,
    get_last_statistics,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import UnitOfEnergy
from homeassistant.core import HomeAssistant
from homeassistant.helpers.storage import Store
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from homeassistant.util import slugify

from .api import WattLedgerClient, WattLedgerError
from .const import DOMAIN, MAX_REQUESTS_PER_UPDATE, ROWS_PER_REQUEST, UPDATE_INTERVAL
from .ledger import HOUR, Channel, HourlyLedger, channels

_LOGGER = logging.getLogger(__name__)

STORAGE_VERSION = 1


@dataclass
class LedgerStatus:
    """What the diagnostic sensors show."""

    info: dict[str, Any]
    imported_until: datetime | None
    backlog: int
    unplaced: int


class WattLedgerCoordinator(DataUpdateCoordinator[LedgerStatus]):
    """Imports the device's 1-minute journal into external statistics.

    Each input and output becomes two statistics (import and export, kWh),
    `wattledger:<device>_<id>_<direction>`, usable in the Energy dashboard.
    They come from the device's own records, so Home Assistant downtime (up
    to the device's 1-minute retention, 48 h by default) leaves no gap.
    """

    config_entry: ConfigEntry

    def __init__(
        self, hass: HomeAssistant, entry: ConfigEntry, client: WattLedgerClient
    ) -> None:
        super().__init__(
            hass,
            _LOGGER,
            config_entry=entry,
            name=DOMAIN,
            update_interval=UPDATE_INTERVAL,
        )
        self.client = client
        self.store: Store[dict[str, Any]] = Store(
            hass, STORAGE_VERSION, f"{DOMAIN}.{entry.entry_id}"
        )
        self.ledger: HourlyLedger | None = None

    def statistic_id(self, info: dict[str, Any], channel: Channel) -> str:
        return f"{DOMAIN}:{slugify(info['name'])}_{slugify(channel.key)}"

    async def _async_update_data(self) -> LedgerStatus:
        try:
            info = await self.client.info()
        except WattLedgerError as err:
            raise UpdateFailed(str(err)) from err
        tier = info.get("tiers", {}).get("min1")
        if tier is None:
            raise UpdateFailed("The device's journal isn't available")

        if self.ledger is None:
            self.ledger = HourlyLedger(await self.store.async_load())
            if self.ledger.released_until is None:
                await self._continue_existing_statistics(info)
        ledger = self.ledger

        if ledger.cursor is None or ledger.cursor > tier["next_seq"]:
            # First run, or the device started a new journal.
            ledger.cursor = tier["first_seq"]
        elif ledger.cursor < tier["first_seq"]:
            _LOGGER.warning(
                "%s: %d one-minute records were overwritten before they could be "
                "imported (Home Assistant was away longer than the device keeps them)",
                info["name"],
                tier["first_seq"] - ledger.cursor,
            )
            ledger.cursor = tier["first_seq"]

        try:
            await self._read_rows(info, ledger)
        except WattLedgerError as err:
            raise UpdateFailed(str(err)) from err
        self._import(info, ledger.release())
        await self.store.async_save(ledger.to_state())

        return LedgerStatus(
            info=info,
            imported_until=(
                datetime.fromtimestamp(ledger.released_until, UTC)
                if ledger.released_until
                else None
            ),
            backlog=max(0, tier["next_seq"] - ledger.cursor),
            unplaced=ledger.unplaced,
        )

    async def _read_rows(self, info: dict[str, Any], ledger: HourlyLedger) -> None:
        for _ in range(MAX_REQUESTS_PER_UPDATE):
            page = await self.client.rows("min1", ledger.cursor, ROWS_PER_REQUEST)
            for row in page["rows"]:
                if not ledger.add_row(row, info):
                    # Waiting for this boot's first valid time.
                    ledger.cursor = row[0]
                    return
                ledger.cursor = row[0] + 1
            # Skips sequence numbers the device couldn't read (damaged records).
            ledger.cursor = max(ledger.cursor, page["next_from_seq"])
            if ledger.cursor >= page["next_seq"] or not page["rows"]:
                return

    def _import(
        self, info: dict[str, Any], released: list[tuple[int, dict[str, float]]]
    ) -> None:
        if not released:
            return
        title = self.config_entry.title
        for channel in channels(info):
            stats = [
                StatisticData(
                    start=datetime.fromtimestamp(hour, UTC), sum=sums[channel.key]
                )
                for hour, sums in released
                if channel.key in sums
            ]
            if not stats:
                continue
            metadata = StatisticMetaData(
                mean_type=StatisticMeanType.NONE,
                has_sum=True,
                name=f"{title} {channel.name} {channel.direction}",
                source=DOMAIN,
                statistic_id=self.statistic_id(info, channel),
                unit_class="energy",
                unit_of_measurement=UnitOfEnergy.KILO_WATT_HOUR,
            )
            async_add_external_statistics(self.hass, metadata, stats)

    async def _continue_existing_statistics(self, info: dict[str, Any]) -> None:
        """Continues statistics this device already has (for example after the
        integration was removed and added again), instead of restarting sums."""
        ledger = self.ledger
        for channel in channels(info):
            statistic_id = self.statistic_id(info, channel)
            last = await get_instance(self.hass).async_add_executor_job(
                get_last_statistics, self.hass, 1, statistic_id, True, {"sum"}
            )
            if rows := last.get(statistic_id):
                ledger.sums[channel.key] = rows[0]["sum"] or 0.0
                end = int(rows[0]["start"]) + HOUR
                ledger.released_until = max(ledger.released_until or 0, end)
                ledger.skip_before = ledger.released_until
