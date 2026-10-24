#include "pvoutput_status.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "timeline.h"

namespace esphome::wattledger {

std::vector<Status> build_statuses(const std::vector<PlacedRecord> &records, size_t max_statuses,
                                   const ToLocal &to_local, DayTotals &day, uint32_t &sent_until,
                                   size_t &consumed) {
  std::map<uint32_t, Status> buckets;
  uint32_t latest_end = 0;
  for (const PlacedRecord &rec : records) {
    for_each_bucket(rec.start_unix, rec.duration_s, STATUS_INTERVAL_S, [&](uint32_t bucket, double fraction) {
      Status &s = buckets[bucket];
      s.start_unix = bucket;
      s.generation_wh += rec.generation_wh * fraction;
      s.consumption_wh += rec.consumption_wh * fraction;
      if (std::isfinite(rec.voltage)) {
        const double seconds = rec.duration_s * fraction;
        s.volt_seconds += rec.voltage * seconds;
        s.voltage_seconds += seconds;
      }
    });
    latest_end = std::max(latest_end, rec.start_unix + rec.duration_s);
  }

  std::vector<Status> out;
  for (auto &entry : buckets) {
    // Already sent: the earlier part of a record that crossed the last batch's end.
    if (entry.first < sent_until)
      continue;
    if (entry.first + STATUS_INTERVAL_S > latest_end || out.size() >= max_statuses)
      break;
    out.push_back(entry.second);
  }
  if (!out.empty())
    sent_until = out.back().start_unix + STATUS_INTERVAL_S;

  for (Status &s : out) {
    const uint32_t day_start = to_local(s.start_unix).day_start;
    if (day_start != day.day_start)
      day = DayTotals{day_start, 0, 0};
    day.generation_wh += s.generation_wh;
    day.consumption_wh += s.consumption_wh;
    s.day_generation_wh = day.generation_wh;
    s.day_consumption_wh = day.consumption_wh;
  }

  consumed = 0;
  while (consumed < records.size() &&
         records[consumed].start_unix + records[consumed].duration_s <= sent_until)
    consumed++;
  return out;
}

std::string format_status(const Status &status, const ToLocal &to_local, bool with_consumption) {
  const uint32_t end = status.start_unix + STATUS_INTERVAL_S;
  LocalTime lt = to_local(end);
  if (lt.minute == 0) {
    // Ends at local midnight: report it as the last minute of its own day.
    lt = to_local(end - 1);
    lt.minute = 23 * 60 + 59;
  }
  const double hours = STATUS_INTERVAL_S / 3600.0;
  char buf[160];
  int n = snprintf(buf, sizeof(buf), "%08u,%02u:%02u,%.0f,%.0f", static_cast<unsigned>(lt.date), lt.minute / 60,
                   lt.minute % 60, status.day_generation_wh, status.generation_wh / hours);
  if (with_consumption) {
    n += snprintf(buf + n, sizeof(buf) - n, ",%.0f,%.0f", status.day_consumption_wh, status.consumption_wh / hours);
  } else {
    n += snprintf(buf + n, sizeof(buf) - n, ",-1,-1");
  }
  if (status.voltage_seconds > 0) {
    snprintf(buf + n, sizeof(buf) - n, ",-1,%.1f", status.volt_seconds / status.voltage_seconds);
  }
  return buf;
}

int count_status_results(const std::string &body) {
  // Each result is "yyyymmdd,hh:mm,0|1", separated by ';'.
  int count = 0;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find(';', pos);
    if (end == std::string::npos)
      end = body.size();
    const std::string item = body.substr(pos, end - pos);
    if (!item.empty() && item.find_first_not_of(" \r\n") != std::string::npos) {
      if (std::count(item.begin(), item.end(), ',') != 2)
        return -1;
      count++;
    }
    pos = end + 1;
  }
  return count;
}

}  // namespace esphome::wattledger
