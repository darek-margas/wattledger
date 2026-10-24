"""Turns journal rows into hourly energy totals.

Plain Python with no Home Assistant imports, so it can be tested on its own.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

HOUR = 3600
FLAG_UNTIMED = 1

# Interval row positions (README, "History API").
ROW_SEQ = 0
ROW_FLAGS = 1
ROW_BOOT_ID = 2
ROW_DEFS_HASH = 3
ROW_START_UNIX = 5
ROW_DURATION = 6
ROW_INPUTS = 8
ROW_OUTPUTS = 9


@dataclass(frozen=True)
class Channel:
    """One energy series: an input or output, import or export."""

    key: str
    kind: str  # "input" or "output"
    name: str
    direction: str  # "import" or "export"


def channels(info: dict[str, Any]) -> list[Channel]:
    """The device's energy series, in row order."""
    result = []
    for kind, names in (("input", info["inputs"]), ("output", info["outputs"])):
        for name in names:
            for direction in ("import", "export"):
                result.append(Channel(f"{name}_{direction}", kind, name, direction))
    return result


def for_each_bucket(start: int, duration: int, bucket: int):
    """Yields (bucket_start, fraction) for each `bucket`-aligned bucket that
    [start, start + duration) overlaps. Mirrors the device's for_each_bucket."""
    if duration <= 0:
        yield start - start % bucket, 1.0
        return
    end = start + duration
    t = start
    while t < end:
        b = t - t % bucket
        nxt = min(b + bucket, end)
        yield b, (nxt - t) / duration
        t = nxt


def row_energies(row: list, info: dict[str, Any]) -> dict[str, float]:
    """kWh per channel key in an interval row. Outputs are left out when the
    row was recorded under other output definitions (their meaning changed)."""
    energies: dict[str, float] = {}
    pairs = [("input", ROW_INPUTS, info["inputs"])]
    if row[ROW_DEFS_HASH] == info["defs_hash"]:
        pairs.append(("output", ROW_OUTPUTS, info["outputs"]))
    for _kind, pos, names in pairs:
        values = row[pos]
        for i, name in enumerate(names):
            if 2 * i + 1 >= len(values):
                break
            energies[f"{name}_import"] = values[2 * i] / 1e6
            energies[f"{name}_export"] = values[2 * i + 1] / 1e6
    return energies


class HourlyLedger:
    """Accumulates rows into hours and releases hours once complete.

    Rows arrive in sequence order, which is time order except around clock
    steps. An hour is complete once a row ends after it. Energy that arrives
    for an hour already released goes into the earliest open hour, so the
    released totals never change.
    """

    def __init__(self, state: dict[str, Any] | None = None) -> None:
        state = state or {}
        self.cursor: int | None = state.get("cursor")
        self.released_until: int | None = state.get("released_until")
        self.sums: dict[str, float] = dict(state.get("sums", {}))
        self.pending: dict[int, dict[str, float]] = {
            int(h): dict(v) for h, v in state.get("pending", {}).items()
        }
        self.latest_end: int = state.get("latest_end", 0)
        self.unplaced: int = state.get("unplaced", 0)
        # Rows ending by this time are already in the statistics (set when
        # continuing statistics imported before this state existed).
        self.skip_before: int = state.get("skip_before", 0)

    def to_state(self) -> dict[str, Any]:
        return {
            "cursor": self.cursor,
            "released_until": self.released_until,
            "sums": self.sums,
            "pending": {str(h): v for h, v in self.pending.items()},
            "latest_end": self.latest_end,
            "unplaced": self.unplaced,
            "skip_before": self.skip_before,
        }

    def add_row(self, row: list, info: dict[str, Any]) -> bool:
        """Adds an interval row. False if it can't be placed yet: an untimed
        row of the device's current boot, before its first valid time."""
        start = row[ROW_START_UNIX]
        if start == 0:
            if row[ROW_BOOT_ID] == info["boot_id"]:
                return False
            # A boot that never got a valid time: its energy can't be placed.
            self.unplaced += 1
            return True
        duration = row[ROW_DURATION]
        if start + duration <= self.skip_before:
            return True
        energies = row_energies(row, info)
        for hour, fraction in for_each_bucket(start, duration, HOUR):
            if self.released_until is not None and hour < self.released_until:
                hour = self.released_until
            bucket = self.pending.setdefault(hour, {})
            for key, kwh in energies.items():
                bucket[key] = bucket.get(key, 0.0) + kwh * fraction
        self.latest_end = max(self.latest_end, start + duration)
        return True

    def release(self) -> list[tuple[int, dict[str, float]]]:
        """Complete hours, oldest first, each with the cumulative sum per
        channel after that hour."""
        complete_before = self.latest_end - self.latest_end % HOUR
        released = []
        for hour in sorted(h for h in self.pending if h < complete_before):
            for key, kwh in self.pending.pop(hour).items():
                self.sums[key] = self.sums.get(key, 0.0) + kwh
            released.append((hour, dict(self.sums)))
            self.released_until = hour + HOUR
        return released
