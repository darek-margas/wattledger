#include "accumulator.h"

#include <cmath>

namespace esphome::wattledger {

static const double MS_PER_HOUR = 3600000.0;

void Accumulator::configure(size_t inputs, size_t lines, std::vector<OutputFn> outputs) {
  this->inputs_ = inputs;
  this->lines_ = lines;
  this->outputs_ = std::move(outputs);
  this->output_w_.assign(this->outputs_.size(), NAN);
  this->totals_ = Totals{};
  this->totals_.input_import_wh.assign(inputs, 0.0);
  this->totals_.input_export_wh.assign(inputs, 0.0);
  this->totals_.output_import_wh.assign(this->outputs_.size(), 0.0);
  this->totals_.output_export_wh.assign(this->outputs_.size(), 0.0);
  this->totals_.line_volt_seconds.assign(lines, 0.0);
  this->totals_.line_valid_ms.assign(lines, 0);
}

void Accumulator::add_split_(double power_w, double hours, double &import_wh, double &export_wh) {
  if (power_w >= 0) {
    import_wh += power_w * hours;
  } else {
    export_wh -= power_w * hours;
  }
}

void Accumulator::integrate(const float *input_w, const float *line_v, uint32_t dt_ms, bool valid,
                            const Prices &prices) {
  if (dt_ms == 0)
    return;
  Totals &t = this->totals_;
  t.total_ms += dt_ms;

  // Voltage is independent of the inputs' validity.
  for (size_t i = 0; i < this->lines_; i++) {
    if (std::isfinite(line_v[i])) {
      t.line_volt_seconds[i] += line_v[i] * (dt_ms / 1000.0);
      t.line_valid_ms[i] += dt_ms;
    }
  }

  if (!valid)
    return;
  t.valid_ms += dt_ms;
  const double hours = dt_ms / MS_PER_HOUR;
  for (size_t i = 0; i < this->inputs_; i++) {
    add_split_(input_w[i], hours, t.input_import_wh[i], t.input_export_wh[i]);
  }
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    float power = this->outputs_[o](input_w);
    if (!std::isfinite(power))
      power = 0.0f;
    this->output_w_[o] = power;
    add_split_(power, hours, t.output_import_wh[o], t.output_export_wh[o]);
  }

  if (this->cost_output_ >= 0 && prices.source != Prices::NONE) {
    const double kwh = this->output_w_[this->cost_output_] * hours / 1000.0;
    if (kwh >= 0) {
      t.cost += kwh * prices.import_per_kwh;
    } else {
      t.credit -= kwh * prices.export_per_kwh;
    }
    if (prices.source == Prices::STALE)
      t.price_stale_ms += dt_ms;
    else if (prices.source == Prices::SCHEDULE)
      t.price_schedule_ms += dt_ms;
  }
}

Totals delta(const Totals &later, const Totals &earlier) {
  Totals d = later;
  for (size_t i = 0; i < d.input_import_wh.size(); i++) {
    d.input_import_wh[i] -= earlier.input_import_wh[i];
    d.input_export_wh[i] -= earlier.input_export_wh[i];
  }
  for (size_t o = 0; o < d.output_import_wh.size(); o++) {
    d.output_import_wh[o] -= earlier.output_import_wh[o];
    d.output_export_wh[o] -= earlier.output_export_wh[o];
  }
  for (size_t l = 0; l < d.line_volt_seconds.size(); l++) {
    d.line_volt_seconds[l] -= earlier.line_volt_seconds[l];
    d.line_valid_ms[l] -= earlier.line_valid_ms[l];
  }
  d.valid_ms -= earlier.valid_ms;
  d.total_ms -= earlier.total_ms;
  d.cost -= earlier.cost;
  d.credit -= earlier.credit;
  d.price_stale_ms -= earlier.price_stale_ms;
  d.price_schedule_ms -= earlier.price_schedule_ms;
  return d;
}

}  // namespace esphome::wattledger
