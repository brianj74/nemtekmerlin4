# Wiring

| Nemtek Merlin 4 | ESP32 |
|---|---|
| DAT | GPIO21 through the level shifter |
| GND | GND |
| Positive supply | Not connected |

Power the ESP32 separately by USB.

UART: 2400 baud, 8 data bits, even parity, 1 stop bit, inverted RX.

Configure `rx_pin` only. This phase does not set `tx_pin` and does not write the DAT line. See [capture.md](capture.md).
