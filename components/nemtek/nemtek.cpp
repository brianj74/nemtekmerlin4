#include "nemtek.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/uart.h>

#include "esphome/components/uart/uart_component_esp_idf.h"
#include "esphome/core/application.h"
#include "hal/uart_ll.h"
#endif

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

static constexpr uint8_t FLAG_PARITY = 0x01;
static constexpr uint8_t FLAG_FRAME = 0x02;
static constexpr uint8_t FLAG_OVERFLOW = 0x04;

// ESP-IDF UART int_st bits (TRM): bit2 parity, bit3 framing, bit4 RX FIFO overflow.
// The hardware does not store these flags beside the FIFO byte. The driver's ISR
// reports them on the select callback just before it clears the status, so the flag
// is attached to the next byte read on the main loop. Treat that link as best-effort;
// the P/F/O totals on the CAP header are the exact counts.
static constexpr uint32_t UART_STATUS_PARITY = 1u << 2;
static constexpr uint32_t UART_STATUS_FRAME = 1u << 3;
static constexpr uint32_t UART_STATUS_OVERFLOW = 1u << 4;

static volatile uint32_t g_parity_errors = 0;
static volatile uint32_t g_frame_errors = 0;
static volatile uint32_t g_overflow_errors = 0;

#ifdef USE_ESP32
static void IRAM_ATTR nemtek_uart_notif(uart_port_t uart_num, uart_select_notif_t notif, BaseType_t *task_woken) {
  if (notif == UART_SELECT_ERROR_NOTIF) {
    const uint32_t status = uart_ll_get_intsts_mask(UART_LL_GET_HW(uart_num));
    if ((status & UART_STATUS_PARITY) != 0)
      g_parity_errors++;
    if ((status & UART_STATUS_FRAME) != 0)
      g_frame_errors++;
    if ((status & UART_STATUS_OVERFLOW) != 0)
      g_overflow_errors++;
  }
  // ESPHome's wake-on-RX callback is a single function pointer. This hook replaces
  // it and has to keep waking the loop, or bytes sit in the driver for a select timeout.
  if ((notif == UART_SELECT_READ_NOTIF || notif == UART_SELECT_ERROR_NOTIF) && task_woken != nullptr)
    esphome::Application::wake_loop_isrsafe(task_woken);
}
#endif

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
  this->prepare_uart_();

  while (this->available()) {
    uint8_t discard;
    this->read_byte(&discard);
  }
  this->sync_error_counters_();

  if (this->capture_switch_ != nullptr) {
    auto initial = this->capture_switch_->get_initial_state_with_restore_mode();
    const bool on = initial.has_value() && initial.value();
    this->set_capture_enabled(on);
    this->capture_switch_->publish_state(on);
  }
  if (this->mark_text_ != nullptr)
    this->mark_text_->publish_state("");
}

void NemtekComponent::prepare_uart_() {
  if (this->uart_prepared_)
    return;
  this->uart_prepared_ = true;
  if (this->parent_ == nullptr)
    return;

  // One-byte RX threshold so each byte is released as it arrives. At 2400 baud
  // that is an interrupt about every 4.6 ms, which the loop can drain.
  this->set_rx_full_threshold(1);
  this->set_rx_timeout(1);
  ESP_LOGI(TAG, "UART RX full threshold %u byte(s), RX timeout %u",
           static_cast<unsigned>(this->get_rx_full_threshold()),
           static_cast<unsigned>(this->get_rx_timeout()));

#ifdef USE_ESP32
  auto *idf = static_cast<uart::IDFUARTComponent *>(this->parent_);
  const uart_port_t port = static_cast<uart_port_t>(idf->get_hw_serial_number());
  if (port >= UART_NUM_MAX || !uart_is_driver_installed(port)) {
    ESP_LOGW(TAG, "Parity/framing flags unavailable (hardware UART driver not ready)");
    return;
  }
  uart_set_select_notif_callback(port, nemtek_uart_notif);
  this->error_hook_ok_ = true;
  ESP_LOGI(TAG, "UART port %u error hook installed (parity, framing, RX overflow)",
           static_cast<unsigned>(port));
#else
  ESP_LOGI(TAG, "Parity/framing flags require the ESP32 hardware UART");
#endif
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
    // Keep the burst open until the wire is actually idle, and keep error totals.
    (void) this->take_error_flags_();
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
  this->flags_[index] = this->take_error_flags_();
  this->burst_len_++;
  this->last_rx_us_ = now_us;
  this->have_rx_ = true;
}

uint8_t NemtekComponent::take_error_flags_() {
  const uint32_t parity = g_parity_errors;
  const uint32_t frame = g_frame_errors;
  const uint32_t overflow = g_overflow_errors;
  const uint32_t d_parity = parity - this->parity_seen_;
  const uint32_t d_frame = frame - this->frame_seen_;
  const uint32_t d_overflow = overflow - this->overflow_seen_;
  this->parity_seen_ = parity;
  this->frame_seen_ = frame;
  this->overflow_seen_ = overflow;
  this->burst_parity_ += d_parity;
  this->burst_frame_ += d_frame;
  this->burst_overflow_ += d_overflow;

  uint8_t flags = 0;
  if (d_parity != 0)
    flags |= FLAG_PARITY;
  if (d_frame != 0)
    flags |= FLAG_FRAME;
  if (d_overflow != 0)
    flags |= FLAG_OVERFLOW;
  return flags;
}

void NemtekComponent::sync_error_counters_() {
  this->parity_seen_ = g_parity_errors;
  this->frame_seen_ = g_frame_errors;
  this->overflow_seen_ = g_overflow_errors;
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
  if (this->burst_len_ < 5 || this->burst_[0] != 0x80 || this->burst_[1] != 0xFB)
    return false;

  bool df_present = false;
  bool eb = false;
  for (size_t i = 2; i < this->burst_len_; i++) {
    df_present |= this->burst_[i] == 0xDF;
    eb |= this->burst_[i] == 0xEB;
  }
  *eb_present = eb;

  const uint8_t s0 = this->burst_[2];
  const uint8_t s1 = this->burst_[3];
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
    this->sync_error_counters_();
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
  char head[192];
  if (this->error_hook_ok_) {
    snprintf(head, sizeof(head),
             "CAP t_us=%lu gap_us=%lu n=%u sync=%u map=%s drop=%u P=%lu F=%lu O=%lu%s",
             static_cast<unsigned long>(this->burst_start_us_), static_cast<unsigned long>(this->burst_gap_us_),
             static_cast<unsigned>(this->burst_len_), sync ? 1 : 0, map_name,
             static_cast<unsigned>(this->burst_dropped_), static_cast<unsigned long>(this->burst_parity_),
             static_cast<unsigned long>(this->burst_frame_), static_cast<unsigned long>(this->burst_overflow_),
             batched ? " batch=1" : "");
  } else {
    snprintf(head, sizeof(head),
             "CAP t_us=%lu gap_us=%lu n=%u sync=%u map=%s drop=%u err=n/a%s",
             static_cast<unsigned long>(this->burst_start_us_), static_cast<unsigned long>(this->burst_gap_us_),
             static_cast<unsigned>(this->burst_len_), sync ? 1 : 0, map_name,
             static_cast<unsigned>(this->burst_dropped_), batched ? " batch=1" : "");
  }
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
    if ((this->flags_[i] & FLAG_PARITY) != 0 && tlen + 2 < sizeof(token)) {
      token[tlen++] = '!';
      token[tlen++] = 'P';
      token[tlen] = '\0';
    }
    if ((this->flags_[i] & FLAG_FRAME) != 0 && tlen + 2 < sizeof(token)) {
      token[tlen++] = '!';
      token[tlen++] = 'F';
      token[tlen] = '\0';
    }
    if ((this->flags_[i] & FLAG_OVERFLOW) != 0 && tlen + 2 < sizeof(token)) {
      token[tlen++] = '!';
      token[tlen++] = 'O';
      token[tlen] = '\0';
    }
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
  ESP_LOGCONFIG(TAG, "  Capture error flags: %s", this->error_hook_ok_ ? "parity, framing, overflow" : "unavailable");
  if (this->mark_text_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Mark text: '%s'", this->mark_text_->get_name().c_str());
  }
  LOG_SWITCH("  ", "Capture", this->capture_switch_);
  LOG_BINARY_SENSOR("  ", "Communications", this->communications_sensor_);
}

}  // namespace nemtek
}  // namespace esphome
