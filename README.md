# WattLedger

An ESPHome external component and a Home Assistant integration that turn an
ESPHome energy monitor (initially the Emporia Vue 3) into an autonomous meter
of record, as a replacement for the discontinued IoTaWatt.

With only mains power available, the device keeps measuring and recording,
and uploads to [PVOutput](https://pvoutput.org) directly when the internet is
back. Home Assistant gets live values and can fill gaps from the device's
history after downtime.

**Status:** early development. See [docs/design.md](docs/design.md) for the
design and milestones. Not usable yet beyond the RTC component.

## Contents

| Path | What |
|---|---|
| `components/wattledger` | Sampler (output lambdas on every sample, import/export energy), flash journal of 1- and 5-minute intervals, HTTP history API (PVOutput uploads and cost to come) |
| `components/rv1805` | ESPHome time platform for the Micro Crystal RV-1805 RTC (SparkFun Qwiic RTC board, supercapacitor backup) |
| `packages/vue3-wattledger.yaml` | Vue 3 base: 8 MB flash, journal partition, SNTP time, no reboot while Home Assistant is down |
| `packages/vue3-rtc.yaml` | Optional: RV-1805 RTC, for units with one soldered in |
| `examples/` | Complete node configurations for the grid and generator Vues |
| `docs/design.md` | Design |

Planned: the journal, PVOutput uploader and cost in `components/wattledger`,
and `custom_components/wattledger` (Home Assistant integration).

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
- Optional: a SparkFun RV-1805 Qwiic RTC fitted inside the Vue on the
  existing I2C bus (SDA GPIO5, SCL GPIO18), powered from the 3.3 V and GND
  flashing pads. Remove the Qwiic connectors and open the board's I2C pull-up
  jumper. Without it, a boot with no NTP server reachable records untimed
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
`[seq, flags, boot_id, defs_hash, start, duration_s, coverage_s, [input import/export mWh...], [output import/export mWh...], [line 0.1 V...], price_import, price_export, cost, credit]`.
An event row is `[seq, type, boot_id, uptime_s, unix_s, a, b]` (type 1 boot,
2 first valid time of a boot).

## RTC component

```yaml
external_components:
  - source: github://darek-margas/wattledger@main
    components: [rv1805]

time:
  - platform: sntp
    id: sntp_time
  - platform: rv1805
    id: rtc_time
    write_from: sntp_time
```

- At boot, sets the system clock from the RTC, unless the RTC reports that it
  lost its backup power or was never set.
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
