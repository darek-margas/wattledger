#pragma once

// Interval and event record layouts (design section 5.1).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::wattledger {

static const uint8_t RECORD_VERSION = 1;

enum RecordFlag : uint8_t {
  FLAG_UNTIMED = 1 << 0,           ///< `start` is monotonic seconds since boot
  FLAG_PARTIAL = 1 << 1,           ///< coverage < duration
  FLAG_PRICE_STALE = 1 << 2,       ///< cost used the last known price
  FLAG_PRICE_SCHEDULE = 1 << 3,    ///< cost used the fallback schedule
  FLAG_CLOCK_STEP = 1 << 4,        ///< interval cut short by a clock correction
  FLAG_FIRST_AFTER_BOOT = 1 << 5,  ///< first interval of this tier after boot
};

/// An interval's contents, decoded.
struct IntervalRecord {
  uint32_t seq{0};
  uint8_t flags{0};
  uint16_t boot_id{0};
  uint32_t defs_hash{0};
  uint32_t start{0};
  uint16_t duration_s{0};
  uint16_t coverage_s{0};
  /// mWh, import then export, per input / output slot.
  std::vector<uint32_t> input_import_mwh, input_export_mwh;
  std::vector<uint32_t> output_import_mwh, output_export_mwh;
  /// Average voltage per line, 0.1 V; 0 when the line had no valid voltage.
  std::vector<uint16_t> line_dv;
  float price_import{0}, price_export{0};
  /// 1e-6 currency units.
  int32_t cost{0}, credit{0};
};

/// Byte layout of an interval record, fixed per firmware build.
class IntervalLayout {
 public:
  IntervalLayout(size_t inputs, size_t output_slots, size_t lines)
      : inputs_(inputs), output_slots_(output_slots), lines_(lines) {}

  size_t size() const;
  size_t inputs() const { return this->inputs_; }
  size_t output_slots() const { return this->output_slots_; }
  size_t lines() const { return this->lines_; }

  /// Writes everything but magic, sequence number and CRC (set by the ring).
  void encode(const IntervalRecord &rec, uint8_t *out) const;
  void decode(const uint8_t *in, IntervalRecord &rec) const;

 protected:
  size_t inputs_;
  size_t output_slots_;
  size_t lines_;
};

enum EventType : uint8_t {
  EVENT_BOOT = 1,    ///< a: record format version
  EVENT_ANCHOR = 2,  ///< monotonic and Unix time at the first valid time of a boot
};

struct EventRecord {
  uint32_t seq{0};
  uint8_t type{0};
  uint16_t boot_id{0};
  uint32_t mono_s{0};
  uint32_t unix_s{0};
  uint32_t a{0}, b{0};
};

static const size_t EVENT_RECORD_SIZE = 32;

void encode_event(const EventRecord &ev, uint8_t *out);
void decode_event(const uint8_t *in, EventRecord &ev);

}  // namespace esphome::wattledger
