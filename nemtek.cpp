#include "nemtek.h"

#include <cstdio>

#include "esphome/core/log.h"

namespace esphome {
namespace nemtek {

static const char *const TAG = "nemtek";
static constexpr uint32_t BURST_GAP_MS = 25;
static constexpr uint32_t COMMS_TIMEOUT_MS = 5000;
static constexpr uint8_t REQUIRED_MATCHES = 3;

void NemtekComponent::setup() {
  ESP_LOGI(TAG, "Starting receive-only Nemtek Merlin 4 monitor");
  this->burst_len_ = 0;
  this->set_comms_(false);

  while (this->available()) {
    uint8_t discard;
    this->read_byte(&discard);
  }
}

void NemtekComponent::loop() {
  uint32_t now = millis();

  if (this->burst_len_ > 0 && static_cast<uint32_t>(now - this->last_rx_ms_) > BURST_GAP_MS) {
    this->decode_burst_();
    this->burst_len_ = 0;
  }

  while (this->available()) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;

    now = millis();
    if (this->burst_len_ > 0 && static_cast<uint32_t>(now - this->last_rx_ms_) > BURST_GAP_MS) {
      this->decode_burst_();
      this->burst_len_ = 0;
    }

    if (this->burst_len_ < this->burst_.size()) {
      this->burst_[this->burst_len_++] = byte;
    } else {
      ESP_LOGW(TAG, "Burst buffer full; dropping capture");
      this->burst_len_ = 0;
    }
    this->last_rx_ms_ = now;
  }

  if (this->comms_state_ && static_cast<uint32_t>(now - this->last_valid_ms_) > COMMS_TIMEOUT_MS) {
    ESP_LOGW(TAG, "No valid Merlin 4 traffic for %lu ms",
             static_cast<unsigned long>(COMMS_TIMEOUT_MS));
    this->set_comms_(false);
    this->current_state_ = MerlinState::UNKNOWN;
    this->candidate_state_ = MerlinState::UNKNOWN;
    this->candidate_count_ = 0;
  }
}

void NemtekComponent::decode_burst_() {
  if (this->burst_len_ < 5 || this->burst_[0] != 0x80 || this->burst_[1] != 0xFB)
    return;

  bool df_present = false;
  bool eb_present = false;
  for (size_t i = 2; i < this->burst_len_; i++) {
    df_present |= this->burst_[i] == 0xDF;
    eb_present |= this->burst_[i] == 0xEB;
  }

  const uint8_t s0 = this->burst_[2];
  const uint8_t s1 = this->burst_len_ > 3 ? this->burst_[3] : 0x00;
  MerlinState state = MerlinState::UNKNOWN;

  // Empirical Merlin 4 mappings established from controlled captures:
  //   FC ... DF  = energised, normal/high-power
  //   FC ...     = energised, low-power mode
  //   FF ... DF  = energised, fence voltage low
  //   F4 FE ...  = fence alarm active/latched
  //   FF ...     = fence off
  if (s0 == 0xF4 && s1 == 0xFE) {
    state = MerlinState::ALARM;
  } else if (s0 == 0xFC && df_present) {
    state = MerlinState::NORMAL;
  } else if (s0 == 0xFC && !df_present) {
    state = MerlinState::LOW_POWER;
  } else if (s0 == 0xFF && df_present) {
    state = MerlinState::VOLTAGE_LOW;
  } else if (s0 == 0xFF && !df_present) {
    state = MerlinState::OFF;
  } else {
    return;
  }

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

const char *NemtekComponent::state_name_(MerlinState state) const {
  switch (state) {
    case MerlinState::NORMAL: return "normal/high-power";
    case MerlinState::LOW_POWER: return "low-power";
    case MerlinState::VOLTAGE_LOW: return "fence voltage low";
    case MerlinState::ALARM: return "fence alarm";
    case MerlinState::OFF: return "off";
    default: return "unknown";
  }
}

void NemtekComponent::publish_(binary_sensor::BinarySensor *sensor, bool state) {
  if (sensor != nullptr && (!sensor->has_state() || sensor->state != state))
    sensor->publish_state(state);
}

void NemtekComponent::set_comms_(bool state) {
  this->comms_state_ = state;
  this->publish_(this->communications_sensor_, state);
}

void NemtekComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Nemtek Merlin 4 energiser monitor:");
  ESP_LOGCONFIG(TAG, "  Mode: receive only");
  ESP_LOGCONFIG(TAG, "  Protocol: legacy keypad bus, inverted UART, 2400 baud, 8E1");
  ESP_LOGCONFIG(TAG, "  State debounce: %u matching bursts", REQUIRED_MATCHES);
  LOG_BINARY_SENSOR("  ", "Communications", this->communications_sensor_);
}

}  // namespace nemtek
}  // namespace esphome
