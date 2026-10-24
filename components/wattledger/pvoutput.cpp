#include "pvoutput.h"

#ifdef USE_WATTLEDGER_PVOUTPUT

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <ctime>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

namespace esphome::wattledger {

static const char *const TAG = "wattledger.pvoutput";
static const char *const URL = "https://pvoutput.org/service/r2/addbatchstatus.jsp";
static const char *const NVS_LIVE = "pvo_live";
static const char *const NVS_REPLAY = "pvo_replay";
/// Unix time before which the clock is considered unset.
static const uint32_t VALID_TIME = 1700000000;
static const uint32_t MAX_BACKOFF_S = 1800;
static const size_t MAX_RESPONSE = 4096;

void PVOutputUploader::setup() {
  if (nvs_open("wattledger", NVS_READWRITE, &this->nvs_) != ESP_OK) {
    ESP_LOGE(TAG, "Can't open NVS; uploads disabled");
    this->mark_failed();
    return;
  }
  size_t size = sizeof(Job);
  if (nvs_get_blob(this->nvs_, NVS_LIVE, &this->live_, &size) != ESP_OK || size != sizeof(Job)) {
    // First start: upload from now on. Older history is sent only by a replay,
    // so existing PVOutput data isn't overwritten unasked.
    uint32_t first, next;
    this->live_ = Job{};
    this->live_.active = 1;
    this->live_.cursor = this->ledger_->min5_range(first, next) ? next : 1;
    this->save_(NVS_LIVE, this->live_);
  }
  size = sizeof(Job);
  if (nvs_get_blob(this->nvs_, NVS_REPLAY, &this->replay_, &size) != ESP_OK || size != sizeof(Job))
    this->replay_ = Job{};

  // TLS needs a large stack.
  if (xTaskCreate(task_entry_, "pvoutput", 12288, this, 1, &this->task_) != pdPASS) {
    ESP_LOGE(TAG, "Can't start the upload task");
    this->mark_failed();
  }
}

void PVOutputUploader::task_entry_(void *arg) { static_cast<PVOutputUploader *>(arg)->run_(); }

void PVOutputUploader::run_() {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(this->backlog_ > 1 ? 2000 : 10000));
    if (this->replay_requested_.exchange(false))
      this->start_replay_();

    uint32_t first, next;
    if (this->ledger_->min5_range(first, next))
      this->backlog_ = next > this->live_.cursor ? next - this->live_.cursor : 0;
    if (this->replay_.active && this->replay_.end > this->replay_.start) {
      const uint32_t done = std::min(this->replay_.cursor, this->replay_.end) - this->replay_.start;
      this->replay_progress_ = 100 * done / (this->replay_.end - this->replay_.start);
    } else {
      this->replay_progress_ = -1;
    }

    const uint32_t now = ::time(nullptr);
    if (now < VALID_TIME || now < this->wait_until_ || !network::is_connected())
      continue;
    // Live data first; a replay uses what's left.
    if (!this->run_batch_(this->live_, false, now) && this->replay_.active)
      this->run_batch_(this->replay_, true, now);
  }
}

bool PVOutputUploader::place_(uint32_t seq, PlacedRecord &out, bool &wait) {
  wait = false;
  IntervalRecord rec;
  uint32_t start;
  if (!this->ledger_->read_min5(seq, rec, start))
    return false;  // damaged or overwritten: skipped
  if (start == 0) {
    // Untimed: wait if this boot may still get a valid time; otherwise it
    // can't be placed and is skipped.
    wait = rec.boot_id == this->ledger_->boot_id();
    return false;
  }
  out.seq = seq;
  out.start_unix = start;
  out.duration_s = rec.duration_s;
  out.generation_wh = rec.output_import_mwh[this->generation_] / 1000.0;
  out.consumption_wh = this->consumption_ >= 0 ? rec.output_import_mwh[this->consumption_] / 1000.0 : 0.0;
  out.voltage = (this->voltage_ >= 0 && rec.line_dv[this->voltage_] != 0) ? rec.line_dv[this->voltage_] / 10.0f : NAN;
  return true;
}

void PVOutputUploader::prime_day_(Job &job) {
  uint32_t first, next;
  if (!this->ledger_->min5_range(first, next))
    return;
  PlacedRecord p;
  bool wait;
  uint32_t ref = 0;
  for (uint32_t seq = job.cursor; seq < next && ref == 0; seq++) {
    if (this->place_(seq, p, wait))
      ref = p.start_unix;
    else if (wait)
      break;
  }
  if (ref == 0)
    ref = ::time(nullptr);
  DayTotals day{local_time(ref).day_start, 0, 0};
  for (uint32_t seq = job.cursor; seq > first;) {
    seq--;
    if (!this->place_(seq, p, wait))
      continue;
    if (p.start_unix + p.duration_s <= day.day_start)
      break;
    if (p.start_unix >= day.day_start && p.start_unix < ref) {
      day.generation_wh += p.generation_wh;
      day.consumption_wh += p.consumption_wh;
    }
  }
  job.day = day;
  job.sent_until = 0;
}

void PVOutputUploader::start_replay_() {
  uint32_t first, next;
  if (!this->ledger_->min5_range(first, next))
    return;
  // A replay starts at the first day whose first record is retained, so the
  // day's energy since midnight is complete.
  PlacedRecord p;
  bool wait;
  for (uint32_t seq = first; seq < this->live_.cursor && seq < next; seq++) {
    if (!this->place_(seq, p, wait))
      continue;
    const uint32_t day_start = local_time(p.start_unix).day_start;
    if (p.start_unix != day_start)
      continue;
    this->replay_ = Job{};
    this->replay_.active = 1;
    this->replay_.start = seq;
    this->replay_.cursor = seq;
    this->replay_.end = this->live_.cursor;
    this->replay_.day = DayTotals{day_start, 0, 0};
    this->save_(NVS_REPLAY, this->replay_);
    ESP_LOGI(TAG, "Replay of records %" PRIu32 "-%" PRIu32 " started", seq, this->replay_.end - 1);
    return;
  }
  this->set_error_("Replay: no complete day in the 5-minute journal");
}

bool PVOutputUploader::run_batch_(Job &job, bool replay, uint32_t now) {
  const char *key = replay ? NVS_REPLAY : NVS_LIVE;
  uint32_t first, next;
  if (!this->ledger_->min5_range(first, next))
    return false;
  if (job.cursor > next) {
    // The device started a new journal.
    job.cursor = first;
    job.day = DayTotals{};
    job.sent_until = 0;
  } else if (job.cursor < first) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%" PRIu32 " records were overwritten before upload", first - job.cursor);
    this->set_error_(msg);
    job.cursor = first;
    job.day = DayTotals{};
    job.sent_until = 0;
  }
  const uint32_t end = replay ? std::min(job.end, next) : next;
  if (job.cursor >= end) {
    if (replay) {
      ESP_LOGI(TAG, "Replay finished");
      job.active = 0;
      this->save_(key, job);
    }
    return false;
  }
  if (job.day.day_start == 0)
    this->prime_day_(job);

  const size_t batch = this->donor_ ? 100 : 30;
  std::vector<PlacedRecord> placed;
  uint32_t seq = job.cursor;
  bool wait = false;
  for (; seq < end && placed.size() < batch + 4; seq++) {
    PlacedRecord p;
    if (this->place_(seq, p, wait)) {
      placed.push_back(p);
    } else if (wait) {
      break;
    }
  }

  DayTotals day = job.day;
  uint32_t sent_until = job.sent_until;
  size_t consumed = 0;
  const std::vector<Status> statuses = build_statuses(placed, batch, local_time, day, sent_until, consumed);
  const uint32_t cursor = consumed < placed.size() ? placed[consumed].seq : seq;

  // PVOutput refuses statuses older than its history limit; keep a margin.
  const uint32_t oldest = now - (this->donor_ ? 90 : 14) * 86400 + 3600;
  std::string data;
  for (const Status &s : statuses) {
    if (s.start_unix < oldest)
      continue;
    if (!data.empty())
      data += ';';
    data += format_status(s, local_time, this->consumption_ >= 0);
  }

  auto commit = [&]() {
    const bool progress = cursor != job.cursor || sent_until != job.sent_until;
    job.cursor = cursor;
    job.day = day;
    job.sent_until = sent_until;
    if (progress)
      this->save_(key, job);
    return progress;
  };
  if (data.empty())
    return commit();

  std::string response;
  const int code = this->post_("data=" + data, response);
  bool progress = false;
  if (code == 200 && count_status_results(response) >= 0) {
    ESP_LOGD(TAG, "Sent %u statuses%s", static_cast<unsigned>(statuses.size()), replay ? " (replay)" : "");
    progress = commit();
    this->last_upload_ = now;
    this->backoff_s_ = 0;
    this->set_error_("");
  } else if (code == 403 && response.find("Exceeded") != std::string::npos) {
    this->set_error_(response);
    this->wait_until_ = this->rate_reset_ > now ? this->rate_reset_ + 5 : now + 3600 - now % 3600 + 5;
  } else if (code == 401 || code == 403) {
    // Bad key or system id: retry rarely until the configuration is fixed.
    this->set_error_(response);
    this->wait_until_ = now + 1800;
  } else if (code == 400) {
    // Rejected for good (e.g. a value too high for the system size): skip it.
    this->set_error_(response);
    progress = commit();
  } else {
    this->set_error_(code < 0 ? "Can't reach pvoutput.org" : response);
    this->backoff_s_ = this->backoff_s_ == 0 ? 30 : std::min(this->backoff_s_ * 2, MAX_BACKOFF_S);
    this->wait_until_ = now + this->backoff_s_;
  }
  // Leave some of the hourly allowance for other clients of this API key.
  const int32_t remaining = this->rate_remaining_;
  if (remaining >= 0 && static_cast<uint32_t>(remaining) <= this->reserve_ && this->rate_reset_ > now)
    this->wait_until_ = std::max(this->wait_until_, this->rate_reset_ + 5);
  return progress;
}

namespace {
struct HttpContext {
  std::string *body;
  std::atomic<int32_t> *remaining;
  std::atomic<uint32_t> *reset;
};

esp_err_t http_event(esp_http_client_event_t *evt) {
  auto *ctx = static_cast<HttpContext *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (strcasecmp(evt->header_key, "X-Rate-Limit-Remaining") == 0)
      *ctx->remaining = atoi(evt->header_value);
    else if (strcasecmp(evt->header_key, "X-Rate-Limit-Reset") == 0)
      *ctx->reset = strtoul(evt->header_value, nullptr, 10);
  } else if (evt->event_id == HTTP_EVENT_ON_DATA && ctx->body->size() < MAX_RESPONSE) {
    ctx->body->append(static_cast<const char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}
}  // namespace

int PVOutputUploader::post_(const std::string &body, std::string &response) {
  HttpContext ctx{&response, &this->rate_remaining_, &this->rate_reset_};
  esp_http_client_config_t config = {};
  config.url = URL;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 20000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = http_event;
  config.user_data = &ctx;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return -1;
  esp_http_client_set_header(client, "X-Pvoutput-Apikey", this->api_key_.c_str());
  esp_http_client_set_header(client, "X-Pvoutput-SystemId", this->system_id_.c_str());
  esp_http_client_set_header(client, "X-Rate-Limit", "1");
  esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  esp_http_client_set_post_field(client, body.c_str(), body.size());
  const esp_err_t err = esp_http_client_perform(client);
  const int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);
  if (code != 200)
    ESP_LOGW(TAG, "Upload failed (%d): %s", code, response.c_str());
  return code;
}

void PVOutputUploader::save_(const char *key, const Job &job) {
  if (nvs_set_blob(this->nvs_, key, &job, sizeof(Job)) != ESP_OK || nvs_commit(this->nvs_) != ESP_OK)
    ESP_LOGE(TAG, "Can't save the upload position");
}

void PVOutputUploader::set_error_(const std::string &error) {
  std::lock_guard<std::mutex> lock(this->error_mutex_);
  if (error != this->last_error_) {
    this->last_error_ = error.substr(0, 250);
    this->error_changed_ = true;
  }
}

void PVOutputUploader::update() {
  if (this->backlog_sensor_ != nullptr)
    this->backlog_sensor_->publish_state(this->backlog_);
  if (this->rate_limit_sensor_ != nullptr && this->rate_remaining_ >= 0)
    this->rate_limit_sensor_->publish_state(this->rate_remaining_);
  if (this->last_upload_sensor_ != nullptr && this->last_upload_ != 0)
    this->last_upload_sensor_->publish_state(this->last_upload_);
  if (this->replay_progress_sensor_ != nullptr) {
    const int32_t progress = this->replay_progress_;
    this->replay_progress_sensor_->publish_state(progress >= 0 ? progress : NAN);
  }
  if (this->last_error_sensor_ != nullptr) {
    std::lock_guard<std::mutex> lock(this->error_mutex_);
    if (this->error_changed_) {
      this->last_error_sensor_->publish_state(this->last_error_.empty() ? "OK" : this->last_error_);
      this->error_changed_ = false;
    }
  }
}

void PVOutputUploader::dump_config() {
  ESP_LOGCONFIG(TAG, "PVOutput:");
  ESP_LOGCONFIG(TAG, "  System ID: %s", this->system_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Donor: %s", YESNO(this->donor_));
  ESP_LOGCONFIG(TAG, "  Live position: record %" PRIu32, this->live_.cursor);
  if (this->replay_.active)
    ESP_LOGCONFIG(TAG, "  Replay position: record %" PRIu32 " of %" PRIu32, this->replay_.cursor, this->replay_.end);
}

}  // namespace esphome::wattledger

#endif  // USE_WATTLEDGER_PVOUTPUT
