# Nemtek Merlin 4 ESPHome monitor

Experimental, receive-only ESPHome integration for monitoring a **Nemtek Merlin 4** electric-fence energiser through its keypad data bus.

> [!WARNING]
> This project is based on reverse engineering and has not been endorsed by Nemtek. It is intended for monitoring only. Do not rely on it as the sole alarm or safety system.

> [!WARNING]
> Optional bus capture logs raw keypad bytes. A PIN entry will be in that log. Keep logs private and redact them before sharing. See [docs/capture.md](docs/capture.md).

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

## Bus capture

An optional Home Assistant switch, **Nemtek Capture**, logs every received burst with per-byte microsecond gaps. A text entity, **Nemtek Capture Mark**, writes `MARK: <label>` into that log. Decoding keeps running while capture is on. The UART is still receive-only.

How to OTA-flash this branch, run a keypad checklist, and redact a log: [docs/capture.md](docs/capture.md).

Opt-in keypad transmit is off unless `tx_enabled: true`. The test stages are in [docs/transmit.md](docs/transmit.md).

## Installation

Dashboard OTA using this branch is documented in [docs/capture.md](docs/capture.md). The example YAML is [examples/nemtek-merlin.yaml](examples/nemtek-merlin.yaml). Sensor keys belong directly under `nemtek:`.

### Local ESPHome component

1. Copy `components/nemtek` into the directory containing your ESPHome YAML.
2. Copy `examples/nemtek-merlin.yaml` alongside the `components` directory.
3. In that copy, point `external_components` at the local folder (`type: local`, `path: components`) if you are not using the git `ref`.
4. Add the required secrets to `secrets.yaml` (`wifi_ssid`, `wifi_password`, `api_encryption_key`, `ota_password`).
5. Run **Clean Build Files** for the first build.
6. Validate, compile and flash the ESP32.

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

Use [docs/capture.md](docs/capture.md). Record idle traffic, then one labelled action at a time. Redact PIN bytes before sending the log.

Useful follow-up captures, after the key checklist, are a stable baseline and one state change:

- High power to low power
- Healthy fence to loaded/shorted fence
- Warning period to alarm
- Fence on to fence off
- Ordinary keypad beep without an alarm

Include the energiser model, the action performed, and the log section before, during, and after the change.

## Licence

MIT. See [LICENSE](LICENSE).
