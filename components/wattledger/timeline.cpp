#include "timeline.h"

#include <ctime>

namespace esphome::wattledger {

/// Unix time of local midnight on the calendar day `tm` (day offset by
/// `days`). Computed through the calendar, with daylight saving worked out
/// again for midnight: on a changeover day, the wall-clock time isn't the
/// time elapsed since midnight.
static uint32_t local_midnight(struct tm tm, int days) {
  tm.tm_mday += days;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  return static_cast<uint32_t>(mktime(&tm));
}

LocalTime local_time(uint32_t unix_time) {
  const time_t t = unix_time;
  struct tm tm;
  localtime_r(&t, &tm);
  LocalTime lt;
  lt.date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  lt.minute = tm.tm_hour * 60 + tm.tm_min;
  lt.day_start = local_midnight(tm, 0);
  return lt;
}

uint32_t next_local_midnight(uint32_t day_start) {
  const time_t t = day_start;
  struct tm tm;
  localtime_r(&t, &tm);
  return local_midnight(tm, 1);
}

void AnchorTable::add(uint16_t boot_id, uint32_t uptime_s, uint32_t unix_s) {
  for (const Anchor &a : this->anchors_) {
    if (a.boot_id == boot_id)
      return;
  }
  this->anchors_.push_back({boot_id, uptime_s, unix_s});
}

uint32_t AnchorTable::resolve(uint16_t boot_id, uint32_t uptime_s) const {
  for (const Anchor &a : this->anchors_) {
    if (a.boot_id == boot_id)
      return a.unix_s - a.uptime_s + uptime_s;
  }
  return 0;
}

}  // namespace esphome::wattledger
