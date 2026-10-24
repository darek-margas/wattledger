#pragma once

// Builds a sealed day from 5-minute records (design section 8.2).
// Independent of ESPHome so it can be unit-tested on the host.

#include <cstdint>
#include <vector>

#include "records.h"

namespace esphome::wattledger {

class DaySealer {
 public:
  /// The local day [day_start, day_end), `date` as yyyymmdd.
  DaySealer(size_t inputs, size_t output_slots, size_t lines, uint32_t date, uint32_t day_start, uint32_t day_end,
            uint32_t defs_hash);

  /// Adds a record placed at `start_unix`. A record crossing midnight
  /// counts in proportion to its overlap with the day.
  void add(const IntervalRecord &rec, uint32_t start_unix);
  /// The day's record, with the daily supply charge (in currency).
  DayRecord finish(double supply_charge) const;

 protected:
  uint32_t date_;
  uint32_t day_start_;
  uint32_t day_end_;
  uint32_t defs_hash_;
  uint8_t flags_{0};
  uint16_t records_{0};
  double coverage_s_{0};
  double stale_s_{0};
  double schedule_s_{0};
  std::vector<double> input_import_, input_export_, output_import_, output_export_;
  std::vector<double> line_dv_seconds_, line_seconds_;
  double cost_{0};
  double credit_{0};
};

}  // namespace esphome::wattledger
