#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "pvoutput_status.h"
#include "wattledger.h"

namespace esphome::wattledger {

/// Uploads 5-minute statuses to PVOutput directly (design section 7).
///
/// Runs in its own task, because an HTTPS request can take seconds and the
/// main loop must keep sampling. Two jobs share the code: live (from the
/// last uploaded record onwards) and replay (all complete days retained,
/// started from the button). Their positions are kept in NVS and advance
/// only when PVOutput accepted a batch.
class PVOutputUploader : public PollingComponent {
 public:
  explicit PVOutputUploader(WattLedgerComponent *ledger) : ledger_(ledger) {}

  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_system_id(const std::string &system_id) { this->system_id_ = system_id; }
  void set_donor(bool donor) { this->donor_ = donor; }
  void set_generation(int output) { this->generation_ = output; }
  void set_consumption(int output) { this->consumption_ = output; }
  void set_voltage(int line) { this->voltage_ = line; }
  void set_reserve_requests(uint32_t reserve) { this->reserve_ = reserve; }
  void set_backlog_sensor(sensor::Sensor *s) { this->backlog_sensor_ = s; }
  void set_rate_limit_sensor(sensor::Sensor *s) { this->rate_limit_sensor_ = s; }
  void set_last_upload_sensor(sensor::Sensor *s) { this->last_upload_sensor_ = s; }
  void set_replay_progress_sensor(sensor::Sensor *s) { this->replay_progress_sensor_ = s; }
  void set_last_error_sensor(text_sensor::TextSensor *s) { this->last_error_sensor_ = s; }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Starts (or restarts) a replay of every complete day retained.
  void request_replay() { this->replay_requested_ = true; }

 protected:
  /// A position in the 5-minute journal and the state needed to continue
  /// from it. Stored in NVS as a blob.
  struct Job {
    uint32_t version{1};
    uint32_t active{0};
    uint32_t start{0};   ///< replay only: first record, for progress
    uint32_t cursor{0};  ///< next record to send
    uint32_t end{0};     ///< replay only: stop before this record
    uint32_t sent_until{0};
    DayTotals day;
  };

  static void task_entry_(void *arg);
  void run_();
  /// Sends one batch for `job`. True if it made progress.
  bool run_batch_(Job &job, bool replay, uint32_t now);
  void start_replay_();
  /// Sums the energy of `job.day`'s records before `job.cursor`, so a job
  /// started mid-day reports energy since midnight.
  void prime_day_(Job &job);
  int post_(const std::string &body, std::string &response);
  void save_(const char *key, const Job &job);
  void set_error_(const std::string &error);
  static LocalTime to_local_(uint32_t unix_time);
  bool place_(uint32_t seq, PlacedRecord &out, bool &wait);

  WattLedgerComponent *ledger_;
  std::string api_key_;
  std::string system_id_;
  bool donor_{false};
  int generation_{0};
  int consumption_{-1};
  int voltage_{-1};
  uint32_t reserve_{10};

  nvs_handle_t nvs_{0};
  TaskHandle_t task_{nullptr};
  Job live_;
  Job replay_;
  std::atomic<bool> replay_requested_{false};
  uint32_t wait_until_{0};
  uint32_t backoff_s_{0};

  // Published from the main loop.
  std::atomic<uint32_t> backlog_{0};
  std::atomic<int32_t> rate_remaining_{-1};
  std::atomic<uint32_t> rate_reset_{0};
  std::atomic<uint32_t> last_upload_{0};
  std::atomic<int32_t> replay_progress_{-1};
  std::mutex error_mutex_;
  std::string last_error_;
  bool error_changed_{false};

  sensor::Sensor *backlog_sensor_{nullptr};
  sensor::Sensor *rate_limit_sensor_{nullptr};
  sensor::Sensor *last_upload_sensor_{nullptr};
  sensor::Sensor *replay_progress_sensor_{nullptr};
  text_sensor::TextSensor *last_error_sensor_{nullptr};
};

class ReplayButton : public button::Button, public Parented<PVOutputUploader> {
 protected:
  void press_action() override { this->parent_->request_replay(); }
};

}  // namespace esphome::wattledger

#endif  // USE_ESP32
