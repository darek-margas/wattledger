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
| `components/rv1805` | ESPHome time platform for the Micro Crystal RV-1805 RTC (SparkFun Qwiic RTC board, supercapacitor backup) |
| `packages/vue3-wattledger.yaml` | Vue 3 base: 8 MB flash, journal partition, SNTP time, no reboot while Home Assistant is down |
| `packages/vue3-rtc.yaml` | Optional: RV-1805 RTC, for units with one soldered in |
| `examples/` | Complete node configurations for the grid and generator Vues |
| `docs/design.md` | Design |

Planned: `components/wattledger` (journal, outputs, PVOutput uploader, cost)
and `custom_components/wattledger` (Home Assistant integration).

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

## RTC component

```yaml
external_components:
  - source: github://darek-margas/wattledger@main
    components: [rv1805]

time:
  - platform: rv1805
    id: rtc_time
  - platform: sntp
    on_time_sync:
      then:
        - lambda: id(rtc_time).write_time();
```

- At boot, sets the system clock from the RTC, unless the RTC reports that it
  lost its backup power or was never set.
- `id(rtc_time).write_time()` stores the system time; call it after every
  SNTP sync.
- `id(rtc_time).read_time()` reads it again; this also happens every
  `update_interval` (default 15 min).
- `trickle_charge` (default `true`) charges the supercapacitor.

## Building the examples

```bash
cp examples/secrets.yaml.example examples/secrets.yaml
esphome compile examples/vue3-grid.yaml
```
