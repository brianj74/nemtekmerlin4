#pragma once

#include <array>
#include <cstdint>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace nemtek {

class NemtekComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

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
  enum class MerlinState : uint8_t {
    UNKNOWN = 0,
    NORMAL,
    LOW_POWER,
    VOLTAGE_LOW,
    ALARM,
    OFF,
  };

  void decode_burst_();
  void consider_state_(MerlinState state, bool eb_present);
  void publish_state_(MerlinState state, bool eb_present);
  void publish_(binary_sensor::BinarySensor *sensor, bool state);
  void set_comms_(bool state);
  const char *state_name_(MerlinState state) const;

  std::array<uint8_t, 32> burst_{};
  size_t burst_len_{0};
  uint32_t last_rx_ms_{0};
  uint32_t last_valid_ms_{0};
  bool comms_state_{false};

  MerlinState current_state_{MerlinState::UNKNOWN};
  MerlinState candidate_state_{MerlinState::UNKNOWN};
  uint8_t candidate_count_{0};
  bool current_eb_{false};

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
