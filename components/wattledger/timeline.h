#pragma once

// Placing records in wall-clock time (design section 6).
// Independent of ESPHome so it can be unit-tested on the host.

#include <cstdint>
#include <vector>

namespace esphome::wattledger {

/// Local calendar position of a Unix time.
struct LocalTime {
  uint32_t date;       ///< yyyymmdd
  uint16_t minute;     ///< minutes since local midnight
  uint32_t day_start;  ///< Unix time of local midnight
};

/// Local time in the process's time zone (set by the ESPHome time component).
LocalTime local_time(uint32_t unix_time);
/// Unix time of the local midnight that ends the day starting at `day_start`
/// (23 or 25 hours later on daylight-saving changes).
uint32_t next_local_midnight(uint32_t day_start);

/// The first valid time of each boot, from the journal's anchor events.
/// Untimed records (start = seconds since boot) are placed through it.
class AnchorTable {
 public:
  /// Records the anchor of `boot_id`; later anchors of the same boot are ignored.
  void add(uint16_t boot_id, uint32_t uptime_s, uint32_t unix_s);
  /// Unix time of `uptime_s` in boot `boot_id`, or 0 if that boot never
  /// got a valid time (its records can't be placed).
  uint32_t resolve(uint16_t boot_id, uint32_t uptime_s) const;
  size_t size() const { return this->anchors_.size(); }

 protected:
  struct Anchor {
    uint16_t boot_id;
    uint32_t uptime_s;
    uint32_t unix_s;
  };
  std::vector<Anchor> anchors_;
};

/// Calls fn(bucket_start, fraction) for every `bucket_s`-aligned bucket that
/// the interval [start, start + duration_s) overlaps, with the fraction of
/// the interval inside it; the fractions add up to 1. Used to realign
/// untimed (or clock-step shortened) intervals to wall-clock buckets, sharing
/// their energy in proportion to time.
template<typename F> void for_each_bucket(uint32_t start, uint32_t duration_s, uint32_t bucket_s, F fn) {
  if (duration_s == 0) {
    fn(start - start % bucket_s, 1.0);
    return;
  }
  const uint32_t end = start + duration_s;
  uint32_t t = start;
  while (t < end) {
    const uint32_t bucket = t - t % bucket_s;
    const uint32_t next = bucket + bucket_s < end ? bucket + bucket_s : end;
    fn(bucket, static_cast<double>(next - t) / duration_s);
    t = next;
  }
}

}  // namespace esphome::wattledger
