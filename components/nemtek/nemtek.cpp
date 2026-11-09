#include "nemtek.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/uart.h>

#include "esphome/components/uart/uart_component_esp_idf.h"
#include "esphome/core/application.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#endif

namespace esphome {
namespace nemtek {

static const char *const TAG = "nemtek";

static volatile uint32_t g_parity_errors = 0;
static volatile uint32_t g_frame_errors = 0;

static constexpr size_t kEdgeMax = 80;

struct EdgeSample {
  uint32_t t_us;
  uint8_t level;
};

#ifdef USE_ESP32
struct EdgeIsrState {
  volatile uint8_t open;
  volatile uint8_t pin;
  volatile uint32_t end_us;
  volatile uint16_t count;
  uint32_t t_us[kEdgeMax];
  uint8_t level[kEdgeMax];
};

static EdgeIsrState g_edge{};

static void IRAM_ATTR nemtek_edge_isr(void *arg) {
  (void) arg;
  if (g_edge.open == 0)
    return;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  if (static_cast<int32_t>(now - g_edge.end_us) >= 0) {
    g_edge.open = 0;
    return;
  }
  const uint16_t n = g_edge.count;
  if (n >= kEdgeMax)
    return;
  g_edge.t_us[n] = now;
  g_edge.level[n] = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(g_edge.pin)));
  g_edge.count = static_cast<uint16_t>(n + 1);
}

static void IRAM_ATTR nemtek_uart_notif(uart_port_t uart_num, uart_select_notif_t notif, BaseType_t *task_woken) {
  if (notif == UART_SELECT_ERROR_NOTIF) {
    const uint32_t status = uart_ll_get_intsts_mask(UART_LL_GET_HW(uart_num));
    if ((status & (1u << 2)) != 0)
      g_parity_errors++;
    if ((status & (1u << 3)) != 0)
      g_frame_errors++;
  }
  // Replaces ESPHome's single wake callback, so READ must still wake the loop.
  // The handler only counts. It does not flush the FIFO or discard bytes.
  if ((notif == UART_SELECT_READ_NOTIF || notif == UART_SELECT_ERROR_NOTIF) && task_woken != nullptr)
    esphome::Application::wake_loop_isrsafe(task_woken);
}
#endif

static int poll_index_(uint8_t byte) {
  switch (byte) {
    case 0x79:
      return 0;
    case 0xF8:
      return 1;
    case 0x27:
      return 2;
    case 0xF7:
      return 3;
    case 0x4F:
      return 4;
    case 0xFB:
      return 5;
    case 0x7A:
      return 6;
    default:
      return -1;
  }
}

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
  this->apply_parity_override_();
  this->install_uart_error_hook_();
  this->install_edge_isr_();
  this->tx_boot_();
  this->parity_seen_ = g_parity_errors;
  this->frame_seen_ = g_frame_errors;

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

  this->service_edge_window_(false);

  const uint32_t now_ms = millis();
  if (this->comms_state_ && static_cast<uint32_t>(now_ms - this->last_valid_ms_) > COMMS_TIMEOUT_MS)
    this->lose_comms_();

  if (this->capture_enabled_ && static_cast<uint32_t>(now_ms - this->last_summary_ms_) >= REPEAT_LOG_MS) {
    this->log_summary_(now_ms);
    this->last_summary_ms_ = now_ms;
  }
  this->tx_service_(micros());
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
  this->on_uart_byte_(byte, now_us);
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
  if (!enabled) {
    this->service_edge_window_(true);
    this->log_summary_(millis());
  }
  this->capture_enabled_ = enabled;
  this->repeat_count_ = 0;
  this->last_len_ = 0;
  if (enabled) {
#ifdef USE_ESP32
    if (this->edge_isr_ok_)
      gpio_intr_enable(static_cast<gpio_num_t>(this->edge_pin_));
#endif
    this->last_summary_ms_ = millis();
    this->status_count_ = 0;
    this->poll_count_ = 0;
    this->poll_4f_count_ = 0;
    this->other_count_ = 0;
    this->edge_quiet_ = 0;
    this->edge_hit_ = 0;
    this->have_status_ = false;
    this->parity_seen_ = g_parity_errors;
    this->frame_seen_ = g_frame_errors;
    ESP_LOGW(TAG,
             "Capture ON. Logs may contain keypad PIN digits. Keep logs private and redact them before sharing.");
  } else {
#ifdef USE_ESP32
    if (this->edge_isr_ok_)
      gpio_intr_disable(static_cast<gpio_num_t>(this->edge_pin_));
#endif
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

  const uint32_t t_us = micros();
  // Immediate line, plus a copy kept for the next CAP sum so a dropped
  // logger slot cannot be the only copy.
  if (this->capture_enabled_)
    ESP_LOGI(TAG, "MARK: %s t_us=%lu", buf, static_cast<unsigned long>(t_us));
  else
    ESP_LOGI(TAG, "MARK: %s t_us=%lu (capture is off)", buf, static_cast<unsigned long>(t_us));
  this->remember_mark_(buf, t_us);
}

const char *NemtekComponent::uart_parity_name_() const {
  if (this->parent_ == nullptr)
    return "UNSET";
  switch (this->parent_->get_parity()) {
    case uart::UART_CONFIG_PARITY_ODD:
      return "ODD";
    case uart::UART_CONFIG_PARITY_NONE:
      return "NONE";
    case uart::UART_CONFIG_PARITY_EVEN:
      return "EVEN";
    default:
      return "OTHER";
  }
}

void NemtekComponent::apply_parity_override_() {
  if (!this->parity_override_ || this->parent_ == nullptr)
    return;
  const auto parity = this->parity_odd_ ? uart::UART_CONFIG_PARITY_ODD : uart::UART_CONFIG_PARITY_EVEN;
  this->parent_->set_parity(parity);
#ifdef USE_ESP32
  auto *idf = static_cast<uart::IDFUARTComponent *>(this->parent_);
  const uart_port_t port = static_cast<uart_port_t>(idf->get_hw_serial_number());
  if (port < UART_NUM_MAX && uart_is_driver_installed(port)) {
    uart_set_parity(port, this->parity_odd_ ? UART_PARITY_ODD : UART_PARITY_EVEN);
    ESP_LOGI(TAG, "UART parity set to %s", this->parity_odd_ ? "ODD" : "EVEN");
  }
#endif
}

void NemtekComponent::install_uart_error_hook_() {
#ifdef USE_ESP32
  if (this->parent_ == nullptr)
    return;
  auto *idf = static_cast<uart::IDFUARTComponent *>(this->parent_);
  const uart_port_t port = static_cast<uart_port_t>(idf->get_hw_serial_number());
  if (port >= UART_NUM_MAX || !uart_is_driver_installed(port)) {
    ESP_LOGW(TAG, "UART error counts unavailable");
    return;
  }
  uart_set_select_notif_callback(port, nemtek_uart_notif);
  ESP_LOGI(TAG, "UART error counts on port %u (parity and framing only, bytes kept)",
           static_cast<unsigned>(port));
#else
  ESP_LOGI(TAG, "UART error counts require the ESP32 hardware UART");
#endif
}

void NemtekComponent::install_edge_isr_() {
  if (this->edge_pin_ < 0)
    return;
#ifdef USE_ESP32
  if (this->edge_pin_ >= GPIO_NUM_MAX) {
    ESP_LOGE(TAG, "Edge pin %d is not a GPIO on this chip", this->edge_pin_);
    this->edge_pin_ = -1;
    return;
  }
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  // Input interrupt only. Do not call gpio_config, gpio_reset_pin, or
  // gpio_set_direction: those retake the pad and would disconnect UART RX.
  const esp_err_t service = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Edge ISR service failed: %s", esp_err_to_name(service));
    return;
  }
  gpio_input_enable(pin);
  gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
  const esp_err_t added = gpio_isr_handler_add(pin, nemtek_edge_isr, nullptr);
  if (added != ESP_OK) {
    ESP_LOGE(TAG, "Edge ISR attach on GPIO%d failed: %s", this->edge_pin_, esp_err_to_name(added));
    return;
  }
  g_edge.pin = static_cast<uint8_t>(this->edge_pin_);
  g_edge.open = 0;
  g_edge.count = 0;
  gpio_intr_disable(pin);
  this->edge_isr_ok_ = true;
  ESP_LOGI(TAG, "Edge capture on GPIO%d, input interrupt only, window %lu us", this->edge_pin_,
           static_cast<unsigned long>(this->edge_window_us_));
#else
  ESP_LOGW(TAG, "Edge capture requires ESP32");
#endif
}

void NemtekComponent::on_uart_byte_(uint8_t byte, uint32_t now_us) {
  if (poll_index_(byte) >= 0)
    this->tx_on_poll_(byte, now_us);
  if (!this->capture_enabled_ || this->tx_runtime_ || poll_index_(byte) < 0)
    return;
  this->poll_count_++;
  if (byte == 0x4F)
    this->poll_4f_count_++;
  if (!this->edge_isr_ok_)
    return;
  if (this->edge_after_4f_only_ && byte != 0x4F)
    return;
  this->service_edge_window_(true);
#ifdef USE_ESP32
  g_edge.count = 0;
  g_edge.end_us = now_us + this->edge_window_us_;
  g_edge.pin = static_cast<uint8_t>(this->edge_pin_);
  g_edge.open = 1;
#endif
  this->window_start_us_ = now_us;
  this->window_poll_ = byte;
  this->window_armed_ = true;
}

void NemtekComponent::service_edge_window_(bool force) {
  if (!this->window_armed_)
    return;
  if (!force && static_cast<int32_t>(micros() - (this->window_start_us_ + this->edge_window_us_)) < 0)
    return;
  this->window_armed_ = false;
#ifdef USE_ESP32
  g_edge.open = 0;
  const uint16_t n = g_edge.count;
#else
  const uint16_t n = 0;
#endif
  if (n == 0) {
    this->edge_quiet_++;
    return;
  }
  this->edge_hit_++;
  this->log_edge_window_(this->window_poll_, this->window_start_us_, n);
}

void NemtekComponent::log_edge_window_(uint8_t poll, uint32_t start_us, uint16_t count) {
#ifdef USE_ESP32
  EdgeSample sample[kEdgeMax];
  uint16_t n = count;
  if (n > kEdgeMax)
    n = kEdgeMax;
  for (uint16_t i = 0; i < n; i++) {
    sample[i].t_us = g_edge.t_us[i];
    sample[i].level = g_edge.level[i];
  }

  const char *mark = this->pending_mark_count_ > 0 ? this->pending_mark_[this->pending_mark_count_ - 1] : "";
  ESP_LOGI(TAG, "EDGE poll=%02X t_us=%lu n=%u%s%s", poll, static_cast<unsigned long>(start_us),
           static_cast<unsigned>(n), mark[0] != '\0' ? " mark=" : "", mark);

  char line[160];
  size_t used = static_cast<size_t>(snprintf(line, sizeof(line), "EDGE dt"));
  for (uint16_t i = 0; i < n; i++) {
    const uint32_t dt = sample[i].t_us - start_us;
    char token[16];
    const int tlen = snprintf(token, sizeof(token), " %lu%c", static_cast<unsigned long>(dt),
                              sample[i].level != 0 ? 'H' : 'L');
    if (tlen < 0)
      continue;
    if (used + static_cast<size_t>(tlen) >= sizeof(line)) {
      ESP_LOGI(TAG, "%s", line);
      used = static_cast<size_t>(snprintf(line, sizeof(line), "EDGE dt"));
    }
    std::memcpy(line + used, token, static_cast<size_t>(tlen));
    used += static_cast<size_t>(tlen);
    line[used] = '\0';
  }
  ESP_LOGI(TAG, "%s", line);

  auto level_at = [&](uint32_t t, uint8_t *level) -> bool {
    bool any = false;
    uint8_t cur = 0;
    for (uint16_t i = 0; i < n; i++) {
      if (sample[i].t_us <= t) {
        cur = sample[i].level;
        any = true;
      } else {
        break;
      }
    }
    if (!any)
      return false;
    *level = cur;
    return true;
  };

  // Bit center i (0 = start bit) at 2400 baud.
  auto sample_bit = [&](uint32_t origin, int bit, bool *uart_one) -> bool {
    const uint32_t at = origin + static_cast<uint32_t>((2 * bit + 1) * 1000000ULL / 4800ULL);
    uint8_t level = 0;
    if (!level_at(at, &level))
      return false;
    const bool gpio_high = level != 0;
    *uart_one = this->edge_inverted_ ? !gpio_high : gpio_high;
    return true;
  };

  char decoded[160];
  size_t dused = static_cast<size_t>(snprintf(decoded, sizeof(decoded), "EDGE try"));
  uint16_t origin_index = 0;
  int bytes = 0;
  while (origin_index < n && bytes < 4 && dused + 40 < sizeof(decoded)) {
    const uint32_t origin = sample[origin_index].t_us;
    bool start = false;
    if (!sample_bit(origin, 0, &start) || start) {
      origin_index++;
      continue;
    }
    uint16_t data9 = 0;
    bool complete8 = true;
    bool complete9 = true;
    for (int bit = 0; bit < 9; bit++) {
      bool one = false;
      if (!sample_bit(origin, bit + 1, &one)) {
        if (bit < 8)
          complete8 = false;
        complete9 = false;
        break;
      }
      if (one)
        data9 = static_cast<uint16_t>(data9 | (1u << bit));
    }
    bool parity_bit = false;
    bool stop_bit = false;
    const bool have_parity = sample_bit(origin, 9, &parity_bit);
    const bool have_stop = sample_bit(origin, 10, &stop_bit);
    const uint8_t data8 = static_cast<uint8_t>(data9 & 0xFF);
    uint8_t ones = 0;
    for (int bit = 0; bit < 8; bit++)
      ones = static_cast<uint8_t>(ones ^ ((data8 >> bit) & 1));
    char piece[48];
    if (!complete8) {
      snprintf(piece, sizeof(piece), " ?");
    } else {
      const bool even_ok = have_parity && have_stop && stop_bit && (parity_bit == (ones != 0));
      const bool odd_ok = have_parity && have_stop && stop_bit && (parity_bit == (ones == 0));
      const bool n_ok = have_parity && parity_bit;  // 8N1: bit 9 is the stop bit
      snprintf(piece, sizeof(piece), " E1=%02X%s O1=%02X%s N1=%02X%s 9=%03X%s", data8, even_ok ? "/ok" : "/bad",
               data8, odd_ok ? "/ok" : "/bad", data8, n_ok ? "/ok" : "/bad", data9, complete9 ? "" : "?");
    }
    const size_t plen = std::strlen(piece);
    if (dused + plen < sizeof(decoded)) {
      std::memcpy(decoded + dused, piece, plen);
      dused += plen;
      decoded[dused] = '\0';
    }
    bytes++;
    const uint32_t next = origin + static_cast<uint32_t>(11 * 1000000ULL / 2400ULL);
    uint16_t next_index = static_cast<uint16_t>(origin_index + 1);
    while (next_index < n && sample[next_index].t_us < next)
      next_index++;
    if (next_index == origin_index)
      break;
    origin_index = next_index;
  }
  if (bytes == 0)
    snprintf(decoded, sizeof(decoded), "EDGE try none");
  ESP_LOGI(TAG, "%s", decoded);

  // Keypad replies are 8E1 with the opposite polarity: pad high is idle/1,
  // and start/0 pull the line low. Panel decode above uses the UART inversion.
  char kp[96];
  size_t kused = static_cast<size_t>(snprintf(kp, sizeof(kp), "EDGE kp"));
  int kp_bytes = 0;
  uint16_t kp_i = 0;
  while (kp_i < n && kp_bytes < 4 && kused + 8 < sizeof(kp)) {
    const uint32_t origin = sample[kp_i].t_us;
    auto bit_is_one = [&](int bit, bool *one) -> bool {
      const uint32_t at = origin + static_cast<uint32_t>((2 * bit + 1) * 1000000ULL / 4800ULL);
      uint8_t level = 0;
      if (!level_at(at, &level))
        return false;
      *one = level != 0;
      return true;
    };
    bool start_one = false;
    if (!bit_is_one(0, &start_one) || start_one) {
      kp_i++;
      continue;
    }
    uint8_t data = 0;
    bool ok = true;
    for (int bit = 0; bit < 8; bit++) {
      bool one = false;
      if (!bit_is_one(bit + 1, &one)) {
        ok = false;
        break;
      }
      if (one)
        data = static_cast<uint8_t>(data | (1u << bit));
    }
    bool parity = false;
    bool stop = false;
    uint8_t x = 0;
    for (int bit = 0; bit < 8; bit++)
      x = static_cast<uint8_t>(x ^ ((data >> bit) & 1));
    const bool parity_ok = ok && bit_is_one(9, &parity) && bit_is_one(10, &stop) && stop && (parity == (x != 0));
    if (!ok) {
      kp_i++;
      continue;
    }
    const int wrote = snprintf(kp + kused, sizeof(kp) - kused, " %02X%s", data, parity_ok ? "" : "?");
    if (wrote > 0)
      kused += static_cast<size_t>(wrote);
    kp_bytes++;
    const uint32_t next = origin + static_cast<uint32_t>(11 * 1000000ULL / 2400ULL);
    uint16_t nxt = static_cast<uint16_t>(kp_i + 1);
    while (nxt < n && sample[nxt].t_us < next)
      nxt++;
    if (nxt == kp_i)
      break;
    kp_i = nxt;
  }
  if (kp_bytes == 0)
    snprintf(kp, sizeof(kp), "EDGE kp none");
  ESP_LOGI(TAG, "%s", kp);
#else
  (void) poll;
  (void) start_us;
  (void) count;
#endif
}

void NemtekComponent::remember_mark_(const char *label, uint32_t t_us) {
  if (this->pending_mark_count_ >= kMarkSlots) {
    this->pending_mark_overflow_ = true;
    return;
  }
  const uint8_t slot = this->pending_mark_count_++;
  std::snprintf(this->pending_mark_[slot], kMarkLen + 1, "%s", label);
  this->pending_mark_us_[slot] = t_us;
}

void NemtekComponent::log_summary_(uint32_t now_ms) {
  (void) now_ms;
  const uint32_t parity_now = g_parity_errors;
  const uint32_t frame_now = g_frame_errors;
  const uint32_t parity_delta = parity_now - this->parity_seen_;
  const uint32_t frame_delta = frame_now - this->frame_seen_;
  this->parity_seen_ = parity_now;
  this->frame_seen_ = frame_now;

  ESP_LOGI(TAG,
           "CAP sum st=%lu poll=%lu 4F=%lu other=%lu edge_q=%lu edge_hit=%lu uart=%s P=%lu F=%lu marks=%u%s",
           static_cast<unsigned long>(this->status_count_), static_cast<unsigned long>(this->poll_count_),
           static_cast<unsigned long>(this->poll_4f_count_), static_cast<unsigned long>(this->other_count_),
           static_cast<unsigned long>(this->edge_quiet_), static_cast<unsigned long>(this->edge_hit_),
           this->uart_parity_name_(), static_cast<unsigned long>(parity_delta),
           static_cast<unsigned long>(frame_delta), static_cast<unsigned>(this->pending_mark_count_),
           this->pending_mark_overflow_ ? " overflow" : "");
  for (uint8_t i = 0; i < this->pending_mark_count_; i++) {
    ESP_LOGI(TAG, "CAP sum mark: %s t_us=%lu", this->pending_mark_[i],
             static_cast<unsigned long>(this->pending_mark_us_[i]));
  }
  this->pending_mark_count_ = 0;
  this->pending_mark_overflow_ = false;
  this->status_count_ = 0;
  this->poll_count_ = 0;
  this->poll_4f_count_ = 0;
  this->other_count_ = 0;
  this->edge_quiet_ = 0;
  this->edge_hit_ = 0;
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

  // Single poll bytes and unchanged status frames are counted, not printed.
  // A full timing dump of a change is DEBUG so the INFO stream stays small.
  if (this->burst_len_ == 1 && poll_index_(this->burst_[0]) >= 0)
    return;

  const bool status = this->burst_len_ >= 3 && this->burst_[0] == 0x80 && this->burst_[1] == 0xFB;
  size_t payload = this->burst_len_;
  if (status && payload > 3 && poll_index_(this->burst_[payload - 1]) >= 0)
    payload--;

  if (status) {
    this->status_count_++;
    const bool same = this->have_status_ && payload == this->last_status_len_ &&
                      std::memcmp(this->burst_.data(), this->last_status_, payload) == 0;
    if (payload <= sizeof(this->last_status_)) {
      std::memcpy(this->last_status_, this->burst_.data(), payload);
      this->last_status_len_ = static_cast<uint8_t>(payload);
      this->have_status_ = true;
    }
    if (same)
      return;

    MerlinState state = MerlinState::UNKNOWN;
    bool eb_present = false;
    const bool mapped = this->classify_(&state, &eb_present);
    char hex[80];
    size_t used = 0;
    for (size_t i = 0; i < payload && used + 3 < sizeof(hex); i++) {
      if (i > 0)
        hex[used++] = ' ';
      used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used, "%02X", this->burst_[i]));
    }
    hex[used] = '\0';
    ESP_LOGI(TAG, "CAP chg map=%s n=%u %s", mapped ? this->map_token_(state) : "none",
             static_cast<unsigned>(payload), hex);
    this->log_capture_full_(mapped ? this->map_token_(state) : "none");
    return;
  }

  this->other_count_++;
  char hex[80];
  size_t used = 0;
  const size_t shown = this->burst_len_ > 16 ? 16 : this->burst_len_;
  for (size_t i = 0; i < shown && used + 3 < sizeof(hex); i++) {
    if (i > 0)
      hex[used++] = ' ';
    used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used, "%02X", this->burst_[i]));
  }
  hex[used] = '\0';
  ESP_LOGI(TAG, "CAP other n=%u %s%s", static_cast<unsigned>(this->burst_len_), hex,
           this->burst_len_ > shown ? " ..." : "");
  this->log_capture_full_("other");
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
  ESP_LOGD(TAG, "%s", head);

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
      ESP_LOGD(TAG, "%s", line);
      used = static_cast<size_t>(snprintf(line, sizeof(line), "CAP |"));
      if (used >= sizeof(line))
        used = sizeof(line) - 1;
    }
    line[used++] = ' ';
    std::memcpy(line + used, token, tlen);
    used += tlen;
    line[used] = '\0';
  }
  ESP_LOGD(TAG, "%s", line);
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
  ESP_LOGCONFIG(TAG, "  Protocol: legacy keypad bus, inverted UART, 2400 baud, 8 data, parity %s",
                this->uart_parity_name_());
  ESP_LOGCONFIG(TAG, "  State debounce: %u matching bursts", REQUIRED_MATCHES);
  ESP_LOGCONFIG(TAG, "  Comms timeout: %lu ms, then fence sensors unavailable",
                static_cast<unsigned long>(COMMS_TIMEOUT_MS));
  ESP_LOGCONFIG(TAG, "  Capture: %s, coalesce: %s", this->capture_switch_ != nullptr ? "switch" : "off",
                this->capture_coalesce_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  UART thresholds: left at the ESPHome UART defaults");
  if (this->edge_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Edge capture: GPIO%d input only, window %lu us, after %s", this->edge_pin_,
                  static_cast<unsigned long>(this->edge_window_us_), this->edge_after_4f_only_ ? "4F" : "all polls");
  } else {
    ESP_LOGCONFIG(TAG, "  Edge capture: off");
  }
  if (this->mark_text_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Mark text: '%s'", this->mark_text_->get_name().c_str());
  }
  LOG_SWITCH("  ", "Capture", this->capture_switch_);
  LOG_BINARY_SENSOR("  ", "Communications", this->communications_sensor_);
}

}  // namespace nemtek
}  // namespace esphome
