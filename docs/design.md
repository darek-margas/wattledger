# WattLedger design

WattLedger is an ESPHome external component and a companion Home Assistant
integration that turn an ESPHome energy monitor (initially the Emporia Vue 3)
into an autonomous meter of record, replacing a discontinued IoTaWatt.

Status: draft, before any code. Sections marked **Open** need verification on
hardware or against external documentation.

## 1. Goals

1. **Autonomous.** With only mains power available, the device keeps
   measuring, recording and, when the internet comes back, uploading. No
   dependency on Home Assistant, a VM, a cloud account or vendor firmware.
2. **Direct PVOutput upload** of 5-minute statuses with a persistent cursor,
   automatic catch-up, rate-limit-aware waiting and a manual replay of the
   whole retained history (30 days).
3. **HA gap filling.** A 1-minute history (48 h) lets the companion
   integration repair Home Assistant energy statistics after HA downtime.
4. **Correct derived values.** Outputs (calculated values) are defined as
   template lambdas and evaluated on every sample, so clamps and sign splits
   (export, self-consumption) are exact, as with IoTaWatt's integrators.
5. **Daily ledger with cost.** Each local day is sealed into a compact row with
   energy per input and output, import cost and export credit, and data
   quality counters. Retained for years on the device.
6. **No invented energy.** Gaps (power loss, meter communication failure,
   unknown time) are recorded as gaps, never interpolated silently.

### Non-goals

- Years of 1-minute or 5-second history on the device. HA (via external
  statistics) and PVOutput are the long-term archives; the device keeps a
  recovery buffer plus sealed daily rows.
- Runtime-editable output scripts. Outputs are compiled C++ lambdas; changing
  one means reflashing (see definitions hash, section 5.3).
- Monthly or yearly aggregation on the device. That is done by the
  integration from the sealed daily rows.

## 2. Deployment

Two Emporia Vue 3 units, both running ESPHome (rosenrot00/emporiavue driver,
I2C mode with stock SAMD09 firmware) plus WattLedger:

| Device | Voltage reference | CTs | PVOutput | Cost |
|---|---|---|---|---|
| **Grid Vue** | grid active and neutral | mains, solar inverter, grid-only circuits | uploads | yes |
| **Generator Vue** | output side of the two-pole changeover | the 4 changeover circuits | off | off |

- The generator has its own neutral, switched by the two-pole changeover. The
  Vue's voltage inputs share one neutral, so the changeover circuits can't be
  measured correctly by the grid Vue. A second Vue powered and referenced from
  the changeover output always sees the selected source, with no transformer
  and no tie between the two neutrals.
- **PVOutput consumption is grid side only.** It is derived from the grid
  Vue's mains and solar CTs. While the changeover selects the grid, the
  changeover circuits pass through the mains CTs and are therefore included;
  while on generator they are excluded. The two devices do not talk to each
  other. HA combines them (section 9).
- Wired Ethernet is preferred where the location allows, to remove Wi-Fi as a
  point of failure.
- An internal RTC is optional (section 6.1). Without one, untimed mode
  (6.2) covers boots with no NTP server reachable.

The component is hardware-agnostic: it consumes ESPHome power and voltage
sensors (section 4), so other monitors (CircuitSetup ATM90E32) can use it
later.

## 3. Component overview

```
 emporiavue driver ──► power/voltage sensors (calibrated, per CT / line)
                                │
                                ▼
                     ┌─────────────────────┐
                     │ Sampler             │ every driver publication
                     │  inputs → lambdas   │ (per-sample evaluation)
                     │  → accumulators     │
                     └─────────┬───────────┘
                               │ minute close
                               ▼
         ┌──────────────── Journal (flash partition) ────────────────┐
         │ 1-min ring (48 h) │ 5-min ring (30 d) │ day ring (2 y) │ events │
         └───────┬───────────────────┬────────────────────┬──────────┘
                 │                   │                    │
        HTTP history API      PVOutput uploader      HTTP history API
        (HA integration)      (cursors in NVS)       (HA integration)
```

ESPHome entities (live, for HA dashboards and automations): per-output power
and energy, voltage per line, uploader status, time source, journal health,
and the replay button.

## 4. Sampling and accumulation

### 4.1 Inputs

Each input is an ESPHome sensor in watts (one per CT, after the driver's
calibration and line assignment). Voltage lines are ESPHome sensors in volts.

```yaml
wattledger:
  inputs:
    - id: mains_a        # references an existing power sensor id
    - id: solar
    - id: hws
  lines:
    - id: line_1_voltage
```

**Sampling trigger.** The sampler runs once per driver publication cycle (the
driver publishes all CTs together at `metering_interval`, target 1 s). It
reads every input's current state and the elapsed monotonic time since the
previous sample, and integrates `P × dt`.

**Validity.** A sample is invalid if any input is NaN or has not updated for
more than 3 × the expected interval. Invalid time is not integrated; it is
counted as missing coverage in the current interval.

**Open:** confirm the emporiavue driver's per-CT publication interval and
whether all CTs publish in the same loop iteration. If the frame callback can
supply calibrated per-CT power, a later source adapter can sample at the frame
rate (about 220 ms) instead. The sampler is written against a small source
interface so that adapter is drop-in.

### 4.2 Per-direction accumulation

For every input, positive and negative energy are accumulated separately
(import and export). A single-input clamp (for example, solar generation
`max(0, solar)`) can therefore be recomputed exactly from stored data.

### 4.3 Outputs (template lambdas)

Outputs are C++ lambdas whose parameters are the inputs, by id:

```yaml
wattledger:
  outputs:
    - id: grid
      name: Grid
      lambda: return mains_a;
    - id: generation
      name: Solar generation
      # inverter standby draws ~0.5 A at PF ~0 at night; clamp it out
      lambda: return (solar > 15.0f) ? solar : 0.0f;
    - id: consumption
      name: Consumption
      lambda: return mains_a + std::max(solar, 0.0f);
```

- Codegen passes every input as a named `float` parameter (`process_lambda`
  with one parameter per input), and wraps the result in a
  `float(const float *inputs)` adapter for the sampler.
- Lambdas run on every valid sample. Each output's result is accumulated by
  direction like an input, so `grid` yields both import and export energy.
- Lambdas must be pure functions of their parameters: no `id(...)` access,
  no state. That is what allows recomputation from stored inputs (5.3).
- Each output also gets live ESPHome sensors: average power (W) per sample
  cycle and energy since boot (Wh, `total_increasing`; HA handles the reset
  on reboot). These are for dashboards only. HA's authoritative history comes
  from the journal (section 9).

### 4.4 Voltage

Per line, voltage × seconds is accumulated over valid time, so the record
holds the average voltage for the interval (used for PVOutput `v6`).

## 5. Journal

### 5.1 Records

All tiers share one binary layout, little-endian, fixed size per firmware
build. The layout is derived from the configuration at compile time.

| Field | Type | Notes |
|---|---|---|
| magic | u16 | |
| version | u8 | record format version |
| flags | u8 | see below |
| seq | u32 | per-tier sequence, strictly increasing |
| boot_id | u16 | increments every boot |
| defs_hash | u32 | hash of output definitions (5.3) |
| start | u32 | Unix seconds, or monotonic seconds since boot if `UNTIMED` |
| duration_s | u16 | length of the interval |
| coverage_s | u16 | seconds of valid samples within it |
| inputs | 2 × u32 per input | import, export in mWh |
| outputs | 2 × u32 per output | import, export in mWh |
| lines | u16 per line | average voltage, 0.1 V |
| price_import, price_export | f32 | price in effect (cost-enabled devices) |
| cost, credit | i32 | in 1e-6 currency units |
| crc32 | u32 | over the whole record |

mWh in u32 holds about 4.3 MWh per interval, far beyond a 5-minute (or even
daily) interval on a domestic supply. Storing interval energy instead of
lifetime totals avoids rollover; sequence numbers detect missing intervals.

Flags: `UNTIMED`, `PARTIAL` (coverage < duration), `PRICE_STALE`,
`PRICE_SCHEDULE`, `CLOCK_STEP` (interval cut short by a clock correction),
`FIRST_AFTER_BOOT`.

### 5.2 Tiers and sizing

| Tier | Interval | Retention | Records |
|---|---|---|---|
| min1 | 1 min | 48 h | 2,880 |
| min5 | 5 min | 30 d | 8,640 |
| day | 1 local day | 730 days | 730 |
| events | n/a | ring, 64 KB | time anchors, clock steps, errors |

Example grid Vue: 19 CT inputs, 3 lines, 10 outputs.
Record = 22 header + 152 inputs + 80 outputs + 6 lines + 16 price/cost + 4 CRC
= 280 bytes; 14 records per 4 KB sector.

| Tier | Size |
|---|---|
| min1 | ~0.83 MB |
| min5 | ~2.5 MB |
| day (larger record, ~400 B) | ~0.3 MB |
| events | 64 KB |
| **Total** | **~3.7 MB** |

The build computes sizes from the actual configuration and fails if the
partition is too small. If space is tight, `min5: store_inputs: false` drops
per-input energy from the 5-minute tier (outputs only, roughly halving it),
at the cost of recomputation beyond 48 h.

### 5.3 Definitions hash and recomputation

Every record stores a hash of the output definitions (ids, order and lambda
source text, computed at codegen). When reading a record:

- hash matches the running firmware: stored outputs are returned as is
  (exact, including clamps);
- hash differs and inputs are stored: outputs are recomputed by applying the
  current lambdas to each input's average net power over the interval, and the
  row is marked `recomputed`. This is exact for linear combinations of inputs
  and an approximation for clamps and cross-input sign splits.

### 5.4 Flash layout and wear

- A dedicated data partition (`wattledger`), split into one ring per tier.
- Records never span sectors. Each record is programmed once into erased
  flash; the sector ahead of the write position is erased in advance.
- On boot, each ring is scanned (binary search by the first record of each
  sector) to find the newest valid record. A record with a bad CRC (power loss
  during a write) is treated as missing.
- Wear: the min1 ring (~200 sectors) cycles every 48 h, so each sector is
  erased ~180 times a year, versus ~100,000-cycle endurance.
- Partition table: ESPHome generates it. Extra partitions are declared under
  `esp32: partitions:` (or registered by a component with `add_partition()`),
  appended at the end, and the two app partitions shrink to make room. The
  emporiavue Vue 3 package adds `samd_bak` (64 KB). With ESP-IDF on 8 MB:

| Name | Size |
|---|---|
| otadata, phy_init | 12 KB |
| app0, app1 | 1.81 MB each |
| nvs | 448 KB |
| samd_bak | 64 KB |
| wattledger | 3.75 MB (0x3C0000) |

  Milestone 1 declares `wattledger` in `packages/vue3-wattledger.yaml`. From
  milestone 3 the component registers it itself, sized from the configuration.
  The example sizing in 5.2 (~3.7 MB) only just fits, so the default may
  need `min5: store_inputs: false` or fewer retained days.

**Open:** confirm the flash size on both units (`esptool flash_id`) and the
compiled firmware size with Ethernet, HTTPS and the web server against the
1.81 MB app partitions.

## 6. Time

### 6.1 Sources

In order of preference:

1. **SNTP** from the internal NTP servers (pfSense and others), then public
   servers.
2. **RTC (optional, needs soldering; `packages/vue3-rtc.yaml`).** A SparkFun RV-1805 module (supercapacitor, about 35 days without
   power) on the Vue 3's existing I2C bus (SDA GPIO5, SCL GPIO18, shared with
   the SAMD09 at 0x64; RV-1805 at 0x69). The `rv1805` ESPHome time
   component reads it at boot; SNTP's `on_time_sync` writes it. The RTC's
   oscillator-failure flag (set when backup power ran out) marks its time
   invalid. It runs on its crystal while powered and switches to its
   autocalibrated RC oscillator on backup.
3. **None** (untimed mode, 6.2).

ESPHome automatic reboots are disabled: `api: reboot_timeout: 0s` and
`wifi: reboot_timeout: 0s` (Ethernet has no such timeout). Otherwise the
device reboots every 15 minutes while HA or the network is down.

**Open:** verify that TP9/TP10 are SDA/SCL (continuity to ESP32 GPIO5/18,
board unpowered), and that the RV-1805 board fits between the board and the
lid with its connectors removed and its pull-up jumpers opened.

### 6.2 Untimed mode

If no valid time is available at boot, the device does not wait:

- Intervals are closed on monotonic time (every 60 s and 300 s since boot),
  with `start` holding monotonic seconds and the `UNTIMED` flag.
- When a time source becomes valid, an **anchor** event
  `{boot_id, monotonic_s, unix_s}` is written to the events ring. Readers
  resolve every untimed record of that boot through its anchor. Records are
  never rewritten.
- Resolved untimed intervals are not aligned to wall-clock minutes. Consumers
  that need aligned buckets (PVOutput, hourly statistics, daily sealing)
  split each interval's energy across the buckets it overlaps,
  proportionally to time, and mark the result `realigned`.
- After the anchor, the current interval is cut short and the next one starts
  on a wall-clock boundary.
- If power fails again before any anchor, that boot's records can't be placed
  in time. They stay in the rings, are reported as `unresolvable` with their
  energy totals, and are excluded from uploads and statistics.

### 6.3 Clock corrections while timed

A correction under 2 s is absorbed. A larger step closes the current interval
(`CLOCK_STEP`), writes an event, and realigns. Intervals are identified by
sequence number, so a backward step can't overwrite data; consumers bucket by
start time and add up overlapping intervals.

### 6.4 Time zone and day boundaries

ESPHome's time zone setting (POSIX TZ string, DST aware) defines the local
day for daily sealing and PVOutput date/time fields.

## 7. PVOutput uploader

Enabled on the grid Vue only.

### 7.1 Configuration

```yaml
wattledger:
  pvoutput:
    api_key: !secret pvoutput_api_key
    system_id: !secret pvoutput_system_id
    donor: true              # batch 100, 300 req/h, 90 days; false: 30, 60 req/h, 14 days
    generation: generation   # output id → v1/v2
    consumption: consumption # output id → v3/v4
    voltage: line_1_voltage  # line → v6
    reserve_requests: 10     # per hour, left for other clients on this key
```

The PVOutput system's status interval must be set to 5 minutes.

### 7.2 Status values

From min5 records (realigned where needed):

- `v2` / `v4`: average power over the interval (W) = interval energy × 12.
- `v1` / `v3`: energy since local midnight (Wh), the running sum of the day's
  min5 records. A replay therefore starts at the first local day fully inside
  retention.
- `v6`: average voltage.
- Missing intervals are skipped, not filled. Partial intervals upload the
  energy actually measured.
- Optional donor extended values `v7`–`v12` can map to further outputs.

### 7.3 Transport and responses

- `addbatchstatus.jsp`, HTTPS, batches of 30 (100 as donor), oldest first.
- Every request sends `X-Rate-Limit: 1`. `X-Rate-Limit-Remaining` and
  `X-Rate-Limit-Reset` (Unix time) are stored. When the remaining count
  reaches `reserve_requests`, or on a rate-limit error, the uploader sleeps
  until the reset time plus jitter.
- The per-status results in the response are parsed. The cursor advances past
  every status PVOutput reports as processed. Statuses rejected as too old are
  skipped and counted. Other errors back off exponentially, capped at 30 min.
- Records older than the account's history limit (14 or 90 days) are skipped
  and counted.

**Open:** confirm the per-status response format and how PVOutput treats an
upload that omits fields an earlier upload for the same interval set.

### 7.4 Cursors and replay

Stored in NVS (ESPHome preferences), written at most once per minute:

- `live_cursor`: sequence of the last min5 record uploaded. After an outage
  the uploader simply continues from it, oldest first, so catch-up needs no
  special mode.
- `replay`: `{active, next_seq, end_seq}`. The **Replay** button (ESPHome
  button entity, also exposed over HTTP) sets it to the oldest complete day in
  retention, up to the current `live_cursor`. Replay survives reboots and
  never moves `live_cursor`.
- Priority: live catch-up first, replay with the remaining request budget.
- If the min5 ring overwrites records not yet uploaded, the loss is counted
  and reported as an error. It is never silent.

### 7.5 Coexistence with ha-pvoutput-uploader

Two writers on one PVOutput system overwrite each other. The device owns
`v1`–`v4` and `v6` for this system. The HA uploader is either disabled for
this system, or restricted to fields the device never sends (for example
battery fields), once 7.3's open question about partial updates is answered.

### 7.6 Status entities

Backlog (statuses), last successful upload, last error, rate limit remaining,
rate limit reset, replay progress, rejected and lost counters.

## 8. Cost and daily ledger

Enabled on the grid Vue.

### 8.1 Prices

```yaml
wattledger:
  cost:
    grid: grid                    # output id; import = positive
    import_price: sensor.import_price    # HA entity, currency/kWh
    export_price: sensor.export_price
    daily_supply_charge: 1.10     # or an HA entity
    max_price_age: 2h
    fallback_schedule:            # optional time-of-use tariff for HA outages
      - { from: "00:00", to: "15:00", import: 0.28, export: 0.05 }
      - { from: "15:00", to: "21:00", import: 0.45, export: 0.05 }
      - { from: "21:00", to: "24:00", import: 0.28, export: 0.05 }
```

- Prices come from HA through ESPHome's `homeassistant` sensor platform and
  are kept in RAM and NVS.
- If a price is older than `max_price_age`, or HA is disconnected: use the
  fallback schedule if configured (`PRICE_SCHEDULE`), otherwise the last
  known price (`PRICE_STALE`).
- Cost and credit are accumulated **per sample** from the grid output's
  import and export energy, so they are exact even with dynamic prices and
  sign changes inside an interval.

**Open:** which HA entities provide the prices, how often they change (5 or
30 minutes for a wholesale-linked plan), and how the supply charge is
expressed.

### 8.2 Sealing a day

- A local day is sealed at local midnight + `seal_delay` (default 15 min) from
  that day's min1 records, which are still in the 48 h ring. If that day has
  untimed records with no anchor yet, sealing waits up to 24 h, then seals
  with an `unresolved` flag.
- The day record holds: date, per-input and per-output import and export
  energy, average voltage per line, cost, credit, supply charge, total, and
  quality counters: missing minutes, untimed minutes resolved, fallback-price
  minutes, recomputed flag, defs hash.
- A sealed day is never modified.
- Monthly and yearly rollups are done by the integration.

## 9. Home Assistant companion integration

Domain `wattledger`, in `custom_components/wattledger` in this repository.
It complements the ESPHome native API (live entities) with history.

### 9.1 Device history API

ESPHome web server handlers under `/wattledger/v1/`:

| Endpoint | Returns |
|---|---|
| `info` | firmware, layout, inputs, outputs, lines, defs hash, ring ranges (first/last seq per tier), boot id, time source |
| `rows?tier=min1&from_seq=N&limit=500` | records with anchors resolved, compact JSON |
| `days?from=YYYY-MM-DD` | sealed day records |
| `events?from_seq=N` | anchors, clock steps, errors |
| `replay` (POST) | starts a PVOutput replay |

Protected by a bearer token from the ESPHome configuration.

### 9.2 Statistics

Patching recorder statistics of the ESPHome entities after downtime fights
the recorder (HA attributes the whole outage's energy to the first hour after
restart). Instead the integration owns **external statistics**, imported
hourly from journal rows:

- `wattledger:<device>_<output>_import` / `_export` for energy, and
  `..._cost` / `..._credit` for the grid output.
- Each hour is computed from min1 rows (realigned where needed) and imported
  idempotently. Any hour can be re-imported, so HA downtime of up to 48 h
  leaves no gap.
- These statistics are what the Energy dashboard uses.
- **Combined values** across the two Vues (for example total house use =
  grid-side consumption while on generator + generator circuits) are
  configured in the integration as sums of device outputs, producing their
  own external statistics.

### 9.3 Daily and monthly

- Sensors for the last sealed day (energy, cost, credit, quality).
- Monthly and yearly rollups from day rows, as statistics and sensors.

### 9.4 Polling

On startup, then every 15 minutes: read `info`, fetch rows newer than the last
imported sequence per tier, import affected hours, fetch new days. The device
holds 48 h of min1, so any HA downtime shorter than that is recovered
completely.

## 10. Calibration notes

- CT phase correction is set at night on the inverter circuit, when the
  inverter draws ~0.5 A at PF ~0: adjust until real power reads its standby
  draw (a few watts). At PF ~1 the correction barely affects readings, so this
  doesn't hurt daytime accuracy.
- Gain and voltage: check against a kettle (resistive) and a multimeter.
- The generation lambda clamps residual standby readings (4.3 example).

## 11. Testing

- Host unit tests (plain C++, no ESP-IDF) for the journal against an in-memory
  flash with simulated power loss mid-write, ring wrap, boot scan, untimed
  anchoring and realignment, the PVOutput batch encoder and response parser,
  rate-limit scheduling, and day sealing.
- Integration tests in Python for the HA side with recorded `rows` payloads.
- Bench: run the grid Vue next to the IoTaWatt for a week, compare daily
  totals per circuit and PVOutput statuses; test HA down, internet down, power
  cycle with and without the RTC.

## 12. Milestones

1. Scaffold: repository layout, example Vue 3 YAMLs (reboot timeouts off,
   per-CT lines), partition table, `rv1805` time component.
2. Sampler, lambdas, per-direction accumulation, live entities.
3. Journal (min1, min5, events), boot scan, HTTP `info`/`rows`.
4. Untimed mode and anchors.
5. PVOutput uploader: cursor, batches, rate limits, replay.
6. Cost and day sealing.
7. HA integration: external statistics, combined values, days, replay button.
8. Bench validation against the IoTaWatt.

## 13. Open questions

1. emporiavue driver: per-CT publication interval and grouping; calibrated
   per-frame access; partition expectations for the SAMD09 backup.
2. Flash size of both Vue 3 units; firmware size with Ethernet, HTTPS, web
   server.
3. TP9/TP10 as SDA/SCL; RV-1805 fit.
4. PVOutput: per-status response format; behaviour of partial-field updates;
   donor status of the account.
5. Price entities, update rate and supply charge format.
6. Whether the generator Vue also needs any cost (fuel) accounting.
