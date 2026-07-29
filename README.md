# Nemtek Merlin 4 ESPHome monitor

Experimental, receive-only ESPHome integration for monitoring a **Nemtek Merlin 4** electric-fence energiser through its keypad data bus.

> [!WARNING]
> This project is based on reverse engineering and has not been endorsed by Nemtek. It is intended for monitoring only. Do not rely on it as the sole alarm or safety system.

## What it currently exposes

- Communications healthy
- Fence off
- Low-power mode
- Fence voltage low
- Fence voltage alarm
- Fence alarm active
- HV good/check/bad convenience states
- Keypad buzzer/warning marker — experimental

Battery, mains, gate and tamper states are not yet positively decoded and are deliberately omitted.

## Hardware

- ESP32 development board
- Bidirectional MOSFET level shifter
- Separate, suitable ESP32 power supply

### Wiring

| Merlin connection | ESP32 connection |
|---|---|
| `DAT` | `GPIO21` through the level shifter |
| `GND` / `-` | ESP32 `GND` |
| `+` | Not connected in the reference setup |

The data signal is read as an **inverted 2400-baud, 8E1 UART**.

See [docs/wiring.md](docs/wiring.md) before connecting anything.

## Installation

### Local ESPHome component

1. Copy `components/nemtek` into the directory containing your ESPHome YAML.
2. Copy `examples/nemtek-merlin.yaml` alongside the `components` directory.
3. Add the required secrets to `secrets.yaml`.
4. Run **Clean Build Files** for the first build.
5. Validate, compile and flash the ESP32.

Expected layout:

```text
/config/esphome/
├── components/
│   └── nemtek/
│       ├── __init__.py
│       ├── nemtek.cpp
│       └── nemtek.h
├── nemtek-merlin.yaml
└── secrets.yaml
```

The Python filename must be exactly `__init__.py`, with no spaces.

## Home Assistant dashboard

A combined template sensor and Mushroom card example is provided in [docs/home-assistant.md](docs/home-assistant.md).

## Protocol status

The current empirical mapping and known uncertainties are documented in [docs/protocol.md](docs/protocol.md).

## Contributing captures

Useful captures include a stable baseline followed by one controlled state change, such as:

- High power to low power
- Healthy fence to loaded/shorted fence
- Warning period to alarm
- Fence on to fence off
- Ordinary keypad beep without an alarm

Please include the energiser model, the action performed and the ESPHome log section before/during/after the change.

## Licence

MIT. See [LICENSE](LICENSE).
