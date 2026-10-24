#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "accumulator.h"

namespace esphome::wattledger {

/// Samples power sensors, evaluates output lambdas on every sample and
/// accumulates import and export energy (design section 4).
class WattLedgerComponent : public PollingComponent {
 public:
  void add_input(const char *name, sensor::Sensor *sensor);
  void add_line(const char *name, sensor::Sensor *sensor);
  void add_output(const char *name, OutputFn fn, sensor::Sensor *power, sensor::Sensor *import_energy,
                  sensor::Sensor *export_energy);
  void set_stale_timeout(uint32_t stale_timeout_ms) { this->stale_timeout_ms_ = stale_timeout_ms; }
  void set_interval(uint32_t interval_ms) { this->interval_ms_ = interval_ms; }
  void set_defs_hash(uint32_t defs_hash) { this->defs_hash_ = defs_hash; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  const Totals &totals() const { return this->accumulator_.totals(); }
  uint32_t defs_hash() const { return this->defs_hash_; }

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

  /// Integrates the held values from the last sample time up to `now`.
  void advance_(uint32_t now);
  bool inputs_valid_(uint32_t now) const;
  void close_interval_(uint32_t now);

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

  /// Interval bookkeeping; from milestone 3 a closed interval is a journal record.
  uint32_t interval_ms_{60000};
  uint32_t interval_start_ms_{0};
  Totals interval_start_;

  /// Totals at the previous update(), for average output power.
  Totals published_;
};

}  // namespace esphome::wattledger
