#include "rv1805.h"
#include "esphome/core/log.h"

// Register map and configuration keys: Micro Crystal RV-1805-C3 application
// manual; setup values follow the SparkFun RV-1805 library (MIT).

namespace esphome::rv1805 {

static const char *const TAG = "rv1805";

static const uint8_t REG_HUNDREDTHS = 0x00;
static const uint8_t REG_CONTROL1 = 0x10;
static const uint8_t REG_OSC_CONTROL = 0x1C;
static const uint8_t REG_OSC_STATUS = 0x1D;
static const uint8_t REG_CONFIG_KEY = 0x1F;
static const uint8_t REG_TRICKLE = 0x20;
static const uint8_t REG_CAP_RC = 0x26;
static const uint8_t REG_IO_BATMODE = 0x27;
static const uint8_t REG_ID0 = 0x28;

static const uint8_t PART_NUMBER_UPPER = 0x18;

static const uint8_t CONTROL1_STOP = 1 << 7;
static const uint8_t CONTROL1_12_24 = 1 << 6;
static const uint8_t CONTROL1_WRTC = 1 << 0;
static const uint8_t OSC_STATUS_OF = 1 << 1;

// Each key unlocks one write to the protected register(s) it covers.
static const uint8_t KEY_OSC_CONTROL = 0xA1;
static const uint8_t KEY_CONFIG = 0x9D;

// Crystal while on main power; autocalibrate the RC oscillator every 512 s;
// switch to the calibrated RC oscillator on backup power or crystal failure;
// disable the I2C interface on backup power.
static const uint8_t OSC_CONTROL_VALUE = 0b01111100;
// Trickle charger on, 0.3 V Schottky diode, 3 kOhm.
static const uint8_t TRICKLE_ON = 0xA5;
static const uint8_t TRICKLE_OFF = 0x00;
static const uint8_t CAP_RC_ENABLE = 0xA0;
static const uint8_t IO_BATMODE_OFF = 0x00;

static uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void RV1805Component::setup() {
  uint8_t part;
  if (!this->read_byte(REG_ID0, &part)) {
    ESP_LOGE(TAG, "No response from the RTC");
    this->mark_failed();
    return;
  }
  if (part != PART_NUMBER_UPPER) {
    ESP_LOGE(TAG, "Unexpected part number 0x%02X, expected 0x%02X", part, PART_NUMBER_UPPER);
    this->mark_failed();
    return;
  }
  if (!this->configure_()) {
    ESP_LOGE(TAG, "Can't configure the RTC");
    this->mark_failed();
    return;
  }
  this->read_time();
}

void RV1805Component::update() { this->read_time(); }

void RV1805Component::dump_config() {
  ESP_LOGCONFIG(TAG, "RV-1805:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication with the RTC failed");
  }
  ESP_LOGCONFIG(TAG, "  Trickle charge: %s", YESNO(this->trickle_charge_));
  RealTimeClock::dump_config();
}

bool RV1805Component::configure_() {
  uint8_t control1;
  if (!this->read_byte(REG_CONTROL1, &control1))
    return false;
  if (control1 & CONTROL1_STOP) {
    ESP_LOGW(TAG, "Clock was stopped; its time is invalid until the next sync");
    this->counters_trusted_ = false;
  }
  if (control1 & CONTROL1_12_24) {
    // Hours were stored in 12-hour format (the SparkFun library's default) and
    // read wrongly in 24-hour mode until the time is written again.
    ESP_LOGW(TAG, "Clock was in 12-hour mode; its time is invalid until the next sync");
    this->counters_trusted_ = false;
  }
  control1 = (control1 & ~(CONTROL1_STOP | CONTROL1_12_24)) | CONTROL1_WRTC;
  return this->write_byte(REG_CONTROL1, control1) &&
         this->write_protected_(KEY_OSC_CONTROL, REG_OSC_CONTROL, OSC_CONTROL_VALUE) &&
         this->write_protected_(KEY_CONFIG, REG_TRICKLE, this->trickle_charge_ ? TRICKLE_ON : TRICKLE_OFF) &&
         this->write_protected_(KEY_CONFIG, REG_CAP_RC, CAP_RC_ENABLE) &&
         this->write_protected_(KEY_CONFIG, REG_IO_BATMODE, IO_BATMODE_OFF);
}

bool RV1805Component::write_protected_(uint8_t key, uint8_t reg, uint8_t value) {
  return this->write_byte(REG_CONFIG_KEY, key) && this->write_byte(reg, value);
}

void RV1805Component::read_time() {
  uint8_t osc_status;
  uint8_t regs[8];
  if (!this->read_byte(REG_OSC_STATUS, &osc_status) ||
      this->read_register(REG_HUNDREDTHS, regs, sizeof(regs)) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Can't read the time");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();

  if (!this->counters_trusted_ || (osc_status & OSC_STATUS_OF)) {
    ESP_LOGW(TAG, "RTC time is invalid (lost backup power or never set); waiting for another time source");
    return;
  }

  ESPTime rtc_time{};
  rtc_time.second = bcd_to_dec(regs[1] & 0x7F);
  rtc_time.minute = bcd_to_dec(regs[2] & 0x7F);
  rtc_time.hour = bcd_to_dec(regs[3] & 0x3F);
  rtc_time.day_of_month = bcd_to_dec(regs[4] & 0x3F);
  rtc_time.month = bcd_to_dec(regs[5] & 0x1F);
  rtc_time.year = bcd_to_dec(regs[6]) + 2000;
  rtc_time.day_of_week = (regs[7] & 0x07) + 1;
  rtc_time.recalc_timestamp_utc(false);
  if (!rtc_time.is_valid(false, false)) {
    ESP_LOGE(TAG, "RTC holds an invalid date/time: %04u-%02u-%02u %02u:%02u:%02u", rtc_time.year, rtc_time.month,
             rtc_time.day_of_month, rtc_time.hour, rtc_time.minute, rtc_time.second);
    return;
  }
  ESP_LOGD(TAG, "Read UTC %04u-%02u-%02u %02u:%02u:%02u", rtc_time.year, rtc_time.month, rtc_time.day_of_month,
           rtc_time.hour, rtc_time.minute, rtc_time.second);
  this->synchronize_epoch_(rtc_time.timestamp);
}

void RV1805Component::write_time() {
  ESPTime now = this->utcnow();
  if (!now.is_valid()) {
    ESP_LOGE(TAG, "System time is invalid; not writing it to the RTC");
    return;
  }
  uint8_t regs[8] = {
      0,
      dec_to_bcd(now.second),
      dec_to_bcd(now.minute),
      dec_to_bcd(now.hour),
      dec_to_bcd(now.day_of_month),
      dec_to_bcd(now.month),
      dec_to_bcd(now.year - 2000),
      static_cast<uint8_t>(now.day_of_week - 1),
  };
  uint8_t osc_status;
  if (this->write_register(REG_HUNDREDTHS, regs, sizeof(regs)) != i2c::ERROR_OK ||
      !this->read_byte(REG_OSC_STATUS, &osc_status) ||
      !this->write_byte(REG_OSC_STATUS, osc_status & ~OSC_STATUS_OF)) {
    ESP_LOGE(TAG, "Can't write the time");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
  this->counters_trusted_ = true;
  ESP_LOGD(TAG, "Wrote UTC %04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day_of_month, now.hour,
           now.minute, now.second);
}

}  // namespace esphome::rv1805
