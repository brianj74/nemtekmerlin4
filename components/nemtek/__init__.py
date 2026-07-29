import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor"]

nemtek_ns = cg.esphome_ns.namespace("nemtek")
NemtekComponent = nemtek_ns.class_(
    "NemtekComponent", cg.Component, uart.UARTDevice
)

SENSORS = {
    "hv_good": ("set_hv_good_sensor", "running"),
    "hv_check": ("set_hv_check_sensor", "problem"),
    "hv_bad": ("set_hv_bad_sensor", "problem"),
    "fence_alarm_volts": ("set_fence_alarm_volts_sensor", "problem"),
    "fence_low_power": ("set_fence_low_power_sensor", "running"),
    "alarm_active": ("set_alarm_active_sensor", "problem"),
    "tamper": ("set_tamper_sensor", "tamper"),
    "gate_alarm": ("set_gate_alarm_sensor", "problem"),
    "battery_history": ("set_battery_history_sensor", "battery"),
    "battery_good": ("set_battery_good_sensor", "battery"),
    "fence_off": ("set_fence_off_sensor", None),
    "fence_volts_low": ("set_fence_volts_low_sensor", "problem"),
    "alarm_bypass": ("set_alarm_bypass_sensor", None),
    "silent_alarm": ("set_silent_alarm_sensor", "running"),
    "gate_immediate": ("set_gate_immediate_sensor", None),
    "gate_bypass": ("set_gate_bypass_sensor", None),
    "battery_empty": ("set_battery_empty_sensor", "battery"),
    "battery_low": ("set_battery_low_sensor", "battery"),
    "buzzer_active": ("set_buzzer_active_sensor", "sound"),
    "mains_present": ("set_mains_present_sensor", "power"),
    "communications": ("set_communications_sensor", "connectivity"),
}

schema = {cv.GenerateID(): cv.declare_id(NemtekComponent)}
for key, (_, device_class) in SENSORS.items():
    options = {}
    if device_class is not None:
        options["device_class"] = device_class
    schema[cv.Optional(key)] = binary_sensor.binary_sensor_schema(**options)

CONFIG_SCHEMA = (
    cv.Schema(schema)
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    for key, (setter, _) in SENSORS.items():
        if key in config:
            sensor = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(sensor))
