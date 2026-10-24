# WattLedger

An ESPHome external component and a Home Assistant integration that turn an
ESPHome energy monitor (initially the Emporia Vue 3) into an autonomous meter
of record, as a replacement for the discontinued IoTaWatt.

With only mains power available, the device keeps measuring and recording,
and uploads to [PVOutput](https://pvoutput.org) directly when the internet is
back. Home Assistant gets live values and can fill gaps from the device's
history after downtime.

**Status:** early development, not yet tested on hardware. See
[docs/design.md](docs/design.md) for the design and milestones.

<img src="custom_components/wattledger/brand/logo.png" alt="WattLedger" height="64">

## Contents

| Path | What |
|---|---|
| `components/wattledger` | Sampler (output lambdas on every sample, import/export energy), flash journal of 1- and 5-minute intervals, HTTP history API, direct PVOutput uploads, cost and sealed days |
| `components/rv3028` | ESPHome time platform for the Micro Crystal RV-3028 RTC (PiicoDev RTC board, supercapacitor backup) |
| `components/rv1805` | Same for the RV-1805 (SparkFun Qwiic RTC board), as an alternative |
| `packages/vue3-wattledger.yaml` | Vue 3 base: 8 MB flash, journal partition, SNTP time, no reboot while Home Assistant is down |
| `packages/vue3-rtc.yaml` | Optional: RV-3028 RTC, for units with one soldered in |
| `examples/` | Complete node configurations for the grid and generator Vues |
| `custom_components/wattledger` | Home Assistant integration: imports the device's history into Energy dashboard statistics |
| `docs/design.md` | Design |
| `docs/brand` | Icon and logo sources |

Planned: monthly rollups and combined devices in the integration.

## Home Assistant integration

Install with HACS as a custom repository (category *Integration*), or copy
`custom_components/wattledger` into your configuration. Requires Home
Assistant 2026.3 or later.

Add it under **Settings → Devices & services → Add integration → WattLedger**
with the device's host name or IP address. It complements the ESPHome
integration, which still provides the live sensors.

- Every 5 minutes it reads new 1-minute records from the device and imports
  them as hourly statistics: for each input and output, an import and an
  export series in kWh, named `wattledger:<device>_<id>_import` /
  `_export`. Choose them in the **Energy dashboard** configuration.
- The statistics come from the device's own records, so Home Assistant
  downtime up to the device's 1-minute retention (48 h by default) leaves no
  gap. Records from before the device's clock was set are placed once it is;
  records from a boot that never got a valid time are counted as unplaced.
- Diagnostic sensors: statistics imported until, records waiting, unplaced
  intervals, boot number. A diagnostics download shows the device info and
  import state.
- If an output's lambda changes, records made with the old definition are
  imported for inputs only.
- Removing the integration keeps the statistics; adding the device again
  continues them.

## Outputs

```yaml
wattledger:
  inputs:                 # power sensors (W); their unfiltered values are used
    - id: mains
      sensor: line_1_power
    - id: solar
      sensor: cir1
  lines:                  # voltage sensors (V)
    - id: line_1
      sensor: line_1_voltage
  outputs:
    - id: generation
      lambda: |-
        return solar < -15.0f ? -solar : 0.0f;
      power:
        name: "Solar generation power"
      import_energy:
        name: "Solar generation"
```

- Inputs are passed to every output lambda as `float` parameters named by
  their `id`. Lambdas must only use those parameters, so history can be
  recalculated from stored inputs.
- Lambdas run on every sample (each driver update, about 220 ms on the Vue),
  so clamps and sign splits are exact. Positive results count as import,
  negative as export.
- Time when any input is missing or stale (`stale_timeout`, default 5 s) is a
  gap: no energy is added for it.
- `power` publishes the average over each `update_interval` (default 10 s);
  `import_energy` and `export_energy` are kWh since boot.

## Hardware

- Emporia Vue 3 flashed with ESPHome using
  [rosenrot00/emporiavue](https://github.com/rosenrot00/emporiavue) in I2C
  mode (stock SAMD09 firmware).
- Optional: a [PiicoDev RV-3028 RTC](https://core-electronics.com.au/piicodev-real-time-clock-rv3028.html)
  fitted inside the Vue on the existing I2C bus (SDA GPIO5, SCL GPIO18),
  powered from the 3.3 V and GND flashing pads. Its supercapacitor keeps time
  for about a week without power. Remove the PiicoDev connectors to keep it
  flat, and cut the board's I2C pull-up jumper (the Vue's bus has its own).
  Without an RTC, a boot with no NTP server reachable records untimed
  intervals until the first sync; data is only lost if power fails again
  before that.

## Journal

Every minute and every 5 minutes the ledger writes an interval record to its
own flash partition: energy per input and output (import and export, mWh),
average voltage per line, how much of the interval had valid samples, and
flags (untimed, partial, first after boot). With `time_id` set, intervals
start on wall-clock boundaries once the time is valid; before that they are
recorded against time since boot.

| Option | Default | |
|---|---|---|
| `time_id` | none | Time source for interval timestamps |
| `min1_retention` | `48h` | 1-minute records kept |
| `min5_retention` | `30d` | 5-minute records kept |
| `output_slots` | outputs + 4 | Room for outputs in each record |

The partition is sized from these and added automatically. Changing the
record layout (inputs, lines, `output_slots`) or the retention starts a new,
empty journal, so leave spare output slots for outputs you may add later.
Changing a lambda keeps the journal.

## History API

| Request | Returns |
|---|---|
| `GET /wattledger/v1/info` | Inputs, outputs, lines, definitions hash, boot id, time, and each tier's sequence range |
| `GET /wattledger/v1/rows?tier=min1&from_seq=N&limit=M` | Up to 60 records of `min1`, `min5` or `events`, oldest first, and `next_from_seq` |

An interval row is
`[seq, flags, boot_id, defs_hash, start, start_unix, duration_s, coverage_s, [input import/export mWh...], [output import/export mWh...], [line 0.1 V...], price_import, price_export, cost, credit]`.

- `flags`: 1 untimed, 2 partial (gaps), 16 cut short by a clock step,
  32 first after boot.
- `start` is as recorded: Unix time, or seconds since boot when untimed.
  `start_unix` is always Unix time: untimed records are placed through the
  first valid time of their boot. It is 0 when that boot never got a valid
  time (power failed again first); such records can't be placed.
- Untimed and clock-step records aren't aligned to minute boundaries; readers
  that need aligned buckets share their energy across the buckets they
  overlap, in proportion to time.

An event row is `[seq, type, boot_id, uptime_s, unix_s, a, b]`: type 1 boot,
2 first valid time of a boot, 3 clock step (`unix_s` new time, `a` expected
time). A jump of 2 s or more is a clock step: the open intervals end there and
new ones start.

## Cost and days

```yaml
sensor:
  - platform: homeassistant          # prices in currency per kWh
    id: import_price
    entity_id: sensor.electricity_import_price
  - platform: homeassistant
    id: export_price
    entity_id: sensor.electricity_export_price

wattledger:
  cost:
    output: grid                     # import is costed, export credited
    import_price: import_price
    export_price: export_price
    daily_supply_charge: 1.10
    max_price_age: 2h                # optional
    fallback_schedule:               # optional, while prices are unavailable
      - {from: "00:00", to: "15:00", import: 0.28, export: 0.05}
      - {from: "15:00", to: "21:00", import: 0.45, export: 0.05}
      - {from: "21:00", to: "24:00", import: 0.28, export: 0.05}
    cost_today: {name: "Electricity cost today", unit_of_measurement: AUD}
    credit_today: {name: "Electricity credit today", unit_of_measurement: AUD}
  day_retention: 730d
  seal_delay: 15min
```

- Cost and credit are accumulated on every sample from the output's import
  and export, so they're exact with dynamic prices and when the flow changes
  direction within an interval. Interval records carry them, and the price
  in effect.
- A price is live while Home Assistant is connected (and, with
  `max_price_age`, recent). Otherwise the fallback schedule is used if it
  covers the time, else the last known price; such intervals are flagged
  (4 stale, 8 schedule). With no price at all, nothing is costed.
- **Days**: after each local midnight (plus `seal_delay`), the day is sealed
  from its 5-minute records into a day record: energy per input and output,
  average voltage, cost, credit, the supply charge, coverage, and the time
  priced from a stale price or the schedule. Sealed days are kept for
  `day_retention` and never change. After an outage, the missed days are
  sealed from what the journal still holds.
- History API: `rows?tier=days` returns
  `[seq, flags, date, day_start, length_s, coverage_s, records, price_stale_s, price_schedule_s, defs_hash, [inputs], [outputs], [lines], cost, credit, supply_charge]`
  (money in 1e-6 currency units; flags: 1 partial, 2 mixed output definitions).
- The Home Assistant integration imports cost and credit as hourly statistics
  (`wattledger:<device>_cost` / `_credit`, in Home Assistant's currency).

## PVOutput

```yaml
wattledger:
  pvoutput:
    api_key: !secret pvoutput_api_key
    system_id: !secret pvoutput_system_id
    donor: false            # true: 100 statuses/request, 300 requests/h, 90 days
    generation: generation  # output ids; their import energy is uploaded
    consumption: consumption
    voltage: line_1         # line id
    reserve_requests: 10    # per hour, left for other clients of the key
    backlog: {name: "PVOutput backlog"}
    rate_limit_remaining: {name: "PVOutput requests left"}
    last_upload: {name: "PVOutput last upload"}
    replay_progress: {name: "PVOutput replay progress"}
    last_error: {name: "PVOutput status"}
    replay: {name: "PVOutput replay"}
```

- Uploads from the device itself, from the 5-minute journal, so it keeps
  working with Home Assistant down and catches up after an internet outage
  (up to the 5-minute retention, 30 days by default, within PVOutput's 14 or
  90 days).
- Each status reports energy since local midnight (`v1`, `v3`), average power
  (`v2`, `v4`) and average voltage (`v6`) at the end of its 5 minutes; the
  last status of a day is reported at 23:59. Set the PVOutput system's status
  interval to 5 minutes and the device's `timezone` to the system's.
- Uploading starts from the moment it's first enabled. **Replay** re-sends
  every complete day still in the journal, overwriting those days on
  PVOutput; live uploads keep priority.
- Upload positions survive reboots. A batch counts as sent only when
  PVOutput accepts it. Rate limits are read from PVOutput's headers; when the
  allowance is used up (or down to `reserve_requests`) uploads wait for the
  reset. Rejected batches (400) are skipped and reported in the status
  sensor; a bad key or system id pauses uploads for 30 minutes at a time.
- Don't upload the same PVOutput system from another uploader (such as an HA
  PVOutput integration) for the same fields: they would overwrite each other.

## RTC components

```yaml
external_components:
  - source: github://darek-margas/wattledger@main
    components: [rv3028]

time:
  - platform: sntp
    id: sntp_time
  - platform: rv3028      # or rv1805
    id: rtc_time
    write_from: sntp_time
```

- At boot, sets the system clock from the RTC, unless the RTC reports that it
  lost its backup power or was never set.
- The RV-3028 keeps UTC in its Unix-time counter. Its backup switchover and
  trickle charger settings are stored in its EEPROM (written only when they
  differ), because the chip reloads them from EEPROM every 24 h.
- `write_from` stores the time in the RTC after every sync of that time
  source. Alternatively call `id(rtc_time).write_time()` yourself.
- `id(rtc_time).read_time()` reads it again; this also happens every
  `update_interval` (default 15 min).
- `trickle_charge` (default `true`) charges the supercapacitor.

## Building the examples

```bash
cp examples/secrets.yaml.example examples/secrets.yaml
esphome compile examples/vue3-grid.yaml
```

## License

GPL-3.0-only, see [LICENSE](LICENSE). Firmware built with this component
also links the GPL-3.0
[rosenrot00/emporiavue](https://github.com/rosenrot00/emporiavue) driver.
The RTC drivers use register details from the SparkFun RV-1805 and PiicoDev
RV-3028 libraries (MIT).
