#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome::rv3028 {

/// Micro Crystal RV-3028-C7 real-time clock (as on the PiicoDev RTC board).
///
/// Uses the chip's 32-bit Unix-time counter, which keeps UTC. Its time is
/// used only while the power-on-reset flag is clear, so a clock that lost
/// its backup power never sets the system time.
class RV3028Component : public time::RealTimeClock, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  void set_trickle_charge(bool trickle_charge) { this->trickle_charge_ = trickle_charge; }
  void set_write_from(time::RealTimeClock *source) { this->write_from_ = source; }

  /// Set the system time from the RTC, if the RTC's time is valid.
  void read_time();
  /// Write the system time to the RTC and mark the RTC's time valid.
  void write_time();

 protected:
  /// Sets backup switchover and the trickle charger in the configuration
  /// EEPROM, which the chip reloads into RAM every 24 h and at power-on.
  bool configure_backup_();
  bool wait_eeprom_();
  bool read_unix_(uint32_t &unix_time);

  bool trickle_charge_{true};
  time::RealTimeClock *write_from_{nullptr};
};

}  // namespace esphome::rv3028
