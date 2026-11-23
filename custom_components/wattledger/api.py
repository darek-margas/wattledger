"""Client for the device's history API (/wattledger/v1/)."""

from __future__ import annotations

from typing import Any

import aiohttp


class WattLedgerError(Exception):
    """The device couldn't be reached or answered unexpectedly."""


class WattLedgerAuthError(WattLedgerError):
    """The device's web server refused the credentials, or wants some."""


class WattLedgerClient:
    """Reads info and journal rows from a WattLedger device.

    The API is served by ESPHome's web_server, so it sits behind web_server's
    auth when that is set. Digest is tried first; Basic is only sent when the
    device asks for it, so a digest device never sees the password in clear.
    """

    def __init__(
        self,
        session: aiohttp.ClientSession,
        host: str,
        port: int,
        username: str | None = None,
        password: str | None = None,
    ) -> None:
        self._session = session
        self._base = f"http://{host}:{port}/wattledger/v1"
        self._username = username or None
        self._password = password or ""
        self._basic = False
        self._digest = (
            aiohttp.DigestAuthMiddleware(self._username, self._password)
            if self._username
            else None
        )

    async def _request(
        self, url: str, params: dict[str, Any] | None
    ) -> tuple[int, str, Any]:
        kwargs: dict[str, Any] = {
            "params": params,
            "timeout": aiohttp.ClientTimeout(total=15),
        }
        if self._basic:
            kwargs["auth"] = aiohttp.BasicAuth(self._username, self._password)
        elif self._digest is not None:
            kwargs["middlewares"] = (self._digest,)
        async with self._session.get(url, **kwargs) as resp:
            if resp.status == 401:
                return 401, resp.headers.get("WWW-Authenticate", ""), None
            resp.raise_for_status()
            return resp.status, "", await resp.json(content_type=None)

    async def _get(self, path: str, params: dict[str, Any] | None = None) -> Any:
        url = f"{self._base}/{path}"
        try:
            status, challenge, body = await self._request(url, params)
            if (
                status == 401
                and self._username
                and not self._basic
                and challenge.lower().startswith("basic")
            ):
                self._basic = True
                status, challenge, body = await self._request(url, params)
        except (aiohttp.ClientError, TimeoutError, ValueError) as err:
            raise WattLedgerError(f"{path}: {err}") from err
        if status == 401:
            raise WattLedgerAuthError(
                f"{path}: the web server "
                + ("refused the user name or password" if self._username else "needs a login")
            )
        return body

    async def info(self) -> dict[str, Any]:
        """Device, layout and journal ranges."""
        return await self._get("info")

    async def rows(self, tier: str, from_seq: int, limit: int) -> dict[str, Any]:
        """Records of a tier from `from_seq`, oldest first."""
        return await self._get(
            "rows", {"tier": tier, "from_seq": from_seq, "limit": limit}
        )
