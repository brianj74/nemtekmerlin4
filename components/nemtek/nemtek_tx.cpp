#include "nemtek.h"

#include <cstring>

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#endif

namespace esphome {
namespace nemtek {

static const char *const TAG_TX = "nemtek";

static constexpr uint32_t TX_START_US = 2910;
static constexpr uint32_t TX_WINDOW_US = 3300;
static constexpr int32_t TX_HOLD_LIMIT_US = 15000;

#ifdef USE_ESP32
static volatile uint8_t g_tx_abort = 0;
#endif

void NemtekTxEnableSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_tx_enabled(state);
}

void NemtekIdleSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_idle_responder(state);
  this->publish_state(state);
}

void NemtekFenceSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->request_fence(state);
}

void NemtekSlotSelect::control(const std::string &value) {
  if (this->parent_ != nullptr)
    this->parent_->set_tx_slot_name(value.c_str());
  this->publish_state(value);
}

void NemtekDigitNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->set_test_digit(static_cast<uint8_t>(value));
  this->publish_state(value);
}

void NemtekIdleButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->request_idle_once();
}

void NemtekDigitButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->request_test_digit();
}

void NemtekPowerButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->request_power_toggle();
}

void NemtekComponent::tx_boot_() {
  this->release_line_();
  const char *slot_name = "79";
  switch (this->tx_slot_) {
    case 0xF8:
      slot_name = "F8";
      break;
    case 0x27:
      slot_name = "27";
      break;
    case 0xF7:
      slot_name = "F7";
      break;
    case 0xFB:
      slot_name = "FB";
      break;
    case 0x7A:
      slot_name = "7A";
      break;
    default:
      slot_name = "79";
      break;
  }
  if (this->slot_select_ != nullptr)
    this->slot_select_->publish_state(slot_name);
  if (this->digit_number_ != nullptr)
    this->digit_number_->publish_state(this->test_digit_);
  if (this->tx_enable_switch_ != nullptr)
    this->tx_enable_switch_->publish_state(false);
  if (this->idle_switch_ != nullptr)
    this->idle_switch_->publish_state(false);
  if (this->fence_switch_ != nullptr)
    this->fence_switch_->publish_state(false);
}

void NemtekComponent::on_shutdown() { this->release_line_(); }

void NemtekComponent::set_fence_pin(const char *pin) {
  this->fence_pin_len_ = 0;
  if (pin == nullptr)
    return;
  for (const char *p = pin; *p != '\0' && this->fence_pin_len_ < 8; p++) {
    if (*p < '0' || *p > '9') {
      this->fence_pin_len_ = 0;
      ESP_LOGE(TAG_TX, "Fence PIN rejected: digits only, not logged");
      return;
    }
    this->fence_pin_[this->fence_pin_len_++] = static_cast<uint8_t>(*p - '0');
  }
  this->fence_pin_[this->fence_pin_len_] = 0;
  if (this->fence_pin_len_ == 0)
    ESP_LOGE(TAG_TX, "Fence PIN empty; fence control will refuse");
}

void NemtekComponent::release_line_() {
  this->tx_armed_ = false;
#ifdef USE_ESP32
  g_tx_abort = 0;
  if (this->edge_pin_ >= 0) {
    const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
  }
#endif
}

void NemtekComponent::set_tx_slot_name(const char *name) {
  uint8_t slot = 0;
  if (name != nullptr && std::strcmp(name, "79") == 0)
    slot = 0x79;
  else if (name != nullptr && std::strcmp(name, "F8") == 0)
    slot = 0xF8;
  else if (name != nullptr && std::strcmp(name, "27") == 0)
    slot = 0x27;
  else if (name != nullptr && std::strcmp(name, "F7") == 0)
    slot = 0xF7;
  else if (name != nullptr && std::strcmp(name, "FB") == 0)
    slot = 0xFB;
  else if (name != nullptr && std::strcmp(name, "7A") == 0)
    slot = 0x7A;
  else {
    ESP_LOGW(TAG_TX, "Slot %s refused. 4F is the real keypad.", name != nullptr ? name : "");
    return;
  }
  this->tx_slot_ = slot;
  ESP_LOGI(TAG_TX, "Emulated slot %02X", slot);
}

void NemtekComponent::set_tx_enabled(bool enabled) {
  if (enabled && !this->tx_compiled_) {
    ESP_LOGW(TAG_TX, "Transmit refused: set tx_enabled: true and reflash");
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    return;
  }
  if (enabled && (this->edge_pin_ < 0 || this->tx_slot_ == 0x4F)) {
    ESP_LOGW(TAG_TX, "Transmit refused: need edge_pin and a slot other than 4F");
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    return;
  }
  if (!enabled) {
    this->tx_runtime_ = false;
    this->tx_fault_ = false;
    this->key_q_len_ = 0;
    this->one_shot_idle_ = false;
    this->need_idle_ack_ = false;
    this->awaiting_confirm_ = false;
    this->release_line_();
    if (this->capture_enabled_ && this->edge_isr_ok_) {
#ifdef USE_ESP32
      gpio_intr_enable(static_cast<gpio_num_t>(this->edge_pin_));
#endif
    }
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    ESP_LOGI(TAG_TX, "Transmit off, GPIO released");
    return;
  }
#ifdef USE_ESP32
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  if (this->edge_isr_ok_)
    gpio_intr_disable(pin);
  this->service_edge_window_(true);
  gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_level(pin, 1);
#endif
  this->tx_fault_ = false;
  this->tx_runtime_ = true;
  if (this->tx_enable_switch_ != nullptr)
    this->tx_enable_switch_->publish_state(true);
  ESP_LOGW(TAG_TX, "Transmit on, slot %02X, open-drain released. Edge capture suspended.", this->tx_slot_);
}

void NemtekComponent::set_idle_responder(bool enabled) {
  if (enabled && (!this->tx_compiled_ || !this->tx_runtime_)) {
    ESP_LOGW(TAG_TX, "Idle responder needs Transmit Enable");
    if (this->idle_switch_ != nullptr)
      this->idle_switch_->publish_state(false);
    return;
  }
  this->idle_responder_ = enabled;
  ESP_LOGI(TAG_TX, "Idle responder %s", enabled ? "on" : "off");
}

bool NemtekComponent::tx_rate_ok_() {
  const uint32_t now = millis();
  uint8_t recent = 0;
  for (uint8_t i = 0; i < this->attempt_n_; i++) {
    if (static_cast<uint32_t>(now - this->attempt_ms_[i]) < 300000)
      recent++;
  }
  return recent < 3;
}

void NemtekComponent::tx_note_attempt_() {
  const uint32_t now = millis();
  if (this->attempt_n_ < 3) {
    this->attempt_ms_[this->attempt_n_++] = now;
    return;
  }
  this->attempt_ms_[0] = this->attempt_ms_[1];
  this->attempt_ms_[1] = this->attempt_ms_[2];
  this->attempt_ms_[2] = now;
}

void NemtekComponent::tx_queue_key_(uint8_t key, bool masked) {
  if (this->key_q_len_ >= sizeof(this->key_q_))
    return;
  this->key_q_[this->key_q_len_] = key;
  this->key_masked_[this->key_q_len_] = masked;
  this->key_q_len_++;
}

static const char *key_words_(uint8_t key, bool masked) {
  if (masked)
    return "pin-digit";
  if (key == 0x0A)
    return "star";
  if (key == 0x0B)
    return "hash";
  return "digit";
}

void NemtekComponent::tx_on_poll_(uint8_t poll, uint32_t now_us) {
  if (this->tx_watch_until_ms_ != 0 && poll == this->tx_slot_ &&
      static_cast<int32_t>(millis() - this->tx_watch_until_ms_) < 0) {
    ESP_LOGI(TAG_TX, "TX ack repoll slot=%02X", poll);
    this->tx_watch_until_ms_ = 0;
  }
  if (!this->tx_runtime_ || this->tx_fault_ || poll == 0x4F || poll != this->tx_slot_)
    return;
  if (this->burst_len_ > 1 && this->burst_[0] == 0x80)
    return;
  const bool want = this->key_q_len_ > 0 || this->need_idle_ack_ || this->one_shot_idle_ || this->idle_responder_;
  if (!want)
    return;
  this->tx_poll_us_ = now_us;
  this->tx_due_us_ = now_us + TX_START_US;
  this->tx_armed_ = true;
}

void NemtekComponent::tx_finish_command_(bool ok, const char *why) {
  this->awaiting_confirm_ = false;
  this->key_q_len_ = 0;
  this->need_idle_ack_ = false;
  ESP_LOGI(TAG_TX, "TX %s: %s", ok ? "confirmed" : "failed", why);
  if (this->fence_switch_ == nullptr)
    return;
  const bool on = this->current_state_ == MerlinState::NORMAL || this->current_state_ == MerlinState::LOW_POWER ||
                  this->current_state_ == MerlinState::VOLTAGE_LOW;
  if (!this->confirm_any_change_)
    this->fence_switch_->publish_state(on);
}

bool NemtekComponent::tx_send_(const uint8_t *bytes, uint8_t len, const char *what) {
#ifndef USE_ESP32
  (void) bytes;
  (void) len;
  (void) what;
  return false;
#else
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  if (g_tx_abort != 0 || gpio_get_level(pin) == 0) {
    gpio_set_level(pin, 1);
    ESP_LOGW(TAG_TX, "TX abort %s: line not released", what);
    this->key_q_len_ = 0;
    this->one_shot_idle_ = false;
    this->need_idle_ack_ = false;
    return false;
  }
  g_tx_abort = 0;
  const uint32_t t0 = micros();
  bool ok = true;
  for (uint8_t i = 0; i < len && ok; i++) {
    const uint8_t data = bytes[i];
    uint8_t parity = 0;
    for (int b = 0; b < 8; b++)
      parity = static_cast<uint8_t>(parity ^ ((data >> b) & 1));
    bool bits[11];
    bits[0] = false;
    for (int b = 0; b < 8; b++)
      bits[b + 1] = ((data >> b) & 1) != 0;
    bits[9] = parity != 0;
    bits[10] = true;
    for (int b = 0; b < 11; b++) {
      const uint32_t index = static_cast<uint32_t>(i * 11 + b);
      const uint32_t mid = t0 + static_cast<uint32_t>((index * 1000000ULL + 500000ULL) / 2400ULL);
      const uint32_t end = t0 + static_cast<uint32_t>(((index + 1) * 1000000ULL) / 2400ULL);
      if (static_cast<int32_t>(micros() - t0) > TX_HOLD_LIMIT_US || g_tx_abort != 0) {
        ok = false;
        break;
      }
      gpio_set_level(pin, bits[b] ? 1 : 0);
      while (static_cast<int32_t>(micros() - mid) < 0) {
      }
      const bool read_one = gpio_get_level(pin) != 0;
      if (read_one != bits[b] || g_tx_abort != 0) {
        ok = false;
        break;
      }
      while (static_cast<int32_t>(micros() - end) < 0) {
      }
    }
  }
  gpio_set_level(pin, 1);
  if (!ok || g_tx_abort != 0) {
    this->release_line_();
    this->tx_runtime_ = false;
    this->tx_fault_ = true;
    this->key_q_len_ = 0;
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    ESP_LOGE(TAG_TX, "TX abort %s: read-back or watchdog, line released. Cycle Transmit Enable to retry.", what);
    return false;
  }
  ESP_LOGI(TAG_TX, "TX sent %s slot=%02X", what, this->tx_slot_);
  this->tx_watch_until_ms_ = millis() + 3000;
  return true;
#endif
}

void NemtekComponent::tx_service_(uint32_t now_us) {
  if (this->awaiting_confirm_ && static_cast<int32_t>(millis() - this->confirm_deadline_ms_) >= 0) {
    const bool on = this->current_state_ == MerlinState::NORMAL || this->current_state_ == MerlinState::LOW_POWER ||
                    this->current_state_ == MerlinState::VOLTAGE_LOW;
    const bool changed = this->current_state_ != this->confirm_from_;
    const bool ok = this->confirm_any_change_ ? changed : (this->confirm_want_on_ ? on : this->current_state_ == MerlinState::OFF);
    if (ok) {
      this->tx_finish_command_(true, this->state_name_(this->current_state_));
    } else if (this->retries_left_ > 0) {
      this->retries_left_ = 0;
      this->awaiting_confirm_ = false;
      this->retrying_ = true;
      ESP_LOGW(TAG_TX, "TX status unchanged, retrying once");
      if (this->confirm_any_change_)
        this->request_power_toggle();
      else
        this->request_fence(this->confirm_want_on_);
    } else {
      this->tx_finish_command_(false, "status unchanged");
    }
  }
  if (!this->tx_armed_)
    return;
  if (static_cast<int32_t>(now_us - this->tx_due_us_) < 0)
    return;
  this->tx_armed_ = false;
  if (static_cast<int32_t>(micros() - this->tx_poll_us_) > static_cast<int32_t>(TX_WINDOW_US)) {
    ESP_LOGW(TAG_TX, "TX skip: outside 2.7-3.2 ms");
    return;
  }
  uint8_t frame[3] = {0xFE, 0x04, 0x03};
  const char *what = "idle";
  bool start_confirm = false;
  if (this->key_q_len_ > 0) {
    const uint8_t key = this->key_q_[0];
    const bool masked = this->key_masked_[0];
    std::memmove(this->key_q_, this->key_q_ + 1, --this->key_q_len_);
    std::memmove(this->key_masked_, this->key_masked_ + 1, this->key_q_len_);
    frame[1] = 0x06;
    frame[2] = key;
    what = key_words_(key, masked);
    if (!masked && key < 10) {
      ESP_LOGI(TAG_TX, "TX sending test digit %u", key);
      what = "test-digit";
    }
    if (this->key_q_len_ == 0)
      this->need_idle_ack_ = true;
  } else if (this->need_idle_ack_ || this->one_shot_idle_ || this->idle_responder_) {
    start_confirm = this->need_idle_ack_ && this->cmd_ != 0;
    this->need_idle_ack_ = false;
    this->one_shot_idle_ = false;
  } else {
    return;
  }
  if (!this->tx_send_(frame, 3, what))
    return;
  if (start_confirm && this->cmd_ != 0) {
    this->awaiting_confirm_ = true;
    this->confirm_deadline_ms_ = millis() + 2000;
    this->confirm_from_ = this->current_state_;
  }
}

void NemtekComponent::request_idle_once() {
  if (!this->tx_runtime_) {
    ESP_LOGW(TAG_TX, "Send idle refused: Transmit Enable is off");
    return;
  }
  this->one_shot_idle_ = true;
  ESP_LOGI(TAG_TX, "TX queued one idle reply on slot %02X", this->tx_slot_);
}

void NemtekComponent::request_test_digit() {
  if (!this->tx_runtime_) {
    ESP_LOGW(TAG_TX, "Send test digit refused: Transmit Enable is off");
    return;
  }
  this->key_q_len_ = 0;
  this->tx_queue_key_(this->test_digit_, false);
  this->cmd_ = 0;
  ESP_LOGI(TAG_TX, "TX queued test digit %u on slot %02X", this->test_digit_, this->tx_slot_);
}

void NemtekComponent::request_fence(bool on) {
  if (!this->fence_control_compiled_ || !this->tx_runtime_) {
    ESP_LOGW(TAG_TX, "Fence command refused: fence_control and Transmit Enable are required");
    if (this->fence_switch_ != nullptr)
      this->fence_switch_->publish_state(this->current_state_ != MerlinState::OFF &&
                                         this->current_state_ != MerlinState::UNKNOWN);
    return;
  }
  if (this->fence_pin_len_ == 0) {
    ESP_LOGW(TAG_TX, "Fence command refused: no digit PIN configured");
    return;
  }
  const bool energised = this->current_state_ == MerlinState::NORMAL ||
                         this->current_state_ == MerlinState::LOW_POWER ||
                         this->current_state_ == MerlinState::VOLTAGE_LOW;
  if ((on && energised) || (!on && this->current_state_ == MerlinState::OFF)) {
    ESP_LOGI(TAG_TX, "Fence already %s, not sending", on ? "on" : "off");
    if (this->fence_switch_ != nullptr)
      this->fence_switch_->publish_state(on);
    return;
  }
  if (!this->retrying_ && (this->key_q_len_ != 0 || this->awaiting_confirm_)) {
    ESP_LOGW(TAG_TX, "Fence command refused: another command is in progress");
    return;
  }
  if (!this->retrying_ && !this->tx_rate_ok_()) {
    ESP_LOGW(TAG_TX, "Fence command refused: 3 attempts in 5 minutes");
    return;
  }
  if (!this->retrying_)
    this->tx_note_attempt_();
  this->retrying_ = false;
  this->key_q_len_ = 0;
  for (uint8_t i = 0; i < this->fence_pin_len_; i++)
    this->tx_queue_key_(this->fence_pin_[i], true);
  this->tx_queue_key_(0x0B, true);
  this->cmd_ = 1;
  this->confirm_any_change_ = false;
  this->confirm_want_on_ = on;
  if (!this->retrying_)
    this->retries_left_ = 1;
  ESP_LOGI(TAG_TX, "TX queued fence %s (%u pin digits + hash, digits not logged)", on ? "on" : "off",
           this->fence_pin_len_);
  if (this->fence_switch_ != nullptr)
    this->fence_switch_->publish_state(on);
}

void NemtekComponent::request_power_toggle() {
  if (!this->fence_control_compiled_ || !this->tx_runtime_) {
    ESP_LOGW(TAG_TX, "Power toggle refused: fence_control and Transmit Enable are required");
    return;
  }
  if (!this->retrying_ && (this->key_q_len_ != 0 || this->awaiting_confirm_)) {
    ESP_LOGW(TAG_TX, "Power toggle refused: another command is in progress");
    return;
  }
  if (!this->retrying_ && !this->tx_rate_ok_()) {
    ESP_LOGW(TAG_TX, "Power toggle refused: 3 attempts in 5 minutes");
    return;
  }
  if (!this->retrying_)
    this->tx_note_attempt_();
  this->retrying_ = false;
  this->key_q_len_ = 0;
  this->tx_queue_key_(0x0A, false);
  this->tx_queue_key_(0x03, false);
  this->tx_queue_key_(0x0B, false);
  this->cmd_ = 2;
  this->confirm_any_change_ = true;
  if (!this->retrying_)
    this->retries_left_ = 1;
  ESP_LOGI(TAG_TX, "TX queued power toggle star 3 hash");
}

}  // namespace nemtek
}  // namespace esphome
