#pragma once

// Builds PVOutput statuses from 5-minute records (design section 7.2).
// Independent of ESPHome so it can be unit-tested on the host.

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "timeline.h"

namespace esphome::wattledger {

static const uint32_t STATUS_INTERVAL_S = 300;

/// A record placed in time, with what PVOutput needs from it.
struct PlacedRecord {
  uint32_t seq;
  uint32_t start_unix;
  uint32_t duration_s;
  double generation_wh;
  double consumption_wh;
  /// Average voltage over the record, NaN if none.
  float voltage;
};

using ToLocal = std::function<LocalTime(uint32_t unix_time)>;

/// One 5-minute status.
struct Status {
  uint32_t start_unix;
  double generation_wh{0};
  double consumption_wh{0};
  /// Energy since local midnight up to the end of this status.
  double day_generation_wh{0};
  double day_consumption_wh{0};
  double volt_seconds{0};
  double voltage_seconds{0};  ///< seconds with a voltage
};

/// Energy already in earlier statuses of a day.
struct DayTotals {
  uint32_t day_start{0};
  double generation_wh{0};
  double consumption_wh{0};
};

/// Groups `records` (in sequence order) into aligned 5-minute statuses,
/// sharing unaligned records' energy across buckets in proportion to time.
/// Only complete statuses are returned (ending no later than the latest
/// record), at most `max_statuses`, oldest first.
///
/// State carried between batches: `day` holds the day's running totals and
/// `sent_until` the end of the last status sent; statuses before it are
/// skipped (the earlier part of a record that crossed the last batch's end).
/// `consumed` is set to the number of leading records fully sent.
std::vector<Status> build_statuses(const std::vector<PlacedRecord> &records, size_t max_statuses,
                                   const ToLocal &to_local, DayTotals &day, uint32_t &sent_until,
                                   size_t &consumed);

/// One `data` entry: d,t,v1,v2,v3,v4[,v5,v6]. The time is the status's end;
/// a status ending at local midnight is reported at 23:59 of its own day,
/// so the day's energy isn't attributed to the next day.
std::string format_status(const Status &status, const ToLocal &to_local, bool with_consumption);

/// Parses an addbatchstatus response ("d,t,1;d,t,0;..."): the number of
/// statuses it reports, or -1 if it isn't a batch response.
int count_status_results(const std::string &body);

}  // namespace esphome::wattledger
