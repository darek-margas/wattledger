#pragma once

// Fallback time-of-use tariff for when live prices aren't available (design 8.1).

#include <cstdint>
#include <vector>

namespace esphome::wattledger {

class PriceSchedule {
 public:
  /// A period from `from_min` to `to_min` (minutes since local midnight,
  /// end exclusive); it wraps past midnight when `to_min` <= `from_min`.
  void add(uint16_t from_min, uint16_t to_min, double import_per_kwh, double export_per_kwh) {
    this->periods_.push_back({from_min, to_min, import_per_kwh, export_per_kwh});
  }
  bool empty() const { return this->periods_.empty(); }
  /// Prices at `minute` since local midnight; false if no period covers it.
  bool lookup(uint16_t minute, double &import_per_kwh, double &export_per_kwh) const {
    for (const Period &p : this->periods_) {
      const bool inside = p.from_min < p.to_min ? (minute >= p.from_min && minute < p.to_min)
                                                : (minute >= p.from_min || minute < p.to_min);
      if (inside) {
        import_per_kwh = p.import_per_kwh;
        export_per_kwh = p.export_per_kwh;
        return true;
      }
    }
    return false;
  }

 protected:
  struct Period {
    uint16_t from_min;
    uint16_t to_min;
    double import_per_kwh;
    double export_per_kwh;
  };
  std::vector<Period> periods_;
};

}  // namespace esphome::wattledger
