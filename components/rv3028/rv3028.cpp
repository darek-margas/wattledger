#include "rv3028.h"

#include <cinttypes>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

// Register map and EEPROM commands: Micro Crystal RV-3028-C7 application
// manual. Backup and trickle settings follow the PiicoDev RV3028 library.

namespace esphome::rv3028 {

static const char *const TAG = "rv3028";

static const uint8_t REG_STATUS = 0x0E;
static const uint8_t REG_CONTROL1 = 0x0F;
static const uint8_t REG_UNIX = 0x1B;  // 4 bytes, least significant first
static const uint8_t REG_EE_ADDRESS = 0x25;
static const uint8_t REG_EE_DATA = 0x26;
static const uint8_t REG_EE_COMMAND = 0x27;
static const uint8_t REG_ID = 0x28;
/// Configuration RAM mirror of the EEPROM backup register (and its EEPROM address).
static const uint8_t REG_EE_BACKUP = 0x37;

static const uint8_t HARDWARE_ID = 0x3;  // upper nibble of REG_ID

static const uint8_t STATUS_PORF = 1 << 0;
static const uint8_t STATUS_EEBUSY = 1 << 7;
/// Disables the automatic EEPROM refresh while the EEPROM is accessed.
static const uint8_t CONTROL1_EERD = 1 << 3;

static const uint8_t EE_COMMAND_FIRST = 0x00;
static const uint8_t EE_COMMAND_WRITE_BYTE = 0x21;
static const uint8_t EE_COMMAND_READ_BYTE = 0x22;

// Backup register: TCE (trickle charge) bit 5, BSM (backup switchover) bits
// 3:2, TCR (trickle resistor) bits 1:0. The other bits are kept.
static const uint8_t BACKUP_TCE = 1 << 5;
static const uint8_t BACKUP_BSM_MASK = 0b11 << 2;
static const uint8_t BACKUP_BSM_DIRECT = 0b01 << 2;
static const uint8_t BACKUP_TCR_MASK = 0b11;
static const uint8_t BACKUP_TCR_3K = 0b00;

void RV3028Component::setup() {
  uint8_t id;
  if (!this->read_byte(REG_ID, &id)) {
    ESP_LOGE(TAG, "No response from the RTC");
    this->mark_failed();
    return;
  }
  if ((id >> 4) != HARDWARE_ID) {
    ESP_LOGE(TAG, "Unexpected ID 0x%02X", id);
    this->mark_failed();
    return;
  }
  if (!this->configure_backup_())
    ESP_LOGW(TAG, "Can't configure backup switchover; the time may not survive a power cut");
  if (this->write_from_ != nullptr) {
    this->write_from_->add_on_time_sync_callback([this]() { this->write_time(); });
  }
  this->read_time();
}

void RV3028Component::update() { this->read_time(); }

void RV3028Component::dump_config() {
  ESP_LOGCONFIG(TAG, "RV-3028:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication with the RTC failed");
  }
  ESP_LOGCONFIG(TAG, "  Trickle charge: %s", YESNO(this->trickle_charge_));
  RealTimeClock::dump_config();
}

bool RV3028Component::wait_eeprom_() {
  for (int i = 0; i < 100; i++) {
    uint8_t status;
    if (!this->read_byte(REG_STATUS, &status))
      return false;
    if (!(status & STATUS_EEBUSY))
      return true;
    delay(1);
  }
  return false;
}

bool RV3028Component::configure_backup_() {
  uint8_t control1;
  if (!this->read_byte(REG_CONTROL1, &control1) || !this->write_byte(REG_CONTROL1, control1 | CONTROL1_EERD))
    return false;
  bool ok = false;
  uint8_t stored;
  if (this->wait_eeprom_() && this->write_byte(REG_EE_ADDRESS, REG_EE_BACKUP) &&
      this->write_byte(REG_EE_COMMAND, EE_COMMAND_FIRST) && this->write_byte(REG_EE_COMMAND, EE_COMMAND_READ_BYTE) &&
      this->wait_eeprom_() && this->read_byte(REG_EE_DATA, &stored)) {
    uint8_t wanted = (stored & ~(BACKUP_TCE | BACKUP_BSM_MASK | BACKUP_TCR_MASK)) | BACKUP_BSM_DIRECT;
    if (this->trickle_charge_)
      wanted |= BACKUP_TCE | BACKUP_TCR_3K;
    ok = this->write_byte(REG_EE_BACKUP, wanted);
    // Only write the EEPROM when it differs, to save its endurance.
    if (ok && wanted != stored) {
      ESP_LOGI(TAG, "Storing backup configuration 0x%02X in EEPROM (was 0x%02X)", wanted, stored);
      ok = this->write_byte(REG_EE_DATA, wanted) && this->write_byte(REG_EE_COMMAND, EE_COMMAND_FIRST) &&
           this->write_byte(REG_EE_COMMAND, EE_COMMAND_WRITE_BYTE) && this->wait_eeprom_();
    }
  }
  return this->write_byte(REG_CONTROL1, control1 & ~CONTROL1_EERD) && ok;
}

bool RV3028Component::read_unix_(uint32_t &unix_time) {
  // The counter can carry between byte reads: read until two reads agree.
  uint32_t previous = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t b[4];
    if (this->read_register(REG_UNIX, b, sizeof(b)) != i2c::ERROR_OK)
      return false;
    unix_time = b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
    if (i > 0 && unix_time == previous)
      return true;
    previous = unix_time;
  }
  return false;
}

void RV3028Component::read_time() {
  uint8_t status;
  uint32_t unix_time;
  if (!this->read_byte(REG_STATUS, &status) || !this->read_unix_(unix_time)) {
    ESP_LOGE(TAG, "Can't read the time");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
  if (status & STATUS_PORF) {
    ESP_LOGW(TAG, "RTC time is invalid (lost backup power or never set); waiting for another time source");
    return;
  }
  ESPTime rtc_time = ESPTime::from_epoch_utc(unix_time);
  if (!rtc_time.is_valid()) {
    ESP_LOGE(TAG, "RTC holds an invalid time: %" PRIu32, unix_time);
    return;
  }
  ESP_LOGD(TAG, "Read UTC %04u-%02u-%02u %02u:%02u:%02u", rtc_time.year, rtc_time.month, rtc_time.day_of_month,
           rtc_time.hour, rtc_time.minute, rtc_time.second);
  this->synchronize_epoch_(unix_time);
}

void RV3028Component::write_time() {
  ESPTime now = this->utcnow();
  if (!now.is_valid()) {
    ESP_LOGE(TAG, "System time is invalid; not writing it to the RTC");
    return;
  }
  const uint32_t unix_time = static_cast<uint32_t>(now.timestamp);
  const uint8_t b[4] = {static_cast<uint8_t>(unix_time), static_cast<uint8_t>(unix_time >> 8),
                        static_cast<uint8_t>(unix_time >> 16), static_cast<uint8_t>(unix_time >> 24)};
  uint8_t status;
  if (this->write_register(REG_UNIX, b, sizeof(b)) != i2c::ERROR_OK || !this->read_byte(REG_STATUS, &status) ||
      !this->write_byte(REG_STATUS, status & ~STATUS_PORF)) {
    ESP_LOGE(TAG, "Can't write the time");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
  ESP_LOGD(TAG, "Wrote UTC %04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day_of_month, now.hour,
           now.minute, now.second);
}

}  // namespace esphome::rv3028
