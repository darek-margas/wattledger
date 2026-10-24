"""Client for the device's history API (/wattledger/v1/)."""

from __future__ import annotations

from typing import Any

import aiohttp


class WattLedgerError(Exception):
    """The device couldn't be reached or answered unexpectedly."""


class WattLedgerClient:
    """Reads info and journal rows from a WattLedger device."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int) -> None:
        self._session = session
        self._base = f"http://{host}:{port}/wattledger/v1"

    async def _get(self, path: str, params: dict[str, Any] | None = None) -> Any:
        try:
            async with self._session.get(
                f"{self._base}/{path}",
                params=params,
                timeout=aiohttp.ClientTimeout(total=15),
            ) as resp:
                resp.raise_for_status()
                return await resp.json(content_type=None)
        except (aiohttp.ClientError, TimeoutError, ValueError) as err:
            raise WattLedgerError(f"{path}: {err}") from err

    async def info(self) -> dict[str, Any]:
        """Device, layout and journal ranges."""
        return await self._get("info")

    async def rows(self, tier: str, from_seq: int, limit: int) -> dict[str, Any]:
        """Records of a tier from `from_seq`, oldest first."""
        return await self._get(
            "rows", {"tier": tier, "from_seq": from_seq, "limit": limit}
        )
