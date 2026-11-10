#include "nemtek.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>
#endif

namespace esphome {
namespace nemtek {

static const char *const TAG_TX = "nemtek";

static constexpr int32_t TX_HOLD_LIMIT_US = 15000;

#ifdef USE_ESP32
static volatile uint8_t g_tx_abort = 0;
static volatile uint8_t s_listen = 0;
static volatile uint8_t s_last_level = 0;
static volatile uint32_t s_start_us = 0;
static esp_timer_handle_t s_byte_timer = nullptr;
static esp_timer_handle_t s_reply_timer = nullptr;
static NemtekComponent *s_owner = nullptr;

static constexpr int kTxRing = 64;
static uint32_t s_et[kTxRing];
static uint8_t s_el[kTxRing];
static volatile uint16_t s_ew = 0;

static volatile uint8_t s_assembling = 0;
static volatile uint8_t s_busy = 0;
static uint8_t s_level_before = 0;
static volatile uint32_t s_last_edge_us = 0;
static volatile uint32_t s_gen = 0;
static volatile uint32_t s_armed_gen = 0;
static volatile uint32_t s_armed_stop = 0;
static volatile uint8_t s_reply_pending = 0;
static TaskHandle_t s_tx_task = nullptr;

void tx_worker(void *);
static void schedule_reply_(uint32_t stop_us, uint32_t delay_us);

void tx_byte_timer_cb(void *) {
  s_assembling = 0;
  if (s_busy != 0 || s_owner == nullptr)
    return;
  s_owner->tx_on_edge_byte_(s_start_us);
}

void tx_reply_timer_cb(void *) {
  if (s_tx_task != nullptr)
    xTaskNotifyGive(s_tx_task);
}

void tx_worker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (s_owner != nullptr)
      s_owner->tx_fire_(s_armed_stop);
  }
}

void nemtek_tx_on_edge(uint32_t now_us, uint8_t level) {
  if (s_listen == 0 || s_busy != 0) {
    s_last_level = level;
    return;
  }
  // A floating pad can chatter far faster than a 2400 baud edge (~417 us).
  // Drop those so the interrupt cannot starve Wi-Fi.
  if (s_last_edge_us != 0 && static_cast<int32_t>(now_us - s_last_edge_us) < 80) {
    s_last_level = level;
    return;
  }
  s_last_edge_us = now_us;
  const uint16_t w = s_ew;
  s_et[w % kTxRing] = now_us;
  s_el[w % kTxRing] = level;
  s_ew = static_cast<uint16_t>(w + 1);
  // A new panel byte starts on a rising edge only while we are not already
  // inside a byte. Data bits that rise mid-byte must not restart the timer.
  if (level != 0 && s_last_level == 0 && s_assembling == 0 && s_byte_timer != nullptr) {
    // A reply armed for an older frame must not fire after this start bit.
    if (s_reply_pending != 0 && static_cast<int32_t>(now_us - s_armed_stop) > 8000) {
      s_gen++;
      s_reply_pending = 0;
      if (s_reply_timer != nullptr)
        esp_timer_stop(s_reply_timer);
    }
    s_assembling = 1;
    s_start_us = now_us;
    esp_timer_stop(s_byte_timer);
    esp_timer_start_once(s_byte_timer, (11ULL * 1000000ULL) / 2400ULL + 80ULL);
  }
  s_last_level = level;
}
#else
void nemtek_tx_on_edge(uint32_t now_us, uint8_t level) {
  (void) now_us;
  (void) level;
}
#endif

bool nemtek_tx_listening() {
#ifdef USE_ESP32
  return s_listen != 0;
#else
  return false;
#endif
}

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

#ifdef USE_ESP32
// Bidirectional BSS138-style shifter (Dilbert66 MOSFET board, same pin for
// read and write): gate is tied to 3.3 V, source is GPIO21, drain is DAT.
// GPIO low pulls DAT low. GPIO high or Hi-Z releases DAT. This is not the
// separate NPN write transistor, which would need a high gate.
static void pin_idle_(gpio_num_t pin) {
  gpio_set_level(pin, 1);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  gpio_pulldown_dis(pin);
  gpio_pullup_en(pin);
}

static void pin_drive_od_(gpio_num_t pin) {
  // Level 1 first so open-drain enable cannot push the bus high.
  gpio_set_level(pin, 1);
  gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_pulldown_dis(pin);
  gpio_pullup_en(pin);
  // Bind the pad output to the GPIO register without gpio_reset_pin, which
  // would drop the UART RX route. Input stays enabled for read-back.
  esp_rom_gpio_connect_out_signal(static_cast<uint32_t>(pin), SIG_GPIO_OUT_IDX, false, false);
  gpio_set_level(pin, 1);
  gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_pullup_en(pin);
}
#endif

void NemtekComponent::release_line_() {
  this->tx_armed_ = false;
#ifdef USE_ESP32
  g_tx_abort = 0;
  s_busy = 0;
  s_reply_pending = 0;
  s_gen++;
  if (s_reply_timer != nullptr)
    esp_timer_stop(s_reply_timer);
  if (s_byte_timer != nullptr)
    esp_timer_stop(s_byte_timer);
  s_assembling = 0;
  if (this->edge_pin_ >= 0)
    pin_idle_(static_cast<gpio_num_t>(this->edge_pin_));
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
#ifdef USE_ESP32
    s_listen = 0;
    if (this->edge_isr_ok_) {
      if (this->capture_enabled_)
        gpio_intr_enable(static_cast<gpio_num_t>(this->edge_pin_));
      else
        gpio_intr_disable(static_cast<gpio_num_t>(this->edge_pin_));
    }
#endif
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    ESP_LOGI(TAG_TX, "Transmit off, GPIO released");
    return;
  }
#ifdef USE_ESP32
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  this->service_edge_window_(true);
  // Stay an input while armed. Output is enabled only for the reply itself,
  // so the UART RX route is not held in a drive mode between polls.
  pin_idle_(pin);
  s_owner = this;
  if (s_byte_timer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = &tx_byte_timer_cb;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "nemtek_byte";
    esp_timer_create(&args, &s_byte_timer);
    args.callback = &tx_reply_timer_cb;
    args.name = "nemtek_reply";
    esp_timer_create(&args, &s_reply_timer);
  }
  if (s_tx_task == nullptr) {
    // Core 1 is the app cpu. The bit timing loop must not run on the Wi-Fi core.
    if (xTaskCreatePinnedToCore(tx_worker, "nemtek_tx", 4096, nullptr, 10, &s_tx_task, 1) != pdPASS)
      ESP_LOGE(TAG_TX, "Transmit task was not created; replies will not be sent");
  }
  s_listen = 1;
  if (this->edge_isr_ok_)
    gpio_intr_enable(pin);
  const int level = gpio_get_level(pin);
  ESP_LOGW(TAG_TX,
           "Transmit on, slot %02X, input+pull-up level=%d. Output stays off until a reply. "
           "GPIO low pulls DAT low (MOSFET shifter). Edge capture suspended.",
           this->tx_slot_, level);
#else
  ESP_LOGW(TAG_TX, "Transmit on, slot %02X. Edge capture suspended.", this->tx_slot_);
#endif
  this->tx_stat_ms_ = millis();
  this->tx_slot_seen_ = 0;
  this->tx_skip_n_ = 0;
  this->tx_fault_ = false;
  this->tx_runtime_ = true;
  if (this->tx_enable_switch_ != nullptr)
    this->tx_enable_switch_->publish_state(true);
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

bool NemtekComponent::tx_want_() const {
  return this->key_q_len_ > 0 || this->need_idle_ack_ || this->one_shot_idle_ || this->idle_responder_;
}

struct TxSkipBucket {
  const char *why;
  uint32_t n;
  uint32_t min_us;
  uint32_t max_us;
  char detail[140];
};

static TxSkipBucket s_skips[4];

static void format_ms_(char *buf, size_t n, uint32_t us) {
  snprintf(buf, n, "%lu.%lu", static_cast<unsigned long>(us / 1000), static_cast<unsigned long>((us % 1000) / 100));
}

void NemtekComponent::tx_note_skip_(uint32_t latency_us, const char *why, const char *detail, bool immediate) {
  if (why == nullptr)
    why = "skip";
  TxSkipBucket *slot = nullptr;
  for (auto &bucket : s_skips) {
    if (bucket.why == why || (bucket.why != nullptr && std::strcmp(bucket.why, why) == 0)) {
      slot = &bucket;
      break;
    }
    if (bucket.n == 0 && slot == nullptr)
      slot = &bucket;
  }
  if (slot == nullptr)
    slot = &s_skips[0];
  if (slot->n == 0 || latency_us < slot->min_us)
    slot->min_us = latency_us;
  if (latency_us > slot->max_us)
    slot->max_us = latency_us;
  slot->n++;
  slot->why = why;
  if (detail != nullptr)
    snprintf(slot->detail, sizeof(slot->detail), "%s", detail);
  if (immediate) {
    char lat[16];
    format_ms_(lat, sizeof(lat), latency_us);
    ESP_LOGW(TAG_TX, "TX skip: latency %sms %s%s%s", lat, why, detail != nullptr ? " " : "",
             detail != nullptr ? detail : "");
  }
}

void NemtekComponent::tx_flush_skips_(uint32_t now_ms) {
  if (static_cast<uint32_t>(now_ms - this->tx_stat_ms_) < 5000)
    return;
  for (auto &bucket : s_skips) {
    if (bucket.n == 0)
      continue;
    char lo[16];
    char hi[16];
    format_ms_(lo, sizeof(lo), bucket.min_us);
    format_ms_(hi, sizeof(hi), bucket.max_us);
    ESP_LOGW(TAG_TX, "TX skip x%lu latency %sms-%sms (%s)%s%s", static_cast<unsigned long>(bucket.n), lo, hi,
             bucket.why != nullptr ? bucket.why : "skip", bucket.detail[0] != '\0' ? " " : "",
             bucket.detail[0] != '\0' ? bucket.detail : "");
    bucket.n = 0;
    bucket.min_us = 0;
    bucket.max_us = 0;
    bucket.detail[0] = '\0';
    bucket.why = nullptr;
  }
  if (this->tx_runtime_ && this->tx_want_() && this->tx_slot_seen_ == 0)
    ESP_LOGW(TAG_TX, "TX wait: slot %02X not seen", this->tx_slot_);
  this->tx_slot_seen_ = 0;
  this->tx_stat_ms_ = now_ms;
}

void NemtekComponent::tx_on_poll_(uint8_t poll, uint32_t now_us) {
  if (this->tx_watch_until_ms_ != 0 && poll == this->tx_slot_ &&
      static_cast<int32_t>(millis() - this->tx_watch_until_ms_) < 0) {
    ESP_LOGI(TAG_TX, "TX ack repoll slot=%02X", poll);
    this->tx_watch_until_ms_ = 0;
  }
  if (!this->tx_runtime_ || this->tx_fault_ || poll == 0x4F || poll != this->tx_slot_)
    return;
  this->tx_slot_seen_++;
  // The UART byte often arrives before the edge timer has scheduled this same
  // poll. Remember it and only log if the edge path does not take the poll.
  this->tx_uart_pending_ = true;
  this->tx_uart_pending_us_ = now_us;
  this->tx_uart_pending_glued_ = this->burst_len_ > 1 && this->burst_[0] == 0x80;
  this->tx_poll_us_ = now_us;
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
  s_busy = 1;
  s_reply_pending = 0;
  if (s_reply_timer != nullptr)
    esp_timer_stop(s_reply_timer);
  if (s_byte_timer != nullptr)
    esp_timer_stop(s_byte_timer);
  s_assembling = 0;
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  // Output turns on only for this frame, and only open-drain.
  pin_drive_od_(pin);
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
  pin_idle_(pin);
  if (!ok || g_tx_abort != 0) {
    this->release_line_();
    this->tx_runtime_ = false;
    this->tx_fault_ = true;
    this->key_q_len_ = 0;
    if (this->tx_enable_switch_ != nullptr)
      this->tx_enable_switch_->publish_state(false);
    const uint32_t latency = micros() - this->tx_stop_us_;
    ESP_LOGE(TAG_TX,
             "TX abort %s: read-back or watchdog, latency=%lu.%lums, line released. Cycle Transmit Enable to retry.",
             what, static_cast<unsigned long>(latency / 1000), static_cast<unsigned long>((latency % 1000) / 100));
    s_busy = 0;
    return false;
  }
  const uint32_t latency = t0 - this->tx_stop_us_;
  ESP_LOGI(TAG_TX, "TX sent %s slot=%02X latency=%lu.%lums readback=ok level-before=%u", what, this->tx_slot_,
           static_cast<unsigned long>(latency / 1000), static_cast<unsigned long>((latency % 1000) / 100),
           static_cast<unsigned>(s_level_before));
  this->tx_watch_until_ms_ = millis() + 3000;
  s_busy = 0;
  return true;
#endif
}

void NemtekComponent::tx_service_(uint32_t now_us) {
  if (this->tx_uart_pending_) {
    const int32_t rel = static_cast<int32_t>(this->tx_edge_sched_us_ - this->tx_uart_pending_us_);
    const bool edge_took_it = this->tx_edge_sched_us_ != 0 && rel > -20000 && rel < 40000;
    if (edge_took_it) {
      this->tx_uart_pending_ = false;
    } else if (static_cast<int32_t>(now_us - this->tx_uart_pending_us_) >= 40000) {
      if (this->tx_want_()) {
        if (this->tx_uart_pending_glued_)
          this->tx_note_skip_(0, "glued to status");
        else if (this->tx_edge_sched_us_ == 0)
          this->tx_note_skip_(0, "slot on uart, no stop edge");
        else
          this->tx_note_skip_(static_cast<uint32_t>(now_us - this->tx_edge_sched_us_), "uart late vs stop edge");
      }
      this->tx_uart_pending_ = false;
    }
  }
  this->tx_flush_skips_(millis());
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
  this->tx_armed_ = false;
  this->tx_fire_(this->tx_stop_us_);
}

void NemtekComponent::tx_on_edge_byte_(uint32_t start_us) {
#ifdef USE_ESP32
  auto level_at = [&](uint32_t t) -> uint8_t {
    uint8_t level = 1;
    uint32_t best = 0;
    bool any = false;
    const uint16_t wr = s_ew;
    for (int i = 0; i < kTxRing; i++) {
      const uint16_t idx = static_cast<uint16_t>((wr + kTxRing - 1 - i) % kTxRing);
      const uint32_t et = s_et[idx];
      if (static_cast<int32_t>(et - start_us) < -200)
        continue;
      if (static_cast<int32_t>(et - t) > 0)
        continue;
      if (!any || static_cast<int32_t>(et - best) >= 0) {
        any = true;
        best = et;
        level = s_el[idx];
      }
    }
    return level;
  };
  uint8_t data = 0;
  uint8_t ones = 0;
  for (int bit = 0; bit < 8; bit++) {
    const uint32_t at = start_us + static_cast<uint32_t>((2 * (bit + 1) + 1) * 1000000ULL / 4800ULL);
    const bool uart_one = level_at(at) == 0;
    if (uart_one) {
      data = static_cast<uint8_t>(data | (1u << bit));
      ones = static_cast<uint8_t>(ones ^ 1);
    }
  }
  const uint32_t parity_at = start_us + static_cast<uint32_t>((2 * 9 + 1) * 1000000ULL / 4800ULL);
  const bool parity_one = level_at(parity_at) == 0;
  const bool odd_ok = parity_one == (ones == 0);
  if (!odd_ok || data == 0x4F || data != this->tx_slot_ || !this->tx_runtime_ || this->tx_fault_)
    return;
  this->tx_slot_seen_++;
  if (!this->tx_want_())
    return;
  const uint32_t stop_us = start_us + (11ULL * 1000000ULL) / 2400ULL;
  this->tx_stop_us_ = stop_us;
  this->tx_edge_sched_us_ = micros();
  const int32_t wait = static_cast<int32_t>((stop_us + this->tx_reply_us_) - micros());
  if (wait > static_cast<int32_t>(this->tx_reply_max_us_)) {
    this->tx_note_skip_(static_cast<uint32_t>(wait), "stop edge already outside window");
    return;
  }
  schedule_reply_(stop_us, wait > 0 ? static_cast<uint32_t>(wait) : 1);
#else
  (void) start_us;
#endif
}

#ifdef USE_ESP32
static void schedule_reply_(uint32_t stop_us, uint32_t delay_us) {
  s_armed_stop = stop_us;
  s_armed_gen = s_gen;
  s_reply_pending = 1;
  if (s_reply_timer == nullptr || s_tx_task == nullptr)
    return;
  esp_timer_stop(s_reply_timer);
  if (esp_timer_start_once(s_reply_timer, delay_us == 0 ? 1 : delay_us) != ESP_OK) {
    esp_timer_stop(s_reply_timer);
    esp_timer_start_once(s_reply_timer, delay_us == 0 ? 1 : delay_us);
  }
}

static void describe_edges_(char *buf, size_t n, uint32_t stop_us, uint32_t now, int level, uint32_t turn_min,
                            uint32_t turn_max, int *since_out, int *outside_out) {
  uint32_t times[6];
  uint8_t levels[6];
  int kept = 0;
  int since = 0;
  int outside = 0;
  const uint16_t wr = s_ew;
  for (int i = 0; i < kTxRing; i++) {
    const uint16_t idx = static_cast<uint16_t>((wr + kTxRing - 1 - i) % kTxRing);
    const uint32_t et = s_et[idx];
    if (static_cast<int32_t>(et - stop_us) <= 0)
      continue;
    if (static_cast<int32_t>(et - now) > 0)
      continue;
    since++;
    const uint32_t dt = et - stop_us;
    if (dt < turn_min || dt > turn_max)
      outside++;
    if (kept < 6) {
      int j = kept;
      while (j > 0 && static_cast<int32_t>(times[j - 1] - et) > 0) {
        times[j] = times[j - 1];
        levels[j] = levels[j - 1];
        j--;
      }
      times[j] = et;
      levels[j] = s_el[idx];
      kept++;
    }
  }
  *since_out = since;
  *outside_out = outside;
  int pos = snprintf(buf, n, "level=%d edges-since-stop=%d", level, since);
  for (int i = 0; i < kept && pos > 0 && static_cast<size_t>(pos) < n; i++) {
    const int32_t dt = static_cast<int32_t>(times[i] - stop_us);
    pos += snprintf(buf + pos, n - static_cast<size_t>(pos), " %+ldus/%u", static_cast<long>(dt),
                    static_cast<unsigned>(levels[i]));
  }
}
#endif

void NemtekComponent::tx_fire_(uint32_t stop_us) {
#ifdef USE_ESP32
  if (s_armed_gen != s_gen)
    return;
#endif
  if (!this->tx_runtime_ || this->tx_fault_ || !this->tx_want_())
    return;
  const uint32_t now = micros();
  const uint32_t latency = now - stop_us;
  if (latency > this->tx_reply_max_us_) {
#ifdef USE_ESP32
    s_reply_pending = 0;
#endif
    this->tx_note_skip_(latency, "stale stop bit");
    return;
  }
  if (latency + 150 < this->tx_reply_min_us_) {
#ifdef USE_ESP32
    schedule_reply_(stop_us, this->tx_reply_min_us_ - latency);
#endif
    return;
  }
#ifdef USE_ESP32
  const gpio_num_t pin = static_cast<gpio_num_t>(this->edge_pin_);
  const int level = this->edge_pin_ >= 0 ? gpio_get_level(pin) : 1;
  char detail[140];
  int since = 0;
  int outside = 0;
  describe_edges_(detail, sizeof(detail), stop_us, now, level, this->tx_turnaround_min_us_,
                  this->tx_turnaround_max_us_, &since, &outside);
  // One edge inside the turnaround window is the panel returning to inverted
  // idle (~2 bit times after the computed stop). The pad then stays low, and
  // that is the level a keypad reply starts from. Any other edge is activity.
  const bool collision = outside > 0 || since > 1;
  if (collision) {
    s_reply_pending = 0;
    this->tx_note_skip_(latency, "activity after stop", detail, true);
    return;
  }
  s_level_before = static_cast<uint8_t>(level);
  s_reply_pending = 0;
#endif
  this->tx_stop_us_ = stop_us;
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
