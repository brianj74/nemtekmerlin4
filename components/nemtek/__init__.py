import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, number, select, switch, text, uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "button", "number", "select", "switch", "text"]

nemtek_ns = cg.esphome_ns.namespace("nemtek")
NemtekComponent = nemtek_ns.class_("NemtekComponent", cg.Component, uart.UARTDevice)
NemtekCaptureSwitch = nemtek_ns.class_("NemtekCaptureSwitch", switch.Switch)
NemtekMarkText = nemtek_ns.class_("NemtekMarkText", text.Text)
NemtekTxEnableSwitch = nemtek_ns.class_("NemtekTxEnableSwitch", switch.Switch)
NemtekIdleSwitch = nemtek_ns.class_("NemtekIdleSwitch", switch.Switch)
NemtekFenceSwitch = nemtek_ns.class_("NemtekFenceSwitch", switch.Switch)
NemtekSlotSelect = nemtek_ns.class_("NemtekSlotSelect", select.Select)
NemtekDigitNumber = nemtek_ns.class_("NemtekDigitNumber", number.Number)
NemtekIdleButton = nemtek_ns.class_("NemtekIdleButton", button.Button)
NemtekDigitButton = nemtek_ns.class_("NemtekDigitButton", button.Button)
NemtekPowerButton = nemtek_ns.class_("NemtekPowerButton", button.Button)

SLOT_OPTIONS = ["79", "F8", "27", "F7", "FB", "7A"]

CONF_CAPTURE = "capture"
CONF_MARK = "mark"
CONF_CAPTURE_COALESCE = "capture_coalesce"

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

schema[cv.Optional(CONF_CAPTURE)] = switch.switch_schema(
    NemtekCaptureSwitch,
    icon="mdi:record-circle-outline",
    entity_category="config",
    default_restore_mode="ALWAYS_OFF",
)
schema[cv.Optional(CONF_MARK)] = text.text_schema(
    NemtekMarkText,
    icon="mdi:marker-check",
    entity_category="config",
    mode="TEXT",
)
schema[cv.Optional(CONF_CAPTURE_COALESCE, default=True)] = cv.boolean
schema[cv.Optional("parity")] = cv.one_of("EVEN", "ODD", upper=True)
schema[cv.Optional("edge_pin")] = cv.int_range(min=0, max=39)
schema[cv.Optional("edge_window", default="30ms")] = cv.positive_time_period_microseconds
schema[cv.Optional("edge_after", default="ALL")] = cv.one_of("ALL", "4F", upper=True)
schema[cv.Optional("edge_inverted", default=True)] = cv.boolean
schema[cv.Optional("tx_enabled", default=False)] = cv.boolean
schema[cv.Optional("fence_control", default=False)] = cv.boolean
schema[cv.Optional("tx_slot", default="79")] = cv.one_of(*SLOT_OPTIONS, upper=True)
schema[cv.Optional("test_digit", default=0)] = cv.int_range(min=0, max=9)
schema[cv.Optional("fence_pin")] = cv.string_strict
schema[cv.Optional("tx_enable")] = switch.switch_schema(
    NemtekTxEnableSwitch,
    icon="mdi:transmit",
    entity_category="config",
    default_restore_mode="ALWAYS_OFF",
)
schema[cv.Optional("idle_responder")] = switch.switch_schema(
    NemtekIdleSwitch,
    icon="mdi:account-clock",
    entity_category="config",
    default_restore_mode="ALWAYS_OFF",
)
schema[cv.Optional("fence_switch")] = switch.switch_schema(
    NemtekFenceSwitch,
    icon="mdi:electric-switch",
    entity_category="config",
    default_restore_mode="ALWAYS_OFF",
)
schema[cv.Optional("slot")] = select.select_schema(
    NemtekSlotSelect,
    icon="mdi:numeric",
    entity_category="config",
)
schema[cv.Optional("test_digit_number")] = number.number_schema(
    NemtekDigitNumber,
    icon="mdi:numeric-0-circle",
    entity_category="config",
)
schema[cv.Optional("send_idle")] = button.button_schema(
    NemtekIdleButton,
    icon="mdi:message-outline",
    entity_category="config",
)
schema[cv.Optional("send_digit")] = button.button_schema(
    NemtekDigitButton,
    icon="mdi:numeric",
    entity_category="config",
)
schema[cv.Optional("toggle_power")] = button.button_schema(
    NemtekPowerButton,
    icon="mdi:flash",
    entity_category="config",
)
schema[cv.Optional("binary_sensor")] = cv.invalid(
    "Put each sensor directly under 'nemtek:'. "
    'Example: nemtek -> communications -> name: "Nemtek Communications"'
)

def _validate_fence_pin(config):
    if config.get("fence_control") and "fence_pin" not in config:
        raise cv.Invalid("fence_control needs fence_pin: !secret fence_pin")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(schema).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA),
    _validate_fence_pin,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_capture_coalesce(config[CONF_CAPTURE_COALESCE]))
    cg.add(var.set_tx_compiled(config["tx_enabled"]))
    cg.add(var.set_fence_control_compiled(config["fence_control"]))
    slot = int(config["tx_slot"], 16)
    cg.add(var.set_default_slot(slot))
    cg.add(var.set_default_test_digit(config["test_digit"]))
    if "fence_pin" in config:
        cg.add(var.set_fence_pin(config["fence_pin"]))
    if "parity" in config:
        cg.add(var.set_parity_odd(config["parity"] == "ODD"))
    if "edge_pin" in config:
        cg.add(var.set_edge_pin(config["edge_pin"]))
        cg.add(var.set_edge_window_us(config["edge_window"].total_microseconds))
        cg.add(var.set_edge_after_4f_only(config["edge_after"] == "4F"))
        cg.add(var.set_edge_inverted(config["edge_inverted"]))

    for key, (setter, _) in SENSORS.items():
        if key in config:
            sensor = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(sensor))

    if CONF_CAPTURE in config:
        capture = await switch.new_switch(config[CONF_CAPTURE])
        cg.add(capture.set_parent(var))
        cg.add(var.set_capture_switch(capture))

    if CONF_MARK in config:
        mark = await text.new_text(
            config[CONF_MARK], min_length=0, max_length=48
        )
        cg.add(mark.set_parent(var))
        cg.add(var.set_mark_text(mark))

    if "tx_enable" in config:
        sw = await switch.new_switch(config["tx_enable"])
        cg.add(sw.set_parent(var))
        cg.add(var.set_tx_enable_switch(sw))
    if "idle_responder" in config:
        sw = await switch.new_switch(config["idle_responder"])
        cg.add(sw.set_parent(var))
        cg.add(var.set_idle_switch(sw))
    if "fence_switch" in config:
        sw = await switch.new_switch(config["fence_switch"])
        cg.add(sw.set_parent(var))
        cg.add(var.set_fence_switch(sw))
    if "slot" in config:
        sel = await select.new_select(config["slot"], options=SLOT_OPTIONS)
        cg.add(sel.set_parent(var))
        cg.add(var.set_slot_select(sel))
    if "test_digit_number" in config:
        num = await number.new_number(
            config["test_digit_number"], min_value=0, max_value=9, step=1
        )
        cg.add(num.set_parent(var))
        cg.add(var.set_digit_number(num))
    if "send_idle" in config:
        btn = await button.new_button(config["send_idle"])
        cg.add(btn.set_parent(var))
    if "send_digit" in config:
        btn = await button.new_button(config["send_digit"])
        cg.add(btn.set_parent(var))
    if "toggle_power" in config:
        btn = await button.new_button(config["toggle_power"])
        cg.add(btn.set_parent(var))
