#pragma once

// Energy accumulation, independent of ESPHome so it can be unit-tested on the host.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::wattledger {

/// An output: computes power (W) from the input powers (W), in input order.
/// Must be a pure function of its arguments (design section 4.3).
using OutputFn = float (*)(const float *inputs);

/// Energy accumulated since the last reset. Import is positive power,
/// export negative power, both stored as positive Wh.
struct Totals {
  std::vector<double> input_import_wh;
  std::vector<double> input_export_wh;
  std::vector<double> output_import_wh;
  std::vector<double> output_export_wh;
  /// Volt-seconds per line over the time that line's voltage was valid.
  std::vector<double> line_volt_seconds;
  std::vector<uint32_t> line_valid_ms;
  /// Time with every input valid, i.e. integrated.
  uint32_t valid_ms{0};
  /// All time, valid or not.
  uint32_t total_ms{0};

  double output_net_wh(size_t output) const { return this->output_import_wh[output] - this->output_export_wh[output]; }
};

class Accumulator {
 public:
  void configure(size_t inputs, size_t lines, std::vector<OutputFn> outputs);

  /// Integrates the held powers and voltages over `dt_ms`. When `valid` is
  /// false only the elapsed time is counted: no energy is invented for a gap.
  void integrate(const float *input_w, const float *line_v, uint32_t dt_ms, bool valid);

  const Totals &totals() const { return this->totals_; }
  /// Output powers from the most recent valid integration.
  const std::vector<float> &output_w() const { return this->output_w_; }
  size_t input_count() const { return this->inputs_; }
  size_t output_count() const { return this->outputs_.size(); }
  size_t line_count() const { return this->lines_; }

 protected:
  static void add_split_(double power_w, double hours, double &import_wh, double &export_wh);

  size_t inputs_{0};
  size_t lines_{0};
  std::vector<OutputFn> outputs_;
  std::vector<float> output_w_;
  Totals totals_;
};

/// Difference between two totals snapshots (later minus earlier).
Totals delta(const Totals &later, const Totals &earlier);

}  // namespace esphome::wattledger
