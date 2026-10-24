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
  /// Cost of the cost output's import and credit for its export, in currency.
  double cost{0};
  double credit{0};
  /// Valid time priced with the last known price or the fallback schedule.
  uint32_t price_stale_ms{0};
  uint32_t price_schedule_ms{0};

  double output_net_wh(size_t output) const { return this->output_import_wh[output] - this->output_export_wh[output]; }
};

/// Prices for one integration step, per kWh.
struct Prices {
  enum Source : uint8_t { NONE, LIVE, STALE, SCHEDULE };
  double import_per_kwh{0};
  double export_per_kwh{0};
  Source source{NONE};
};

class Accumulator {
 public:
  void configure(size_t inputs, size_t lines, std::vector<OutputFn> outputs);
  /// The output whose import is costed and export credited, or -1.
  void set_cost_output(int output) { this->cost_output_ = output; }
  int cost_output() const { return this->cost_output_; }

  /// Integrates the held powers and voltages over `dt_ms`. When `valid` is
  /// false only the elapsed time is counted: no energy is invented for a gap.
  /// Cost is accumulated per step, so it is exact for changing prices and
  /// for sign changes within an interval.
  void integrate(const float *input_w, const float *line_v, uint32_t dt_ms, bool valid,
                 const Prices &prices = Prices{});

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
  int cost_output_{-1};
  std::vector<OutputFn> outputs_;
  std::vector<float> output_w_;
  Totals totals_;
};

/// Difference between two totals snapshots (later minus earlier).
Totals delta(const Totals &later, const Totals &earlier);

}  // namespace esphome::wattledger
