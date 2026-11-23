#include "timeline.h"

#include <ctime>

namespace esphome::wattledger {

static bool libc_local(time_t unix_time, struct tm *out) { return localtime_r(&unix_time, out) != nullptr; }

static LocalConverter converter = libc_local;

void set_local_converter(LocalConverter c) { converter = c != nullptr ? c : libc_local; }

static struct tm to_local(int64_t unix_time) {
  struct tm tm {};
  if (!converter(static_cast<time_t>(unix_time), &tm)) {
    const time_t t = static_cast<time_t>(unix_time);
    gmtime_r(&t, &tm);
  }
  return tm;
}

/// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
/// days_from_civil), so no mktime() and no TZ are involved.
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static int64_t local_day(const struct tm &tm) {
  return days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/// Local wall-clock seconds minus UTC seconds at `unix_time`.
static int64_t utc_offset(int64_t unix_time) {
  const struct tm tm = to_local(unix_time);
  return local_day(tm) * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec - unix_time;
}

/// Unix time of local midnight starting local day `day`. The offset is
/// worked out again at midnight itself: on a daylight-saving changeover day
/// the offset at midnight differs from the one later that day.
static uint32_t local_midnight(int64_t day, int64_t hint) {
  const int64_t wall = day * 86400;
  int64_t utc = wall - utc_offset(hint);
  for (int i = 0; i < 3; i++) {
    const int64_t next = wall - utc_offset(utc);
    if (next == utc)
      break;
    utc = next;
  }
  return static_cast<uint32_t>(utc);
}

LocalTime local_time(uint32_t unix_time) {
  const struct tm tm = to_local(unix_time);
  LocalTime lt;
  lt.date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  lt.minute = tm.tm_hour * 60 + tm.tm_min;
  lt.day_start = local_midnight(local_day(tm), unix_time);
  return lt;
}

uint32_t next_local_midnight(uint32_t day_start) {
  // Noon of the day is clear of any changeover, which happen near midnight
  // or in the small hours.
  return local_midnight(local_day(to_local(day_start)) + 1, static_cast<int64_t>(day_start) + 36 * 3600);
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
