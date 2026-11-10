#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#ifdef USE_ESP32
#include <esp_attr.h>
#endif

namespace esphome {
namespace nemtek {

bool nemtek_tx_listening();
#ifdef USE_ESP32
void IRAM_ATTR nemtek_tx_on_edge(uint32_t now_us, uint8_t level);
#else
void nemtek_tx_on_edge(uint32_t now_us, uint8_t level);
#endif

class NemtekComponent;

// Home Assistant switch. Boot default is off (restore_mode ALWAYS_OFF).
class NemtekCaptureSwitch : public switch_::Switch {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;

  NemtekComponent *parent_{nullptr};
};

// Setting this text from Home Assistant inserts one "MARK: <label>" log line.
class NemtekMarkText : public text::Text {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void control(const std::string &value) override;

  NemtekComponent *parent_{nullptr};
};

class NemtekTxEnableSwitch : public switch_::Switch {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;
  NemtekComponent *parent_{nullptr};
};

class NemtekIdleSwitch : public switch_::Switch {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;
  NemtekComponent *parent_{nullptr};
};

class NemtekFenceSwitch : public switch_::Switch {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;
  NemtekComponent *parent_{nullptr};
};

class NemtekSlotSelect : public select::Select {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void control(const std::string &value) override;
  NemtekComponent *parent_{nullptr};
};

class NemtekDigitNumber : public number::Number {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void control(float value) override;
  NemtekComponent *parent_{nullptr};
};

class NemtekIdleButton : public button::Button {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void press_action() override;
  NemtekComponent *parent_{nullptr};
};

class NemtekDigitButton : public button::Button {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void press_action() override;
  NemtekComponent *parent_{nullptr};
};

class NemtekPowerButton : public button::Button {
 public:
  void set_parent(NemtekComponent *parent) { this->parent_ = parent; }

 protected:
  void press_action() override;
  NemtekComponent *parent_{nullptr};
};

class NemtekComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_tx_compiled(bool enabled) { this->tx_compiled_ = enabled; }
  void set_fence_control_compiled(bool enabled) { this->fence_control_compiled_ = enabled; }
  void set_default_slot(uint8_t slot) { this->tx_slot_ = slot; }
  void set_default_test_digit(uint8_t digit) { this->test_digit_ = digit; }
  void set_tx_reply_us(uint32_t us) { this->tx_reply_us_ = us; }
  void set_tx_reply_min_us(uint32_t us) { this->tx_reply_min_us_ = us; }
  void set_tx_reply_max_us(uint32_t us) { this->tx_reply_max_us_ = us; }
  void set_tx_turnaround_min_us(uint32_t us) { this->tx_turnaround_min_us_ = us; }
  void set_tx_turnaround_max_us(uint32_t us) { this->tx_turnaround_max_us_ = us; }
  void set_fence_pin(const char *pin);

  void set_capture_switch(NemtekCaptureSwitch *capture_switch) { this->capture_switch_ = capture_switch; }
  void set_mark_text(NemtekMarkText *mark_text) { this->mark_text_ = mark_text; }
  void set_tx_enable_switch(NemtekTxEnableSwitch *sw) { this->tx_enable_switch_ = sw; }
  void set_idle_switch(NemtekIdleSwitch *sw) { this->idle_switch_ = sw; }
  void set_fence_switch(NemtekFenceSwitch *sw) { this->fence_switch_ = sw; }
  void set_slot_select(NemtekSlotSelect *sel) { this->slot_select_ = sel; }
  void set_digit_number(NemtekDigitNumber *num) { this->digit_number_ = num; }
  void set_capture_coalesce(bool coalesce) { this->capture_coalesce_ = coalesce; }
  void set_parity_odd(bool odd) {
    this->parity_override_ = true;
    this->parity_odd_ = odd;
  }
  // GPIO number of the UART RX pad. The interrupt is input-only and does not
  // retake the pin, so the UART keeps receiving.
  void set_edge_pin(int pin) { this->edge_pin_ = pin; }
  void set_edge_window_us(uint32_t window_us) { this->edge_window_us_ = window_us; }
  void set_edge_after_4f_only(bool only_4f) { this->edge_after_4f_only_ = only_4f; }
  void set_edge_inverted(bool inverted) { this->edge_inverted_ = inverted; }

  // Runtime toggle used by the capture switch. Starts off.
  void set_capture_enabled(bool enabled);
  // Insert a capture annotation. Empty labels are ignored.
  void mark(const std::string &label);

  void set_tx_enabled(bool enabled);
  void set_idle_responder(bool enabled);
  void set_tx_slot_name(const char *name);
  void set_test_digit(uint8_t digit) { this->test_digit_ = digit; }
  void request_idle_once();
  void request_test_digit();
  void request_fence(bool on);
  void request_power_toggle();
  void release_line_();
  void tx_boot_();

#define NEMTEK_SETTER(name) \
  void set_##name##_sensor(binary_sensor::BinarySensor *sensor) { this->name##_sensor_ = sensor; }

  NEMTEK_SETTER(hv_good)
  NEMTEK_SETTER(hv_check)
  NEMTEK_SETTER(hv_bad)
  NEMTEK_SETTER(fence_alarm_volts)
  NEMTEK_SETTER(fence_low_power)
  NEMTEK_SETTER(alarm_active)
  NEMTEK_SETTER(tamper)
  NEMTEK_SETTER(gate_alarm)
  NEMTEK_SETTER(battery_history)
  NEMTEK_SETTER(battery_good)
  NEMTEK_SETTER(fence_off)
  NEMTEK_SETTER(fence_volts_low)
  NEMTEK_SETTER(alarm_bypass)
  NEMTEK_SETTER(silent_alarm)
  NEMTEK_SETTER(gate_immediate)
  NEMTEK_SETTER(gate_bypass)
  NEMTEK_SETTER(battery_empty)
  NEMTEK_SETTER(battery_low)
  NEMTEK_SETTER(buzzer_active)
  NEMTEK_SETTER(mains_present)
  NEMTEK_SETTER(communications)
#undef NEMTEK_SETTER

 protected:
  // Receive-only phase. These hide UARTDevice's write path so this component
  // cannot drive the keypad DAT line. A later transmit phase has to remove them.
  void write_byte(uint8_t data) = delete;
  void write_array(const uint8_t *data, size_t len) = delete;
  void write_array(const std::vector<uint8_t> &data) = delete;
  void write_str(const char *str) = delete;
  size_t write(uint8_t data) = delete;

  enum class MerlinState : uint8_t {
    UNKNOWN = 0,
    NORMAL,
    LOW_POWER,
    VOLTAGE_LOW,
    ALARM,
    OFF,
  };

  void ingest_(uint8_t byte, uint32_t now_us);
  void finish_burst_();
  bool classify_(MerlinState *state, bool *eb_present) const;
  void decode_burst_();
  void consider_state_(MerlinState state, bool eb_present);
  void publish_state_(MerlinState state, bool eb_present);
  void publish_(binary_sensor::BinarySensor *sensor, bool state);
  void invalidate_(binary_sensor::BinarySensor *sensor);
  void lose_comms_();
  void set_comms_(bool state);
  const char *state_name_(MerlinState state) const;
  const char *map_token_(MerlinState state) const;

  void on_uart_byte_(uint8_t byte, uint32_t now_us);
  void tx_on_poll_(uint8_t poll, uint32_t now_us);
  bool tx_lone_poll_(uint8_t poll) const;
  void tx_record_poll_(uint8_t poll);
  void tx_service_(uint32_t now_us);
  void tx_on_edge_byte_(uint32_t start_us);
  void tx_fire_(uint32_t stop_us);
  friend void tx_byte_timer_cb(void *);
  friend void tx_reply_timer_cb(void *);
  friend void tx_worker(void *);
  void tx_note_skip_(uint32_t latency_us, const char *why, const char *detail = nullptr, bool immediate = false);
  void tx_flush_skips_(uint32_t now_ms);
  bool tx_want_() const;
  bool tx_send_(const uint8_t *bytes, uint8_t len, const char *what);
  bool tx_rate_ok_();
  void tx_note_attempt_();
  void tx_queue_key_(uint8_t key, bool masked);
  void tx_finish_command_(bool ok, const char *why);
  void service_edge_window_(bool force);
  void log_edge_window_(uint8_t poll, uint32_t start_us, uint16_t count);
  void log_summary_(uint32_t now_ms);
  void remember_mark_(const char *label, uint32_t t_us);
  void install_edge_isr_();
  void install_uart_error_hook_();
  void apply_parity_override_();
  const char *uart_parity_name_() const;

  void log_capture_();
  void log_capture_full_(const char *map_name);
  void log_repeat_(uint32_t extra_count);
  void flush_pending_repeats_(uint32_t now_ms);
  void save_pattern_();
  void note_repeat_gap_();
  bool same_pattern_() const;
  bool timing_differs_() const;

  static constexpr size_t kBurstMax = 48;

  std::array<uint8_t, kBurstMax> burst_{};
  std::array<uint32_t, kBurstMax> delta_us_{};
  std::array<uint8_t, kBurstMax> flags_{};
  size_t burst_len_{0};
  uint16_t burst_dropped_{0};
  uint32_t burst_parity_{0};
  uint32_t burst_frame_{0};
  uint32_t burst_overflow_{0};
  uint32_t burst_start_us_{0};
  uint32_t burst_gap_us_{0};
  uint32_t last_rx_us_{0};
  bool have_rx_{false};

  std::array<uint8_t, kBurstMax> last_bytes_{};
  std::array<uint32_t, kBurstMax> last_delta_us_{};
  std::array<uint8_t, kBurstMax> last_flags_{};
  size_t last_len_{0};
  uint16_t last_dropped_{0};
  uint32_t last_parity_{0};
  uint32_t last_frame_{0};
  uint32_t last_overflow_{0};
  uint32_t repeat_count_{0};
  uint32_t repeat_gap_min_us_{0};
  uint32_t repeat_gap_max_us_{0};
  uint32_t last_repeat_log_ms_{0};
  uint32_t last_summary_ms_{0};

  static constexpr uint8_t kMarkSlots = 8;
  static constexpr uint8_t kMarkLen = 48;
  char pending_mark_[kMarkSlots][kMarkLen + 1]{};
  uint32_t pending_mark_us_[kMarkSlots]{};
  uint8_t pending_mark_count_{0};
  bool pending_mark_overflow_{false};

  int edge_pin_{-1};
  uint32_t edge_window_us_{30000};
  bool edge_after_4f_only_{false};
  bool edge_inverted_{true};
  bool edge_isr_ok_{false};
  bool parity_override_{false};
  bool parity_odd_{false};
  uint32_t parity_seen_{0};
  uint32_t frame_seen_{0};
  uint32_t status_count_{0};
  uint32_t poll_count_{0};
  uint32_t poll_4f_count_{0};
  uint32_t other_count_{0};
  uint32_t edge_quiet_{0};
  uint32_t edge_hit_{0};
  bool window_armed_{false};
  uint32_t window_start_us_{0};
  uint8_t window_poll_{0};
  uint8_t last_status_[16]{};
  uint8_t last_status_len_{0};
  bool have_status_{false};

  uint32_t last_valid_ms_{0};
  bool comms_state_{false};
  bool capture_enabled_{false};
  bool capture_coalesce_{true};

  MerlinState current_state_{MerlinState::UNKNOWN};
  MerlinState candidate_state_{MerlinState::UNKNOWN};
  uint8_t candidate_count_{0};
  bool current_eb_{false};

  NemtekCaptureSwitch *capture_switch_{nullptr};
  NemtekMarkText *mark_text_{nullptr};
  NemtekTxEnableSwitch *tx_enable_switch_{nullptr};
  NemtekIdleSwitch *idle_switch_{nullptr};
  NemtekFenceSwitch *fence_switch_{nullptr};
  NemtekSlotSelect *slot_select_{nullptr};
  NemtekDigitNumber *digit_number_{nullptr};

  bool tx_compiled_{false};
  bool fence_control_compiled_{false};
  bool tx_runtime_{false};
  bool tx_fault_{false};
  bool idle_responder_{false};
  bool one_shot_idle_{false};
  bool need_idle_ack_{false};
  bool tx_armed_{false};
  bool awaiting_confirm_{false};
  bool confirm_any_change_{false};
  uint8_t tx_slot_{0x79};
  uint8_t test_digit_{0};
  uint8_t key_q_[12]{};
  bool key_masked_[12]{};
  uint8_t key_q_len_{0};
  uint8_t cmd_{0};
  bool retrying_{false};
  uint8_t fence_pin_[9]{};
  uint8_t fence_pin_len_{0};
  uint8_t retries_left_{0};
  MerlinState confirm_from_{MerlinState::UNKNOWN};
  bool confirm_want_on_{false};
  uint32_t confirm_deadline_ms_{0};
  uint32_t tx_due_us_{0};
  uint32_t tx_poll_us_{0};
  uint32_t tx_watch_until_ms_{0};
  uint32_t tx_reply_us_{2900};
  uint32_t tx_reply_min_us_{2500};
  uint32_t tx_reply_max_us_{5000};
  uint32_t tx_turnaround_min_us_{600};
  uint32_t tx_turnaround_max_us_{1100};
  bool tx_uart_pending_{false};
  bool tx_uart_pending_glued_{false};
  uint32_t tx_uart_pending_us_{0};
  const char *reset_reason_{"unknown"};
  int reset_reason_code_{-1};
  uint32_t tx_stop_us_{0};
  const char *tx_skip_why_{nullptr};
  uint32_t tx_skip_n_{0};
  uint32_t tx_skip_min_us_{0};
  uint32_t tx_skip_max_us_{0};
  uint32_t tx_slot_seen_{0};
  uint32_t tx_stat_ms_{0};
  uint32_t tx_edge_sched_us_{0};
  uint32_t tx_uart_gap_us_{0};
  uint32_t tx_glue_n_{0};
  uint32_t tx_glue_min_us_{0};
  uint32_t tx_glue_max_us_{0};
  char tx_post_text_[96]{};
  uint8_t tx_post_left_{0};
  char tx_base_text_[96]{};
  uint8_t tx_base_left_{0};
  uint32_t tx_extra_stop_us_{0};
  uint32_t tx_extra_at_us_{0};
  int32_t tx_extra_dt_{0};
  uint8_t tx_extra_slot_{0};
  bool tx_extra_pending_{false};
  uint32_t attempt_ms_[3]{};
  uint8_t attempt_n_{0};
  char tx_line_pin_{0};

#define NEMTEK_SENSOR(name) binary_sensor::BinarySensor *name##_sensor_{nullptr};
  NEMTEK_SENSOR(hv_good)
  NEMTEK_SENSOR(hv_check)
  NEMTEK_SENSOR(hv_bad)
  NEMTEK_SENSOR(fence_alarm_volts)
  NEMTEK_SENSOR(fence_low_power)
  NEMTEK_SENSOR(alarm_active)
  NEMTEK_SENSOR(tamper)
  NEMTEK_SENSOR(gate_alarm)
  NEMTEK_SENSOR(battery_history)
  NEMTEK_SENSOR(battery_good)
  NEMTEK_SENSOR(fence_off)
  NEMTEK_SENSOR(fence_volts_low)
  NEMTEK_SENSOR(alarm_bypass)
  NEMTEK_SENSOR(silent_alarm)
  NEMTEK_SENSOR(gate_immediate)
  NEMTEK_SENSOR(gate_bypass)
  NEMTEK_SENSOR(battery_empty)
  NEMTEK_SENSOR(battery_low)
  NEMTEK_SENSOR(buzzer_active)
  NEMTEK_SENSOR(mains_present)
  NEMTEK_SENSOR(communications)
#undef NEMTEK_SENSOR
};

}  // namespace nemtek
}  // namespace esphome
