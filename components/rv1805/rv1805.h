#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome::rv1805 {

/// Micro Crystal RV-1805 real-time clock (as on the SparkFun Qwiic RTC board).
///
/// The clock keeps UTC. Its time is used only while the oscillator-failure flag
/// is clear, so a clock that lost its backup power never sets the system time.
class RV1805Component : public time::RealTimeClock, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  void set_trickle_charge(bool trickle_charge) { this->trickle_charge_ = trickle_charge; }

  /// Set the system time from the RTC, if the RTC's time is valid.
  void read_time();
  /// Write the system time to the RTC and mark the RTC's time valid.
  void write_time();

 protected:
  bool configure_();
  bool write_protected_(uint8_t key, uint8_t reg, uint8_t value);

  bool trickle_charge_{true};
  /// False when the counters can't be trusted until the next write_time(),
  /// for example after the clock was found stopped or in 12-hour mode.
  bool counters_trusted_{true};
};

}  // namespace esphome::rv1805
