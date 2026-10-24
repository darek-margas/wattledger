"""Tests for the integration's row-to-hour logic (no Home Assistant needed)."""

import importlib.util
from pathlib import Path
import sys

import pytest

# Load ledger.py directly: the package __init__ imports Home Assistant.
_PATH = Path(__file__).parents[2] / "custom_components" / "wattledger" / "ledger.py"
_SPEC = importlib.util.spec_from_file_location("wattledger_ledger", _PATH)
ledger = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = ledger  # dataclasses look the module up here
_SPEC.loader.exec_module(ledger)

INFO = {"inputs": ["mains"], "outputs": ["grid"], "defs_hash": 7, "boot_id": 3}


def row(seq, start_unix, duration=60, mains=(1000, 0), grid=(1000, 0), flags=0, boot=3, defs=7, start=None):
    return [
        seq,
        flags,
        boot,
        defs,
        start_unix if start is None else start,
        start_unix,
        duration,
        duration,
        list(mains),
        list(grid),
        [2400],
        0,
        0,
        0,
        0,
    ]


def test_channels_order():
    keys = [c.key for c in ledger.channels(INFO)]
    assert keys == ["mains_import", "mains_export", "grid_import", "grid_export"]


def test_hour_released_once_a_row_reaches_its_end():
    led = ledger.HourlyLedger()
    for i in range(59):
        led.add_row(row(i + 1, 7200 + 60 * i), INFO)
    assert led.release() == []
    led.add_row(row(60, 7200 + 60 * 59), INFO)  # ends at 10800
    released = led.release()
    assert [h for h, _ in released] == [7200]
    assert released[0][1]["mains_import"] == pytest.approx(0.06)  # 60 x 1 Wh
    assert led.released_until == 10800


def test_unaligned_row_is_split_between_hours():
    led = ledger.HourlyLedger()
    led.add_row(row(1, 7200 + 3600 - 30, mains=(600, 0)), INFO)  # 30 s each side
    led.add_row(row(2, 7200 + 2 * 3600), INFO)
    released = dict(led.release())
    assert released[7200]["mains_import"] == pytest.approx(0.0003)
    assert released[10800]["mains_import"] == pytest.approx(0.0006)


def test_untimed_row_of_current_boot_waits():
    led = ledger.HourlyLedger()
    assert not led.add_row(row(1, 0, flags=1, boot=3, start=100), INFO)


def test_untimed_row_of_old_boot_is_counted_and_skipped():
    led = ledger.HourlyLedger()
    assert led.add_row(row(1, 0, flags=1, boot=2, start=100), INFO)
    assert led.unplaced == 1


def test_outputs_skipped_under_other_definitions():
    energies = ledger.row_energies(row(1, 7200, defs=99), INFO)
    assert "grid_import" not in energies and "mains_import" in energies


def test_late_energy_goes_to_earliest_open_hour():
    led = ledger.HourlyLedger()
    led.add_row(row(1, 7200), INFO)
    led.add_row(row(2, 10800), INFO)
    led.release()
    # After a backward clock step, a row lands in the released hour.
    led.add_row(row(3, 7300), INFO)
    led.add_row(row(4, 14400), INFO)
    released = dict(led.release())
    assert released[10800]["mains_import"] == pytest.approx(0.003)  # 1 + 1 (moved) + 1 (cumulative)


def test_rows_already_in_statistics_are_skipped():
    led = ledger.HourlyLedger({"released_until": 10800, "skip_before": 10800, "sums": {"mains_import": 5.0}})
    led.add_row(row(1, 7200), INFO)  # already imported before
    led.add_row(row(2, 10800), INFO)
    led.add_row(row(3, 14400), INFO)
    released = dict(led.release())
    assert released[10800]["mains_import"] == pytest.approx(5.001)


def test_cost_and_credit_channels():
    info = dict(INFO, cost=True)
    assert [c.key for c in ledger.channels(info)][-2:] == ["cost", "credit"]
    r = row(1, 7200)
    r[13], r[14] = 250000, 50000  # 0.25 and 0.05 in 1e-6 currency units
    energies = ledger.row_energies(r, info)
    assert energies["cost"] == pytest.approx(0.25) and energies["credit"] == pytest.approx(0.05)
    assert "cost" not in ledger.row_energies(r, INFO)  # cost disabled on the device


def test_state_round_trip():
    led = ledger.HourlyLedger()
    led.cursor = 5
    led.add_row(row(1, 7200), INFO)
    again = ledger.HourlyLedger(led.to_state())
    assert again.cursor == 5
    assert again.pending == led.pending
