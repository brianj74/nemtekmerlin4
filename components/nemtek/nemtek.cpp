#include "nemtek.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nemtek {

static const char *const TAG = "nemtek";

// Back-to-back 2400 8E1 bytes are 11 bit times apart: 11/2400 s = 4583 us.
static constexpr uint32_t BURST_GAP_US = 25000;
static constexpr uint32_t COMMS_TIMEOUT_MS = 5000;
static constexpr uint32_t REPEAT_LOG_MS = 5000;
static constexpr uint32_t TIMING_MATCH_US = 2000;
static constexpr uint32_t BATCHED_DELTA_US = 250;
static constexpr uint8_t REQUIRED_MATCHES = 3;
static constexpr int MAX_BYTES_PER_LOOP = 64;

void NemtekCaptureSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_capture_enabled(state);
  this->publish_state(state);
}

void NemtekMarkText::control(const std::string &value) {
  if (this->parent_ != nullptr && !value.empty())
    this->parent_->mark(value);
  // Clear so the same label can be submitted again. publish_state does not call control.
  this->publish_state("");
}

void NemtekComponent::setup() {
  ESP_LOGI(TAG, "Starting receive-only Nemtek Merlin 4 monitor");
  ESP_LOGI(TAG, "Capture logs can contain keypad PIN digits. Keep them private.");
  this->burst_len_ = 0;
  this->set_comms_(false);

  while (this->available()) {
    uint8_t discard;
    this->read_byte(&discard);
  }

  if (this->capture_switch_ != nullptr) {
    auto initial = this->capture_switch_->get_initial_state_with_restore_mode();
    const bool on = initial.has_value() && initial.value();
    this->set_capture_enabled(on);
    this->capture_switch_->publish_state(on);
  }
  if (this->mark_text_ != nullptr)
    this->mark_text_->publish_state("");
}

void NemtekComponent::loop() {
  uint32_t now_us = micros();
  if (this->burst_len_ > 0 && static_cast<uint32_t>(now_us - this->last_rx_us_) > BURST_GAP_US)
    this->finish_burst_();

  // available() is checked first. read_byte() waits up to 100 ms when the buffer is empty.
  int budget = MAX_BYTES_PER_LOOP;
  while (budget-- > 0 && this->available()) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;
    this->ingest_(byte, micros());
  }

  now_us = micros();
  if (this->burst_len_ > 0 && static_cast<uint32_t>(now_us - this->last_rx_us_) > BURST_GAP_US)
    this->finish_burst_();

  const uint32_t now_ms = millis();
  if (this->comms_state_ && static_cast<uint32_t>(now_ms - this->last_valid_ms_) > COMMS_TIMEOUT_MS)
    this->lose_comms_();

  if (this->capture_enabled_ && this->capture_coalesce_ &&
      static_cast<uint32_t>(now_ms - this->last_repeat_log_ms_) >= REPEAT_LOG_MS) {
    this->flush_pending_repeats_(now_ms);
    this->last_repeat_log_ms_ = now_ms;
  }
}

void NemtekComponent::ingest_(uint8_t byte, uint32_t now_us) {
  if (this->burst_len_ > 0 && this->have_rx_ &&
      static_cast<uint32_t>(now_us - this->last_rx_us_) > BURST_GAP_US) {
    this->finish_burst_();
  }

  if (this->burst_len_ >= kBurstMax) {
    if (this->burst_dropped_ < 65535)
      this->burst_dropped_++;
    this->last_rx_us_ = now_us;
    this->have_rx_ = true;
    return;
  }

  uint32_t delta = 0;
  if (this->burst_len_ == 0) {
    this->burst_gap_us_ = this->have_rx_ ? static_cast<uint32_t>(now_us - this->last_rx_us_) : 0;
    this->burst_start_us_ = now_us;
  } else {
    delta = static_cast<uint32_t>(now_us - this->last_rx_us_);
  }

  const size_t index = this->burst_len_;
  this->burst_[index] = byte;
  this->delta_us_[index] = delta;
  this->flags_[index] = 0;
  this->burst_len_++;
  this->last_rx_us_ = now_us;
  this->have_rx_ = true;
}

void NemtekComponent::finish_burst_() {
  if (this->burst_len_ == 0)
    return;
  this->log_capture_();
  this->decode_burst_();
  this->burst_len_ = 0;
  this->burst_dropped_ = 0;
  this->burst_parity_ = 0;
  this->burst_frame_ = 0;
  this->burst_overflow_ = 0;
}

bool NemtekComponent::classify_(MerlinState *state, bool *eb_present) const {
  *state = MerlinState::UNKNOWN;
  *eb_present = false;
  // Idle fence-off on this Merlin 4 is the 3-byte frame 80 FB FF. A following
  // byte about 30 ms later is a separate burst (past the 25 ms gap) and is not
  // part of the state. Main required 5 bytes, so it only accepted off when a
  // slow read glued that later byte, and more, onto the frame.
  if (this->burst_len_ < 3 || this->burst_[0] != 0x80 || this->burst_[1] != 0xFB)
    return false;

  bool df_present = false;
  bool eb = false;
  for (size_t i = 2; i < this->burst_len_; i++) {
    df_present |= this->burst_[i] == 0xDF;
    eb |= this->burst_[i] == 0xEB;
  }
  *eb_present = eb;

  const uint8_t s0 = this->burst_[2];
  const uint8_t s1 = this->burst_len_ > 3 ? this->burst_[3] : 0x00;
  if (s0 == 0xF4 && s1 == 0xFE) {
    *state = MerlinState::ALARM;
  } else if (s0 == 0xFC && df_present) {
    *state = MerlinState::NORMAL;
  } else if (s0 == 0xFC && !df_present) {
    *state = MerlinState::LOW_POWER;
  } else if (s0 == 0xFF && df_present) {
    *state = MerlinState::VOLTAGE_LOW;
  } else if (s0 == 0xFF && !df_present) {
    *state = MerlinState::OFF;
  } else {
    return false;
  }
  return true;
}

void NemtekComponent::decode_burst_() {
  MerlinState state = MerlinState::UNKNOWN;
  bool eb_present = false;
  if (!this->classify_(&state, &eb_present))
    return;

  this->last_valid_ms_ = millis();
  this->set_comms_(true);
  this->consider_state_(state, eb_present);
}

void NemtekComponent::consider_state_(MerlinState state, bool eb_present) {
  if (state == this->candidate_state_) {
    if (this->candidate_count_ < 255)
      this->candidate_count_++;
  } else {
    this->candidate_state_ = state;
    this->candidate_count_ = 1;
  }

  // EB is treated as a provisional keypad-buzzer/warning marker. Update it
  // immediately when seen, but require debouncing before clearing it.
  if (eb_present && !this->current_eb_) {
    this->current_eb_ = true;
    this->publish_(this->buzzer_active_sensor_, true);
    ESP_LOGI(TAG, "Merlin 4 warning/buzzer marker detected (EB)");
  }

  if (this->candidate_count_ < REQUIRED_MATCHES)
    return;

  if (state != this->current_state_) {
    this->current_state_ = state;
    ESP_LOGI(TAG, "Merlin 4 state: %s", this->state_name_(state));
    this->publish_state_(state, eb_present);
  } else if (!eb_present && this->current_eb_) {
    this->current_eb_ = false;
    this->publish_(this->buzzer_active_sensor_, false);
  }
}

void NemtekComponent::publish_state_(MerlinState state, bool eb_present) {
  const bool normal = state == MerlinState::NORMAL;
  const bool low_power = state == MerlinState::LOW_POWER;
  const bool voltage_low = state == MerlinState::VOLTAGE_LOW || state == MerlinState::ALARM;
  const bool alarm = state == MerlinState::ALARM;
  const bool off = state == MerlinState::OFF;

  this->publish_(this->fence_off_sensor_, off);
  this->publish_(this->fence_low_power_sensor_, low_power);
  this->publish_(this->fence_volts_low_sensor_, voltage_low);
  this->publish_(this->fence_alarm_volts_sensor_, alarm);
  this->publish_(this->alarm_active_sensor_, alarm);

  // Reuse the Pico-era HV entities with Merlin 4 meanings.
  this->publish_(this->hv_good_sensor_, normal || low_power);
  this->publish_(this->hv_check_sensor_, state == MerlinState::VOLTAGE_LOW);
  this->publish_(this->hv_bad_sensor_, alarm);

  this->current_eb_ = eb_present;
  this->publish_(this->buzzer_active_sensor_, eb_present);
}

const char *NemtekComponent::map_token_(MerlinState state) const {
  switch (state) {
    case MerlinState::NORMAL:
      return "normal";
    case MerlinState::LOW_POWER:
      return "low-power";
    case MerlinState::VOLTAGE_LOW:
      return "voltage-low";
    case MerlinState::ALARM:
      return "alarm";
    case MerlinState::OFF:
      return "off";
    default:
      return "none";
  }
}

const char *NemtekComponent::state_name_(MerlinState state) const {
  switch (state) {
    case MerlinState::NORMAL:
      return "normal/high-power";
    case MerlinState::LOW_POWER:
      return "low-power";
    case MerlinState::VOLTAGE_LOW:
      return "fence voltage low";
    case MerlinState::ALARM:
      return "fence alarm";
    case MerlinState::OFF:
      return "off";
    default:
      return "unknown";
  }
}

void NemtekComponent::publish_(binary_sensor::BinarySensor *sensor, bool state) {
  if (sensor != nullptr && (!sensor->has_state() || sensor->state != state))
    sensor->publish_state(state);
}

void NemtekComponent::invalidate_(binary_sensor::BinarySensor *sensor) {
  if (sensor != nullptr && sensor->has_state())
    sensor->invalidate_state();
}

void NemtekComponent::lose_comms_() {
  ESP_LOGW(TAG, "No valid Merlin 4 traffic for %lu ms; fence sensors unavailable",
           static_cast<unsigned long>(COMMS_TIMEOUT_MS));
  this->set_comms_(false);
  this->current_state_ = MerlinState::UNKNOWN;
  this->candidate_state_ = MerlinState::UNKNOWN;
  this->candidate_count_ = 0;
  this->current_eb_ = false;

  // Communications stays OFF. That entity is the loss signal. The fence
  // readings are stale, so they become unavailable and keep their entity IDs.
  this->invalidate_(this->hv_good_sensor_);
  this->invalidate_(this->hv_check_sensor_);
  this->invalidate_(this->hv_bad_sensor_);
  this->invalidate_(this->fence_alarm_volts_sensor_);
  this->invalidate_(this->fence_low_power_sensor_);
  this->invalidate_(this->alarm_active_sensor_);
  this->invalidate_(this->fence_off_sensor_);
  this->invalidate_(this->fence_volts_low_sensor_);
  this->invalidate_(this->buzzer_active_sensor_);
}

void NemtekComponent::set_comms_(bool state) {
  this->comms_state_ = state;
  this->publish_(this->communications_sensor_, state);
}

void NemtekComponent::set_capture_enabled(bool enabled) {
  if (enabled == this->capture_enabled_)
    return;
  if (!enabled)
    this->flush_pending_repeats_(millis());
  this->capture_enabled_ = enabled;
  this->repeat_count_ = 0;
  this->last_len_ = 0;
  if (enabled) {
    this->last_repeat_log_ms_ = millis();
    ESP_LOGW(TAG,
             "Capture ON. Logs may contain keypad PIN digits. Keep logs private and redact them before sharing.");
  } else {
    ESP_LOGI(TAG, "Capture off");
  }
}

void NemtekComponent::mark(const std::string &label) {
  char buf[49];
  size_t n = 0;
  for (unsigned char c : label) {
    if (n + 1 >= sizeof(buf))
      break;
    if (c < 32 || c == 127)
      continue;
    buf[n++] = static_cast<char>(c);
  }
  size_t start = 0;
  while (start < n && buf[start] == ' ')
    start++;
  while (n > start && buf[n - 1] == ' ')
    n--;
  if (start >= n)
    return;
  if (start > 0) {
    std::memmove(buf, buf + start, n - start);
    n -= start;
  }
  buf[n] = '\0';

  if (this->capture_enabled_) {
    ESP_LOGI(TAG, "MARK: %s t_us=%lu", buf, static_cast<unsigned long>(micros()));
  } else {
    ESP_LOGI(TAG, "MARK: %s t_us=%lu (capture is off)", buf, static_cast<unsigned long>(micros()));
  }
}

bool NemtekComponent::timing_differs_() const {
  for (size_t i = 1; i < this->burst_len_; i++) {
    // A queued read has a near-zero delta. Ignore it so a late loop() does not
    // defeat coalescing. A real multi-millisecond hole still fails the match.
    if (this->delta_us_[i] < BATCHED_DELTA_US)
      continue;
    const uint32_t a = this->delta_us_[i];
    const uint32_t b = this->last_delta_us_[i];
    const uint32_t diff = a > b ? a - b : b - a;
    if (diff > TIMING_MATCH_US)
      return true;
  }
  return false;
}

bool NemtekComponent::same_pattern_() const {
  if (this->last_len_ == 0 || this->burst_len_ != this->last_len_)
    return false;
  if (this->burst_dropped_ != this->last_dropped_ || this->burst_parity_ != this->last_parity_ ||
      this->burst_frame_ != this->last_frame_ || this->burst_overflow_ != this->last_overflow_)
    return false;
  if (std::memcmp(this->burst_.data(), this->last_bytes_.data(), this->burst_len_) != 0)
    return false;
  if (std::memcmp(this->flags_.data(), this->last_flags_.data(), this->burst_len_) != 0)
    return false;
  return !this->timing_differs_();
}

void NemtekComponent::note_repeat_gap_() {
  if (this->repeat_count_ <= 1) {
    this->repeat_gap_min_us_ = this->burst_gap_us_;
    this->repeat_gap_max_us_ = this->burst_gap_us_;
  } else {
    if (this->burst_gap_us_ < this->repeat_gap_min_us_)
      this->repeat_gap_min_us_ = this->burst_gap_us_;
    if (this->burst_gap_us_ > this->repeat_gap_max_us_)
      this->repeat_gap_max_us_ = this->burst_gap_us_;
  }
}

void NemtekComponent::save_pattern_() {
  this->last_len_ = this->burst_len_;
  this->last_dropped_ = this->burst_dropped_;
  this->last_parity_ = this->burst_parity_;
  this->last_frame_ = this->burst_frame_;
  this->last_overflow_ = this->burst_overflow_;
  if (this->burst_len_ == 0)
    return;
  std::memcpy(this->last_bytes_.data(), this->burst_.data(), this->burst_len_);
  std::memcpy(this->last_delta_us_.data(), this->delta_us_.data(), this->burst_len_ * sizeof(uint32_t));
  std::memcpy(this->last_flags_.data(), this->flags_.data(), this->burst_len_);
}

void NemtekComponent::log_capture_() {
  if (!this->capture_enabled_ || this->burst_len_ == 0)
    return;

  if (this->capture_coalesce_ && this->same_pattern_()) {
    this->note_repeat_gap_();
    this->repeat_count_++;
    return;
  }

  if (this->capture_coalesce_)
    this->flush_pending_repeats_(millis());

  MerlinState state = MerlinState::UNKNOWN;
  bool eb_present = false;
  const bool mapped = this->classify_(&state, &eb_present);
  this->log_capture_full_(mapped ? this->map_token_(state) : "none");
  this->save_pattern_();
  this->repeat_count_ = 1;
  this->last_repeat_log_ms_ = millis();
}

void NemtekComponent::log_capture_full_(const char *map_name) {
  bool batched = false;
  for (size_t i = 1; i < this->burst_len_; i++) {
    if (this->delta_us_[i] < BATCHED_DELTA_US) {
      batched = true;
      break;
    }
  }

  const bool sync = this->burst_len_ >= 2 && this->burst_[0] == 0x80 && this->burst_[1] == 0xFB;
  char head[160];
  snprintf(head, sizeof(head), "CAP t_us=%lu gap_us=%lu n=%u sync=%u map=%s drop=%u%s",
           static_cast<unsigned long>(this->burst_start_us_), static_cast<unsigned long>(this->burst_gap_us_),
           static_cast<unsigned>(this->burst_len_), sync ? 1 : 0, map_name,
           static_cast<unsigned>(this->burst_dropped_), batched ? " batch=1" : "");
  ESP_LOGI(TAG, "%s", head);

  char line[180];
  size_t used = static_cast<size_t>(snprintf(line, sizeof(line), "CAP |"));
  if (used >= sizeof(line))
    used = sizeof(line) - 1;

  for (size_t i = 0; i < this->burst_len_; i++) {
    char token[24];
    size_t tlen = static_cast<size_t>(
        snprintf(token, sizeof(token), "%02X+%lu", this->burst_[i], static_cast<unsigned long>(this->delta_us_[i])));
    if (tlen >= sizeof(token))
      tlen = sizeof(token) - 1;
    if (i > 0 && this->delta_us_[i] < BATCHED_DELTA_US && tlen + 1 < sizeof(token)) {
      token[tlen++] = '*';
      token[tlen] = '\0';
    }

    if (used + 1 + tlen >= sizeof(line)) {
      ESP_LOGI(TAG, "%s", line);
      used = static_cast<size_t>(snprintf(line, sizeof(line), "CAP |"));
      if (used >= sizeof(line))
        used = sizeof(line) - 1;
    }
    line[used++] = ' ';
    std::memcpy(line + used, token, tlen);
    used += tlen;
    line[used] = '\0';
  }
  ESP_LOGI(TAG, "%s", line);
}

void NemtekComponent::log_repeat_(uint32_t extra_count) {
  char hex[96];
  size_t used = 0;
  const size_t preview = this->last_len_ > 24 ? 24 : this->last_len_;
  for (size_t i = 0; i < preview && used + 4 < sizeof(hex); i++) {
    if (i > 0)
      hex[used++] = ' ';
    used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used, "%02X", this->last_bytes_[i]));
  }
  if (this->last_len_ > preview && used + 4 < sizeof(hex)) {
    std::memcpy(hex + used, " ...", 4);
    used += 4;
  }
  hex[used] = '\0';

  ESP_LOGI(TAG, "CAP repeat x%lu gap_us=%lu-%lu n=%u %s", static_cast<unsigned long>(extra_count),
           static_cast<unsigned long>(this->repeat_gap_min_us_),
           static_cast<unsigned long>(this->repeat_gap_max_us_), static_cast<unsigned>(this->last_len_), hex);
}

void NemtekComponent::flush_pending_repeats_(uint32_t now_ms) {
  if (this->repeat_count_ <= 1 || this->last_len_ == 0)
    return;
  this->log_repeat_(this->repeat_count_ - 1);
  this->repeat_count_ = 1;
  this->last_repeat_log_ms_ = now_ms;
}

void NemtekComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Nemtek Merlin 4 energiser monitor:");
  ESP_LOGCONFIG(TAG, "  Direction: receive");
  ESP_LOGCONFIG(TAG, "  Protocol: legacy keypad bus, inverted UART, 2400 baud, 8E1");
  ESP_LOGCONFIG(TAG, "  State debounce: %u matching bursts", REQUIRED_MATCHES);
  ESP_LOGCONFIG(TAG, "  Comms timeout: %lu ms, then fence sensors unavailable",
                static_cast<unsigned long>(COMMS_TIMEOUT_MS));
  ESP_LOGCONFIG(TAG, "  Capture: %s, coalesce: %s", this->capture_switch_ != nullptr ? "switch" : "off",
                this->capture_coalesce_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  UART thresholds: left at the ESPHome UART defaults");
  if (this->mark_text_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Mark text: '%s'", this->mark_text_->get_name().c_str());
  }
  LOG_SWITCH("  ", "Capture", this->capture_switch_);
  LOG_BINARY_SENSOR("  ", "Communications", this->communications_sensor_);
}

}  // namespace nemtek
}  // namespace esphome
