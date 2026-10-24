#include "wattledger.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

#include "sealing.h"

namespace esphome::wattledger {

static const char *const TAG = "wattledger";

/// Integration steps are taken on every input update; between updates the
/// loop still advances at least this often, so stale inputs are noticed.
static const uint32_t MAX_STEP_MS = 1000;
static const char *const PARTITION_LABEL = "wattledger";
/// Rows per history API response, to bound the response's RAM.
static const uint32_t MAX_ROWS = 60;

void WattLedgerComponent::add_input(const char *name, sensor::Sensor *sensor) {
  this->inputs_.push_back({name, sensor});
}

void WattLedgerComponent::add_line(const char *name, sensor::Sensor *sensor) {
  this->lines_.push_back({name, sensor});
}

void WattLedgerComponent::add_output(const char *name, OutputFn fn, sensor::Sensor *power,
                                     sensor::Sensor *import_energy, sensor::Sensor *export_energy) {
  this->outputs_.push_back({name, power, import_energy, export_energy});
  this->output_fns_.push_back(fn);
}

void WattLedgerComponent::setup() {
  this->accumulator_.configure(this->inputs_.size(), this->lines_.size(), this->output_fns_);
  this->input_w_.assign(this->inputs_.size(), NAN);
  this->input_seen_ms_.assign(this->inputs_.size(), 0);
  this->line_v_.assign(this->lines_.size(), NAN);
  this->line_v_scratch_.assign(this->lines_.size(), NAN);
  this->line_seen_ms_.assign(this->lines_.size(), 0);
  this->layout_ = IntervalLayout(this->inputs_.size(), this->layout_.output_slots(), this->lines_.size());
  this->day_layout_ = DayLayout(this->inputs_.size(), this->layout_.output_slots(), this->lines_.size());
  this->record_buffer_.assign(std::max(this->layout_.size(), this->day_layout_.size()), 0);

  if (this->import_price_sensor_ != nullptr) {
    this->import_price_sensor_->add_on_state_callback([this](float price) {
      this->last_import_price_ = price;
      this->import_price_seen_unix_ = this->unix_now_();
    });
  }
  if (this->export_price_sensor_ != nullptr) {
    this->export_price_sensor_->add_on_state_callback([this](float price) {
      this->last_export_price_ = price;
      this->export_price_seen_unix_ = this->unix_now_();
    });
  }

  const uint32_t now = millis();
  this->last_sample_ms_ = now;
  this->last_millis_ = now;
  this->published_ = this->accumulator_.totals();

#ifdef USE_ESP32
  if (!this->storage_.open(PARTITION_LABEL)) {
    ESP_LOGE(TAG, "No '%s' partition; recording disabled", PARTITION_LABEL);
  } else if (!this->journal_.begin(&this->storage_,
                                   {static_cast<uint32_t>(this->layout_.size()), this->min1_records_,
                                    this->min5_records_, this->event_records_,
                                    static_cast<uint32_t>(this->day_layout_.size()), this->day_records_,
                                    this->layout_identity_})) {
    ESP_LOGE(TAG, "Can't open the journal (partition too small or flash error); recording disabled");
  } else {
    this->journal_ok_ = true;
    if (this->journal_.started_new())
      ESP_LOGW(TAG, "Started a new journal (first boot or record layout changed)");
  }
#endif
  if (this->journal_ok_) {
    Ring &events = this->journal_.events();
    uint8_t raw[EVENT_RECORD_SIZE];
    EventRecord last;
    if (events.next_seq() > 1 && events.read(events.next_seq() - 1, raw)) {
      decode_event(raw, last);
      this->boot_id_ = last.boot_id + 1;
    } else {
      this->boot_id_ = 1;
    }
    this->load_anchors_();
    this->write_event_(EVENT_BOOT, 0, RECORD_VERSION, this->defs_hash_);
    this->min1_.ring = &this->journal_.min1();
    this->min5_.ring = &this->journal_.min5();
  }
  this->min1_.flags = FLAG_FIRST_AFTER_BOOT;
  this->min5_.flags = FLAG_FIRST_AFTER_BOOT;

  // Raw state: the driver's values before any display filter (throttle, average).
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    this->inputs_[i].sensor->add_on_raw_state_callback([this, i](float value) {
      const uint32_t now = millis();
      this->advance_(now);
      this->input_w_[i] = value;
      this->input_seen_ms_[i] = now;
    });
  }
  for (size_t l = 0; l < this->lines_.size(); l++) {
    this->lines_[l].sensor->add_on_raw_state_callback([this, l](float value) {
      const uint32_t now = millis();
      this->advance_(now);
      this->line_v_[l] = value;
      this->line_seen_ms_[l] = now;
    });
  }
}

uint32_t WattLedgerComponent::unix_now_() {
  if (this->time_ == nullptr)
    return 0;
  const ESPTime now = this->time_->utcnow();
  return now.is_valid() ? static_cast<uint32_t>(now.timestamp) : 0;
}

void WattLedgerComponent::loop() {
  const uint32_t now = millis();
  this->uptime_ms_ += now - this->last_millis_;
  this->last_millis_ = now;
  if (now - this->last_sample_ms_ >= MAX_STEP_MS)
    this->advance_(now);

  const uint32_t unix_now = this->unix_now_();
  this->track_time_(unix_now);

  for (Tier *tier : {&this->min1_, &this->min5_}) {
    if (!tier->open) {
      this->open_tier_(*tier, unix_now);
      continue;
    }
    const bool due = tier->timed ? unix_now / tier->length_s != tier->start / tier->length_s
                                 : this->uptime_ms_ - tier->start_uptime_ms >= tier->length_s * 1000ULL;
    if (due) {
      this->close_tier_(*tier);
      this->open_tier_(*tier, unix_now);
    }
  }

  if (now - this->last_price_update_ms_ >= 1000) {
    this->last_price_update_ms_ = now;
    this->update_prices_(unix_now);
  }
  if (unix_now != 0 && this->journal_ok_ && now - this->last_seal_check_ms_ >= 10000) {
    this->last_seal_check_ms_ = now;
    this->seal_days_(unix_now);
  }
}

void WattLedgerComponent::update_prices_(uint32_t unix_now) {
  if (this->accumulator_.cost_output() < 0)
    return;
  Prices p;
  const bool known = std::isfinite(this->last_import_price_) &&
                     (this->export_price_sensor_ == nullptr || std::isfinite(this->last_export_price_));
  bool fresh = known;
#ifdef USE_API
  // A Home Assistant price is current only while Home Assistant is connected.
  fresh = fresh && api::global_api_server != nullptr && api::global_api_server->is_connected();
#endif
  // Each price must be current on its own: an export price that keeps
  // updating doesn't keep a stalled import price live.
  fresh = fresh && price_current(this->import_price_seen_unix_, unix_now, this->max_price_age_s_);
  if (this->export_price_sensor_ != nullptr)
    fresh = fresh && price_current(this->export_price_seen_unix_, unix_now, this->max_price_age_s_);
  const float export_price = std::isfinite(this->last_export_price_) ? this->last_export_price_ : 0.0f;
  if (fresh) {
    p = Prices{this->last_import_price_, export_price, Prices::LIVE};
  } else if (!this->schedule_.empty() && unix_now != 0 &&
             this->schedule_.lookup(local_time(unix_now).minute, p.import_per_kwh, p.export_per_kwh)) {
    p.source = Prices::SCHEDULE;
  } else if (known) {
    p = Prices{this->last_import_price_, export_price, Prices::STALE};
  }
  // With no price at all, cost isn't accumulated (source NONE).
  this->prices_ = p;
}

void WattLedgerComponent::seal_days_(uint32_t unix_now) {
  if (this->next_seal_day_ == 0) {
    // Continue after the newest sealed day, or start with the day of the
    // oldest 5-minute record.
    std::lock_guard<std::mutex> lock(this->journal_mutex_);
    Ring &days = this->journal_.days();
    if (days.next_seq() > 1 && days.read(days.next_seq() - 1, this->record_buffer_.data())) {
      DayRecord last;
      this->day_layout_.decode(this->record_buffer_.data(), last);
      this->next_seal_day_ = last.day_start + last.length_s;
    } else {
      Ring &min5 = this->journal_.min5();
      IntervalRecord rec;
      for (uint32_t seq = min5.first_seq(); seq < min5.next_seq(); seq++) {
        if (!min5.read(seq, this->record_buffer_.data()))
          continue;
        this->layout_.decode(this->record_buffer_.data(), rec);
        const uint32_t start =
            (rec.flags & FLAG_UNTIMED) ? this->anchors_.resolve(rec.boot_id, rec.start) : rec.start;
        if (start != 0) {
          this->next_seal_day_ = local_time(start).day_start;
          break;
        }
      }
    }
    if (this->next_seal_day_ == 0)
      return;
  }

  const uint32_t day_start = this->next_seal_day_;
  const uint32_t day_end = next_local_midnight(day_start);
  if (unix_now < day_end + this->seal_delay_s_)
    return;

  DaySealer sealer(this->inputs_.size(), this->layout_.output_slots(), this->lines_.size(),
                   local_time(day_start).date, day_start, day_end, this->defs_hash_);
  {
    std::lock_guard<std::mutex> lock(this->journal_mutex_);
    Ring &min5 = this->journal_.min5();
    IntervalRecord rec;
    // Newest first; stop well before the day (two hours' margin for clock steps).
    for (uint32_t seq = min5.next_seq(); seq > min5.first_seq();) {
      seq--;
      if (!min5.read(seq, this->record_buffer_.data()))
        continue;
      this->layout_.decode(this->record_buffer_.data(), rec);
      const uint32_t start = (rec.flags & FLAG_UNTIMED) ? this->anchors_.resolve(rec.boot_id, rec.start) : rec.start;
      if (start == 0)
        continue;
      if (start + rec.duration_s + 7200 < day_start)
        break;
      sealer.add(rec, start);
    }
  }
  const DayRecord day = sealer.finish(this->supply_charge_);
  this->day_layout_.encode(day, this->record_buffer_.data());
  {
    std::lock_guard<std::mutex> lock(this->journal_mutex_);
    if (this->journal_.days().append(this->record_buffer_.data()) == 0) {
      ESP_LOGE(TAG, "Can't write the day record for %" PRIu32, day.date);
      return;
    }
  }
  ESP_LOGI(TAG, "Sealed %" PRIu32 ": %u records, %" PRIu32 " s covered, cost %.2f, credit %.2f", day.date,
           day.records, day.coverage_s, day.cost / 1e6, day.credit / 1e6);
  this->next_seal_day_ = day_end;
}

void WattLedgerComponent::load_cost_today_(uint32_t unix_now) {
  this->today_start_ = local_time(unix_now).day_start;
  this->cost_today_base_ = 0;
  this->credit_today_base_ = 0;
  // Records of today written before this boot.
  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  Ring &min5 = this->journal_.min5();
  IntervalRecord rec;
  for (uint32_t seq = min5.next_seq(); seq > min5.first_seq();) {
    seq--;
    if (!min5.read(seq, this->record_buffer_.data()))
      continue;
    this->layout_.decode(this->record_buffer_.data(), rec);
    if (rec.boot_id == this->boot_id_)
      continue;
    const uint32_t start = (rec.flags & FLAG_UNTIMED) ? this->anchors_.resolve(rec.boot_id, rec.start) : rec.start;
    if (start == 0)
      continue;
    if (start < this->today_start_)
      break;
    this->cost_today_base_ += rec.cost / 1e6;
    this->credit_today_base_ += rec.credit / 1e6;
  }
}

void WattLedgerComponent::track_time_(uint32_t unix_now) {
  if (unix_now == 0)
    return;
  if (!this->anchored_) {
    // First valid time this boot: lets readers place this boot's untimed
    // intervals (design 6.2). Untimed intervals end here.
    this->anchored_ = true;
    {
      std::lock_guard<std::mutex> lock(this->journal_mutex_);
      this->anchors_.add(this->boot_id_, static_cast<uint32_t>(this->uptime_ms_ / 1000), unix_now);
    }
    this->write_event_(EVENT_ANCHOR, unix_now, 0, 0);
    this->restart_tiers_(unix_now, 0);
  } else {
    // Valid time advances with uptime, give or take a second for its
    // whole-second resolution; more than that is a clock step (design 6.3).
    const double elapsed = (this->uptime_ms_ - this->last_unix_uptime_ms_) / 1000.0;
    const double expected = this->last_unix_ + elapsed;
    if (std::fabs(unix_now - expected) >= 2.0) {
      ESP_LOGW(TAG, "Clock stepped by %.0f s", unix_now - expected);
      this->write_event_(EVENT_CLOCK_STEP, unix_now, static_cast<uint32_t>(expected), 0);
      this->restart_tiers_(unix_now, FLAG_CLOCK_STEP);
    }
  }
  this->last_unix_ = unix_now;
  this->last_unix_uptime_ms_ = this->uptime_ms_;
}

void WattLedgerComponent::restart_tiers_(uint32_t unix_now, uint8_t flags) {
  for (Tier *tier : {&this->min1_, &this->min5_}) {
    if (!tier->open)
      continue;
    tier->flags |= flags;
    this->close_tier_(*tier);
    this->open_tier_(*tier, unix_now);
  }
}

void WattLedgerComponent::load_anchors_() {
  Ring &events = this->journal_.events();
  uint8_t raw[EVENT_RECORD_SIZE];
  for (uint32_t seq = events.first_seq(); seq < events.next_seq(); seq++) {
    if (!events.read(seq, raw))
      continue;
    EventRecord ev;
    decode_event(raw, ev);
    if (ev.type == EVENT_ANCHOR)
      this->anchors_.add(ev.boot_id, ev.mono_s, ev.unix_s);
  }
}

bool WattLedgerComponent::inputs_valid_(uint32_t now) const {
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    if (!std::isfinite(this->input_w_[i]) || now - this->input_seen_ms_[i] > this->stale_timeout_ms_)
      return false;
  }
  return true;
}

void WattLedgerComponent::advance_(uint32_t now) {
  const uint32_t dt = now - this->last_sample_ms_;
  if (dt == 0)
    return;
  // A line voltage that stopped updating counts as missing, like an input.
  for (size_t l = 0; l < this->line_v_.size(); l++) {
    this->line_v_scratch_[l] = now - this->line_seen_ms_[l] > this->stale_timeout_ms_ ? NAN : this->line_v_[l];
  }
  this->accumulator_.integrate(this->input_w_.data(), this->line_v_scratch_.data(), dt, this->inputs_valid_(now),
                               this->prices_);
  this->last_sample_ms_ = now;
}

std::vector<int64_t> WattLedgerComponent::rounded_mwh_(const Totals &totals) const {
  std::vector<int64_t> mwh;
  mwh.reserve(2 * (totals.input_import_wh.size() + totals.output_import_wh.size()));
  for (size_t i = 0; i < totals.input_import_wh.size(); i++) {
    mwh.push_back(llround(totals.input_import_wh[i] * 1000.0));
    mwh.push_back(llround(totals.input_export_wh[i] * 1000.0));
  }
  for (size_t o = 0; o < totals.output_import_wh.size(); o++) {
    mwh.push_back(llround(totals.output_import_wh[o] * 1000.0));
    mwh.push_back(llround(totals.output_export_wh[o] * 1000.0));
  }
  return mwh;
}

void WattLedgerComponent::open_tier_(Tier &tier, uint32_t unix_now) {
  this->advance_(millis());
  tier.open = true;
  tier.timed = unix_now != 0;
  tier.start = tier.timed ? unix_now : static_cast<uint32_t>(this->uptime_ms_ / 1000);
  tier.start_uptime_ms = this->uptime_ms_;
  tier.start_totals = this->accumulator_.totals();
  tier.start_mwh = this->rounded_mwh_(tier.start_totals);
  tier.start_cost = llround(tier.start_totals.cost * 1e6);
  tier.start_credit = llround(tier.start_totals.credit * 1e6);
}

void WattLedgerComponent::close_tier_(Tier &tier) {
  this->advance_(millis());
  const Totals &totals = this->accumulator_.totals();
  const Totals d = delta(totals, tier.start_totals);
  const std::vector<int64_t> end_mwh = this->rounded_mwh_(totals);
  auto energy = [&](size_t k) -> uint32_t {
    const int64_t v = end_mwh[k] - tier.start_mwh[k];
    return v < 0 ? 0 : static_cast<uint32_t>(v);
  };

  IntervalRecord rec;
  rec.flags = tier.flags | (tier.timed ? 0 : FLAG_UNTIMED);
  // Gaps under a second (sampling jitter) don't make an interval partial.
  if (d.valid_ms + 1000 < d.total_ms)
    rec.flags |= FLAG_PARTIAL;
  rec.boot_id = this->boot_id_;
  rec.defs_hash = this->defs_hash_;
  rec.start = tier.start;
  rec.duration_s = static_cast<uint16_t>((this->uptime_ms_ - tier.start_uptime_ms + 500) / 1000);
  rec.coverage_s = static_cast<uint16_t>((d.valid_ms + 500) / 1000);
  const size_t inputs = this->inputs_.size();
  for (size_t i = 0; i < inputs; i++) {
    rec.input_import_mwh.push_back(energy(2 * i));
    rec.input_export_mwh.push_back(energy(2 * i + 1));
  }
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    rec.output_import_mwh.push_back(energy(2 * inputs + 2 * o));
    rec.output_export_mwh.push_back(energy(2 * inputs + 2 * o + 1));
  }
  for (size_t l = 0; l < this->lines_.size(); l++) {
    const uint32_t ms = d.line_valid_ms[l];
    rec.line_dv.push_back(ms == 0 ? 0 : static_cast<uint16_t>(lround(d.line_volt_seconds[l] * 10000.0 / ms)));
  }
  if (this->accumulator_.cost_output() >= 0) {
    rec.cost = static_cast<int32_t>(llround(totals.cost * 1e6) - tier.start_cost);
    rec.credit = static_cast<int32_t>(llround(totals.credit * 1e6) - tier.start_credit);
    rec.price_import = this->prices_.import_per_kwh;
    rec.price_export = this->prices_.export_per_kwh;
    if (d.price_stale_ms > 0)
      rec.flags |= FLAG_PRICE_STALE;
    if (d.price_schedule_ms > 0)
      rec.flags |= FLAG_PRICE_SCHEDULE;
  }
  tier.flags = 0;

  ESP_LOGD(TAG, "%s interval: start %" PRIu32 "%s, %u s, %u s valid", tier.name, rec.start,
           tier.timed ? "" : " (untimed)", rec.duration_s, rec.coverage_s);
  if (tier.ring == nullptr)
    return;
  this->layout_.encode(rec, this->record_buffer_.data());
  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  if (tier.ring->append(this->record_buffer_.data()) == 0)
    ESP_LOGE(TAG, "Can't write a %s record", tier.name);
}

void WattLedgerComponent::write_event_(uint8_t type, uint32_t unix_s, uint32_t a, uint32_t b) {
  if (!this->journal_ok_)
    return;
  EventRecord ev;
  ev.type = type;
  ev.boot_id = this->boot_id_;
  ev.mono_s = static_cast<uint32_t>(this->uptime_ms_ / 1000);
  ev.unix_s = unix_s;
  ev.a = a;
  ev.b = b;
  uint8_t raw[EVENT_RECORD_SIZE];
  encode_event(ev, raw);
  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  if (this->journal_.events().append(raw) == 0)
    ESP_LOGE(TAG, "Can't write an event");
}

void WattLedgerComponent::update() {
  this->advance_(millis());
  const Totals &totals = this->accumulator_.totals();
  const Totals d = delta(totals, this->published_);
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    const Output &out = this->outputs_[o];
    if (out.power != nullptr) {
      // Average over the valid time since the last update, not the latest sample.
      const float power = d.valid_ms > 0 ? d.output_net_wh(o) * 3600000.0 / d.valid_ms : NAN;
      out.power->publish_state(power);
    }
    if (out.import_energy != nullptr)
      out.import_energy->publish_state(totals.output_import_wh[o] / 1000.0);
    if (out.export_energy != nullptr)
      out.export_energy->publish_state(totals.output_export_wh[o] / 1000.0);
  }
  this->published_ = totals;

  const uint32_t unix_now = this->unix_now_();
  if ((this->cost_today_sensor_ != nullptr || this->credit_today_sensor_ != nullptr) && unix_now != 0 &&
      this->journal_ok_) {
    const uint32_t today = local_time(unix_now).day_start;
    if (this->today_start_ == 0) {
      // First valid time: today's cost before this boot comes from the journal.
      this->load_cost_today_(unix_now);
    } else if (today != this->today_start_) {
      this->today_start_ = today;
      this->cost_today_base_ = 0;
      this->credit_today_base_ = 0;
      this->cost_at_today_start_ = totals.cost;
      this->credit_at_today_start_ = totals.credit;
    }
    if (this->cost_today_sensor_ != nullptr)
      this->cost_today_sensor_->publish_state(this->cost_today_base_ + totals.cost - this->cost_at_today_start_);
    if (this->credit_today_sensor_ != nullptr)
      this->credit_today_sensor_->publish_state(this->credit_today_base_ + totals.credit -
                                                this->credit_at_today_start_);
  }
}

static std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20)
      out += c;
  }
  return out;
}

static void append_names(std::string &out, const char *key, const std::vector<const char *> &names) {
  out += "\"";
  out += key;
  out += "\":[";
  for (size_t i = 0; i < names.size(); i++) {
    if (i > 0)
      out += ",";
    out += "\"";
    out += names[i];
    out += "\"";
  }
  out += "]";
}

std::string WattLedgerComponent::info_json() {
  std::vector<const char *> inputs, outputs, lines;
  for (const Input &i : this->inputs_)
    inputs.push_back(i.name);
  for (const Output &o : this->outputs_)
    outputs.push_back(o.name);
  for (const Input &l : this->lines_)
    lines.push_back(l.name);

  char buf[160];
  // Identifies the device to the integration's config flow.
  char mac[MAC_ADDRESS_BUFFER_SIZE];
  get_mac_address_into_buffer(mac);
  std::string out = "{\"name\":\"" + json_escape(App.get_name().str()) + "\",\"friendly_name\":\"" +
                    json_escape(App.get_friendly_name().str()) + "\",\"mac\":\"" + mac + "\",";
  snprintf(buf, sizeof(buf),
           "\"version\":%u,\"boot_id\":%u,\"defs_hash\":%" PRIu32 ",\"time\":%" PRIu32 ",\"uptime_s\":%" PRIu32
           ",\"output_slots\":%u,\"cost\":%s,",
           RECORD_VERSION, this->boot_id_, this->defs_hash_, this->unix_now_(),
           static_cast<uint32_t>(this->uptime_ms_ / 1000), static_cast<unsigned>(this->layout_.output_slots()),
           this->accumulator_.cost_output() >= 0 ? "true" : "false");
  out += buf;
  append_names(out, "inputs", inputs);
  out += ",";
  append_names(out, "outputs", outputs);
  out += ",";
  append_names(out, "lines", lines);
  out += ",\"tiers\":{";
  if (this->journal_ok_) {
    std::lock_guard<std::mutex> lock(this->journal_mutex_);
    const struct {
      const char *name;
      Ring *ring;
      uint32_t interval_s;
    } tiers[] = {{"min1", &this->journal_.min1(), 60},
                 {"min5", &this->journal_.min5(), 300},
                 {"events", &this->journal_.events(), 0},
                 {"days", &this->journal_.days(), 86400}};
    for (size_t t = 0; t < 4; t++) {
      snprintf(buf, sizeof(buf),
               "%s\"%s\":{\"first_seq\":%" PRIu32 ",\"next_seq\":%" PRIu32 ",\"capacity\":%" PRIu32
               ",\"interval_s\":%" PRIu32 "}",
               t > 0 ? "," : "", tiers[t].name, tiers[t].ring->first_seq(), tiers[t].ring->next_seq(),
               tiers[t].ring->capacity(), tiers[t].interval_s);
      out += buf;
    }
  }
  out += "}}";
  return out;
}

void WattLedgerComponent::append_interval_json_(const IntervalRecord &rec, std::string &out) const {
  // Untimed records are placed through their boot's anchor; 0 if it has none.
  const uint32_t start_unix =
      (rec.flags & FLAG_UNTIMED) ? this->anchors_.resolve(rec.boot_id, rec.start) : rec.start;
  char buf[112];
  snprintf(buf, sizeof(buf), "[%" PRIu32 ",%u,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u,[", rec.seq, rec.flags,
           rec.boot_id, rec.defs_hash, rec.start, start_unix, rec.duration_s, rec.coverage_s);
  out += buf;
  for (size_t i = 0; i < rec.input_import_mwh.size(); i++) {
    snprintf(buf, sizeof(buf), "%s%" PRIu32 ",%" PRIu32, i > 0 ? "," : "", rec.input_import_mwh[i],
             rec.input_export_mwh[i]);
    out += buf;
  }
  out += "],[";
  // Only the configured outputs, not spare slots.
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    snprintf(buf, sizeof(buf), "%s%" PRIu32 ",%" PRIu32, o > 0 ? "," : "", rec.output_import_mwh[o],
             rec.output_export_mwh[o]);
    out += buf;
  }
  out += "],[";
  for (size_t l = 0; l < rec.line_dv.size(); l++) {
    snprintf(buf, sizeof(buf), "%s%u", l > 0 ? "," : "", rec.line_dv[l]);
    out += buf;
  }
  snprintf(buf, sizeof(buf), "],%g,%g,%" PRId32 ",%" PRId32 "]", rec.price_import, rec.price_export, rec.cost,
           rec.credit);
  out += buf;
}

void WattLedgerComponent::append_day_json_(const DayRecord &rec, std::string &out) const {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "[%" PRIu32 ",%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",[",
           rec.seq, rec.flags, rec.date, rec.day_start, rec.length_s, rec.coverage_s, rec.records, rec.price_stale_s,
           rec.price_schedule_s, rec.defs_hash);
  out += buf;
  for (size_t i = 0; i < rec.input_import_mwh.size(); i++) {
    snprintf(buf, sizeof(buf), "%s%" PRIu32 ",%" PRIu32, i > 0 ? "," : "", rec.input_import_mwh[i],
             rec.input_export_mwh[i]);
    out += buf;
  }
  out += "],[";
  for (size_t o = 0; o < this->outputs_.size(); o++) {
    snprintf(buf, sizeof(buf), "%s%" PRIu32 ",%" PRIu32, o > 0 ? "," : "", rec.output_import_mwh[o],
             rec.output_export_mwh[o]);
    out += buf;
  }
  out += "],[";
  for (size_t l = 0; l < rec.line_dv.size(); l++) {
    snprintf(buf, sizeof(buf), "%s%u", l > 0 ? "," : "", rec.line_dv[l]);
    out += buf;
  }
  snprintf(buf, sizeof(buf), "],%" PRId32 ",%" PRId32 ",%" PRId32 "]", rec.cost, rec.credit, rec.supply_charge);
  out += buf;
}

bool WattLedgerComponent::rows_json(const std::string &tier, uint32_t from_seq, uint32_t limit, std::string &out) {
  if (!this->journal_ok_)
    return false;
  Ring *ring;
  if (tier == "min1") {
    ring = &this->journal_.min1();
  } else if (tier == "min5") {
    ring = &this->journal_.min5();
  } else if (tier == "events") {
    ring = &this->journal_.events();
  } else if (tier == "days") {
    ring = &this->journal_.days();
  } else {
    return false;
  }
  if (limit == 0 || limit > MAX_ROWS)
    limit = MAX_ROWS;

  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  std::vector<uint8_t> raw(ring->record_size());
  uint32_t seq = std::max(from_seq, ring->first_seq());
  const uint32_t end = ring->next_seq();
  out = "{\"tier\":\"" + tier + "\",\"rows\":[";
  uint32_t count = 0;
  char buf[96];
  for (; seq < end && count < limit; seq++) {
    // Missing sequence numbers (damaged records) are skipped; readers see the gap.
    if (!ring->read(seq, raw.data()))
      continue;
    if (count > 0)
      out += ",";
    if (ring == &this->journal_.events()) {
      EventRecord ev;
      decode_event(raw.data(), ev);
      snprintf(buf, sizeof(buf), "[%" PRIu32 ",%u,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]", ev.seq,
               ev.type, ev.boot_id, ev.mono_s, ev.unix_s, ev.a, ev.b);
      out += buf;
    } else if (ring == &this->journal_.days()) {
      DayRecord day;
      this->day_layout_.decode(raw.data(), day);
      this->append_day_json_(day, out);
    } else {
      IntervalRecord rec;
      this->layout_.decode(raw.data(), rec);
      this->append_interval_json_(rec, out);
    }
    count++;
  }
  snprintf(buf, sizeof(buf), "],\"next_from_seq\":%" PRIu32 ",\"next_seq\":%" PRIu32 "}", seq, end);
  out += buf;
  return true;
}

bool WattLedgerComponent::min5_range(uint32_t &first_seq, uint32_t &next_seq) {
  if (!this->journal_ok_)
    return false;
  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  first_seq = this->journal_.min5().first_seq();
  next_seq = this->journal_.min5().next_seq();
  return true;
}

bool WattLedgerComponent::read_min5(uint32_t seq, IntervalRecord &rec, uint32_t &start_unix) {
  if (!this->journal_ok_)
    return false;
  std::lock_guard<std::mutex> lock(this->journal_mutex_);
  Ring &ring = this->journal_.min5();
  std::vector<uint8_t> raw(ring.record_size());
  if (!ring.read(seq, raw.data()))
    return false;
  this->layout_.decode(raw.data(), rec);
  start_unix = (rec.flags & FLAG_UNTIMED) ? this->anchors_.resolve(rec.boot_id, rec.start) : rec.start;
  return true;
}

void WattLedgerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "WattLedger:");
  ESP_LOGCONFIG(TAG, "  Definitions hash: 0x%08" PRIX32, this->defs_hash_);
  ESP_LOGCONFIG(TAG, "  Stale timeout: %" PRIu32 " ms", this->stale_timeout_ms_);
  for (const Input &input : this->inputs_)
    ESP_LOGCONFIG(TAG, "  Input %s", input.name);
  for (const Input &line : this->lines_)
    ESP_LOGCONFIG(TAG, "  Line %s", line.name);
  for (const Output &output : this->outputs_)
    ESP_LOGCONFIG(TAG, "  Output %s", output.name);
  if (this->journal_ok_) {
    ESP_LOGCONFIG(TAG, "  Journal: boot %u, record %u bytes, %" PRIu32 " + %" PRIu32 " records", this->boot_id_,
                  static_cast<unsigned>(this->layout_.size()), this->journal_.min1().capacity(),
                  this->journal_.min5().capacity());
  } else {
    ESP_LOGCONFIG(TAG, "  Journal: unavailable");
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::wattledger
