#include "wattledger.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::wattledger {

static const char *const TAG = "wattledger";

/// Integration steps are taken on every input update; between updates the
/// loop still advances at least this often, so stale inputs are noticed.
static const uint32_t MAX_STEP_MS = 1000;

void WattLedgerComponent::add_input(const char *name, sensor::Sensor *sensor) {
  this->inputs_.push_back({name, sensor});
}

void WattLedgerComponent::add_line(const char *name, sensor::Sensor *sensor) {
  this->lines_.push_back({name, sensor});
}

void WattLedgerComponent::add_output(const char *name, OutputFn fn, sensor::Sensor *power,
                                     sensor::Sensor *import_energy, sensor::Sensor *export_energy) {
  this->outputs_.push_back({name, power, import_energy, export_energy});
  this->output_fns_.push_back(fn);
}

void WattLedgerComponent::setup() {
  this->accumulator_.configure(this->inputs_.size(), this->lines_.size(), this->output_fns_);
  this->input_w_.assign(this->inputs_.size(), NAN);
  this->input_seen_ms_.assign(this->inputs_.size(), 0);
  this->line_v_.assign(this->lines_.size(), NAN);
  this->line_v_scratch_.assign(this->lines_.size(), NAN);
  this->line_seen_ms_.assign(this->lines_.size(), 0);

  const uint32_t now = millis();
  this->last_sample_ms_ = now;
  this->interval_start_ms_ = now;
  this->interval_start_ = this->accumulator_.totals();
  this->published_ = this->accumulator_.totals();

  // Raw state: the driver's values before any display filter (throttle, average).
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    this->inputs_[i].sensor->add_on_raw_state_callback([this, i](float value) {
      const uint32_t now = millis();
      this->advance_(now);
      this->input_w_[i] = value;
      this->input_seen_ms_[i] = now;
    });
  }
  for (size_t l = 0; l < this->lines_.size(); l++) {
    this->lines_[l].sensor->add_on_raw_state_callback([this, l](float value) {
      const uint32_t now = millis();
      this->advance_(now);
      this->line_v_[l] = value;
      this->line_seen_ms_[l] = now;
    });
  }
}

void WattLedgerComponent::loop() {
  const uint32_t now = millis();
  if (now - this->last_sample_ms_ >= MAX_STEP_MS)
    this->advance_(now);
  if (now - this->interval_start_ms_ >= this->interval_ms_)
    this->close_interval_(now);
}

bool WattLedgerComponent::inputs_valid_(uint32_t now) const {
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    if (!std::isfinite(this->input_w_[i]) || now - this->input_seen_ms_[i] > this->stale_timeout_ms_)
      return false;
  }
  return true;
}

void WattLedgerComponent::advance_(uint32_t now) {
  const uint32_t dt = now - this->last_sample_ms_;
  if (dt == 0)
    return;
  // A line voltage that stopped updating counts as missing, like an input.
  for (size_t l = 0; l < this->line_v_.size(); l++) {
    this->line_v_scratch_[l] = now - this->line_seen_ms_[l] > this->stale_timeout_ms_ ? NAN : this->line_v_[l];
  }
  this->accumulator_.integrate(this->input_w_.data(), this->line_v_scratch_.data(), dt, this->inputs_valid_(now));
  this->last_sample_ms_ = now;
}

void WattLedgerComponent::close_interval_(uint32_t now) {
  this->advance_(now);
  const Totals &totals = this->accumulator_.totals();
  const Totals d = delta(totals, this->interval_start_);
  ESP_LOGD(TAG, "Interval closed: %.1f s, %.1f s valid", d.total_ms / 1000.0f, d.valid_ms / 1000.0f);
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    ESP_LOGD(TAG, "  %s: import %.3f Wh, export %.3f Wh", this->outputs_[o].name, d.output_import_wh[o],
             d.output_export_wh[o]);
  }
  this->interval_start_ = totals;
  this->interval_start_ms_ = now;
}

void WattLedgerComponent::update() {
  this->advance_(millis());
  const Totals &totals = this->accumulator_.totals();
  const Totals d = delta(totals, this->published_);
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    const Output &out = this->outputs_[o];
    if (out.power != nullptr) {
      // Average over the valid time since the last update, not the latest sample.
      const float power = d.valid_ms > 0 ? d.output_net_wh(o) * 3600000.0 / d.valid_ms : NAN;
      out.power->publish_state(power);
    }
    if (out.import_energy != nullptr)
      out.import_energy->publish_state(totals.output_import_wh[o] / 1000.0);
    if (out.export_energy != nullptr)
      out.export_energy->publish_state(totals.output_export_wh[o] / 1000.0);
  }
  this->published_ = totals;
}

void WattLedgerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "WattLedger:");
  ESP_LOGCONFIG(TAG, "  Definitions hash: 0x%08X", (unsigned) this->defs_hash_);
  ESP_LOGCONFIG(TAG, "  Stale timeout: %u ms", (unsigned) this->stale_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Interval: %u ms", (unsigned) this->interval_ms_);
  for (const Input &input : this->inputs_)
    ESP_LOGCONFIG(TAG, "  Input %s", input.name);
  for (const Input &line : this->lines_)
    ESP_LOGCONFIG(TAG, "  Line %s", line.name);
  for (const Output &output : this->outputs_)
    ESP_LOGCONFIG(TAG, "  Output %s", output.name);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::wattledger
