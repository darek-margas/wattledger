#include "timeline.h"

#include <ctime>

namespace esphome::wattledger {

LocalTime local_time(uint32_t unix_time) {
  const time_t t = unix_time;
  struct tm tm;
  localtime_r(&t, &tm);
  LocalTime lt;
  lt.date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  lt.minute = tm.tm_hour * 60 + tm.tm_min;
  lt.day_start = unix_time - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
  return lt;
}

uint32_t next_local_midnight(uint32_t day_start) {
  // 36 h later is inside the next day whether this one has 23, 24 or 25 hours.
  return local_time(day_start + 36 * 3600).day_start;
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
