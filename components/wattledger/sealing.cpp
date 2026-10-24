#include "sealing.h"

#include <algorithm>
#include <cmath>

namespace esphome::wattledger {

DaySealer::DaySealer(size_t inputs, size_t output_slots, size_t lines, uint32_t date, uint32_t day_start,
                     uint32_t day_end, uint32_t defs_hash)
    : date_(date),
      day_start_(day_start),
      day_end_(day_end),
      defs_hash_(defs_hash),
      input_import_(inputs, 0.0),
      input_export_(inputs, 0.0),
      output_import_(output_slots, 0.0),
      output_export_(output_slots, 0.0),
      line_dv_seconds_(lines, 0.0),
      line_seconds_(lines, 0.0) {}

void DaySealer::add(const IntervalRecord &rec, uint32_t start_unix) {
  const uint32_t end = start_unix + rec.duration_s;
  const uint32_t from = std::max(start_unix, this->day_start_);
  const uint32_t to = std::min(end, this->day_end_);
  if (to <= from && !(rec.duration_s == 0 && start_unix >= this->day_start_ && start_unix < this->day_end_))
    return;
  const double f = rec.duration_s == 0 ? 1.0 : static_cast<double>(to - from) / rec.duration_s;
  const double seconds = rec.duration_s * f;

  this->records_++;
  if (rec.defs_hash != this->defs_hash_)
    this->flags_ |= DAY_MIXED_DEFS;
  this->coverage_s_ += rec.coverage_s * f;
  if (rec.flags & FLAG_PRICE_STALE)
    this->stale_s_ += seconds;
  if (rec.flags & FLAG_PRICE_SCHEDULE)
    this->schedule_s_ += seconds;
  for (size_t i = 0; i < this->input_import_.size() && i < rec.input_import_mwh.size(); i++) {
    this->input_import_[i] += rec.input_import_mwh[i] * f;
    this->input_export_[i] += rec.input_export_mwh[i] * f;
  }
  for (size_t o = 0; o < this->output_import_.size() && o < rec.output_import_mwh.size(); o++) {
    this->output_import_[o] += rec.output_import_mwh[o] * f;
    this->output_export_[o] += rec.output_export_mwh[o] * f;
  }
  for (size_t l = 0; l < this->line_seconds_.size() && l < rec.line_dv.size(); l++) {
    if (rec.line_dv[l] != 0) {
      this->line_dv_seconds_[l] += rec.line_dv[l] * seconds;
      this->line_seconds_[l] += seconds;
    }
  }
  this->cost_ += rec.cost * f;
  this->credit_ += rec.credit * f;
}

DayRecord DaySealer::finish(double supply_charge) const {
  DayRecord day;
  day.date = this->date_;
  day.day_start = this->day_start_;
  day.length_s = this->day_end_ - this->day_start_;
  day.coverage_s = static_cast<uint32_t>(std::lround(this->coverage_s_));
  day.records = this->records_;
  day.price_stale_s = static_cast<uint32_t>(std::lround(this->stale_s_));
  day.price_schedule_s = static_cast<uint32_t>(std::lround(this->schedule_s_));
  day.defs_hash = this->defs_hash_;
  day.flags = this->flags_;
  // Gaps under a minute (sampling jitter) don't make a day partial.
  if (day.coverage_s + 60 < day.length_s)
    day.flags |= DAY_PARTIAL;
  auto to_u32 = [](const std::vector<double> &v) {
    std::vector<uint32_t> out;
    for (double x : v)
      out.push_back(static_cast<uint32_t>(std::llround(x)));
    return out;
  };
  day.input_import_mwh = to_u32(this->input_import_);
  day.input_export_mwh = to_u32(this->input_export_);
  day.output_import_mwh = to_u32(this->output_import_);
  day.output_export_mwh = to_u32(this->output_export_);
  for (size_t l = 0; l < this->line_seconds_.size(); l++) {
    day.line_dv.push_back(this->line_seconds_[l] > 0
                              ? static_cast<uint16_t>(std::lround(this->line_dv_seconds_[l] / this->line_seconds_[l]))
                              : 0);
  }
  day.cost = static_cast<int32_t>(std::llround(this->cost_));
  day.credit = static_cast<int32_t>(std::llround(this->credit_));
  day.supply_charge = static_cast<int32_t>(std::llround(supply_charge * 1e6));
  return day;
}

}  // namespace esphome::wattledger
