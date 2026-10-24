#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/time/real_time_clock.h"

#include "accumulator.h"
#include "journal.h"
#include "partition_storage.h"
#include "records.h"
#include "timeline.h"

namespace esphome::wattledger {

/// Samples power sensors, evaluates output lambdas on every sample,
/// accumulates import and export energy (design section 4) and records
/// 1- and 5-minute intervals in the flash journal (section 5).
class WattLedgerComponent : public PollingComponent {
 public:
  WattLedgerComponent(uint32_t output_slots, uint32_t min1_records, uint32_t min5_records, uint32_t event_records)
      : layout_(0, output_slots, 0),
        min1_records_(min1_records),
        min5_records_(min5_records),
        event_records_(event_records) {}

  void add_input(const char *name, sensor::Sensor *sensor);
  void add_line(const char *name, sensor::Sensor *sensor);
  void add_output(const char *name, OutputFn fn, sensor::Sensor *power, sensor::Sensor *import_energy,
                  sensor::Sensor *export_energy);
  void set_stale_timeout(uint32_t stale_timeout_ms) { this->stale_timeout_ms_ = stale_timeout_ms; }
  void set_defs_hash(uint32_t defs_hash) { this->defs_hash_ = defs_hash; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// JSON for the history API (design section 9.1). Safe to call from the
  /// web server's task.
  std::string info_json();
  /// Rows of a tier ("min1", "min5" or "events") from `from_seq`. False if
  /// the tier is unknown or the journal isn't available.
  bool rows_json(const std::string &tier, uint32_t from_seq, uint32_t limit, std::string &out);

  /// Sequence range of the 5-minute tier. Safe from other tasks.
  bool min5_range(uint32_t &first_seq, uint32_t &next_seq);
  /// Reads a 5-minute record; `start_unix` is its placed start, 0 if its boot
  /// has no valid time (yet). False if the record isn't there. Safe from other tasks.
  bool read_min5(uint32_t seq, IntervalRecord &rec, uint32_t &start_unix);
  uint16_t boot_id() const { return this->boot_id_; }

 protected:
  struct Input {
    const char *name;
    sensor::Sensor *sensor;
  };
  struct Output {
    const char *name;
    sensor::Sensor *power;
    sensor::Sensor *import_energy;
    sensor::Sensor *export_energy;
  };
  /// An open interval of one tier.
  struct Tier {
    const char *name;
    uint32_t length_s;
    Ring *ring;
    bool open{false};
    bool timed{false};
    uint8_t flags{0};
    /// Unix seconds if timed, else seconds since boot.
    uint32_t start{0};
    uint64_t start_uptime_ms{0};
    Totals start_totals;
    std::vector<int64_t> start_mwh;
  };

  /// Integrates the held values from the last sample time up to `now`.
  void advance_(uint32_t now);
  bool inputs_valid_(uint32_t now) const;
  /// Unix time, or 0 while the time isn't valid.
  uint32_t unix_now_();
  void open_tier_(Tier &tier, uint32_t unix_now);
  void close_tier_(Tier &tier);
  /// Ends the open intervals now and starts new ones at `unix_now`, marking
  /// the ended ones with `flags`.
  void restart_tiers_(uint32_t unix_now, uint8_t flags);
  /// Watches the valid time for the first valid time (anchor) and for jumps.
  void track_time_(uint32_t unix_now);
  void load_anchors_();
  /// Rounded mWh per channel (inputs then outputs, import and export), so
  /// interval energies telescope: rounding never accumulates.
  std::vector<int64_t> rounded_mwh_(const Totals &totals) const;
  void write_event_(uint8_t type, uint32_t unix_s, uint32_t a, uint32_t b);
  void append_interval_json_(const IntervalRecord &rec, std::string &out) const;

  std::vector<Input> inputs_;
  std::vector<Input> lines_;
  std::vector<Output> outputs_;
  std::vector<OutputFn> output_fns_;

  /// Latest raw (unfiltered) value of each input and line, held until the next.
  std::vector<float> input_w_;
  std::vector<uint32_t> input_seen_ms_;
  std::vector<float> line_v_;
  std::vector<uint32_t> line_seen_ms_;
  /// Line voltages as integrated (stale ones replaced by NaN), reused per step.
  std::vector<float> line_v_scratch_;

  Accumulator accumulator_;
  uint32_t last_sample_ms_{0};
  uint32_t stale_timeout_ms_{5000};
  uint32_t defs_hash_{0};
  time::RealTimeClock *time_{nullptr};

  /// Milliseconds since boot, without millis()'s 49-day wrap.
  uint64_t uptime_ms_{0};
  uint32_t last_millis_{0};

  IntervalLayout layout_;
  uint32_t min1_records_;
  uint32_t min5_records_;
  uint32_t event_records_;
#ifdef USE_ESP32
  PartitionStorage storage_;
#endif
  Journal journal_;
  bool journal_ok_{false};
  /// Guards the journal: records are written from the main loop and read
  /// from the web server's task.
  std::mutex journal_mutex_;
  std::vector<uint8_t> record_buffer_;
  uint16_t boot_id_{0};
  bool anchored_{false};
  /// Anchors of all boots in the journal, for placing untimed records.
  AnchorTable anchors_;
  /// Last valid time seen, and the uptime then, to detect clock jumps.
  uint32_t last_unix_{0};
  uint64_t last_unix_uptime_ms_{0};
  Tier min1_{"min1", 60, nullptr};
  Tier min5_{"min5", 300, nullptr};

  /// Totals at the previous update(), for average output power.
  Totals published_;
};

}  // namespace esphome::wattledger
