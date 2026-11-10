# Keypad transmit test

This is opt-in. With `tx_enabled: false` the pin is never driven. GPIO21 stays the UART RX pad. While Transmit Enable is on the pad stays an input with the pull-up re-applied, so it does not float and the UART keeps the pin between polls. Output is enabled only for the reply itself, open-drain: level 0 pulls the bus low, level 1 releases it. It never drives the pin push-pull high.

The Dilbert66 MOSFET board is the bidirectional level shifter (same pin reads and writes). The MOSFET gate is tied to 3.3 V. Pulling GPIO21 low pulls DAT low. A high gate is the other circuit, the separate NPN, and this firmware does not do that. Panel idle on this pad is low, because the panel UART is inverted. A steady low after the stop bit is not treated as another device talking. The reply waits, up to 5 ms, for the pad to go high, and the read-back abort still releases the line if a bit does not match.

The real keypad owns poll `4F`. Transmit refuses that slot. The default slot is `79`, one of the silent polls. The Merlin 4 can take more than one keypad; which silent poll it will accept is not known yet.

Do not type a PIN into a mark or a text entity. The only PIN is `fence_pin: !secret fence_pin`, and the log says `pin-digit` instead of the number.

## Back out

1. Turn **Nemtek Transmit Enable** off. The log should say `Transmit off, GPIO released`.
2. Turn **Nemtek Idle Responder** off and **Nemtek Fence** off.
3. If the line stays odd, reboot the ESP. Shutdown, the example's OTA `on_begin` and safe-mode actions, and the 15 ms bit-loop deadline all release the pin.
4. Set `tx_enabled: false`, reflash, and the output driver is not enabled again.

## What you add

```yaml
logger:
  level: DEBUG
  task_log_buffer_size: 4096
  logs:
    nemtek: DEBUG

uart:
  id: nemtek_uart
  rx_pin:
    number: GPIO21
    inverted: true
  baud_rate: 2400
  data_bits: 8
  parity: ODD
  stop_bits: 1
  rx_buffer_size: 256

nemtek:
  uart_id: nemtek_uart
  parity: ODD
  edge_pin: 21
  edge_window: 30ms
  edge_after: 4F
  edge_inverted: true
  tx_enabled: false
  fence_control: false
  tx_slot: "79"
  test_digit: 0
  # Reply start measured from the poll stop bit. Defaults match a real keypad.
  # tx_reply: 2900us
  # tx_reply_min: 2500us
  # tx_reply_max: 5000us
  tx_enable:
    name: "Nemtek Transmit Enable"
  slot:
    name: "Nemtek Keypad Slot"
  idle_responder:
    name: "Nemtek Idle Responder"
  send_idle:
    name: "Nemtek Send Idle Reply"
  test_digit_number:
    name: "Nemtek Test Digit"
  send_digit:
    name: "Nemtek Send Test Digit"
  fence_switch:
    name: "Nemtek Fence"
  toggle_power:
    name: "Nemtek Toggle Power"
```

`edge_after: 4F` keeps capture logs to the real keypad only. Transmit does not use those edges. Edge capture is suspended while Transmit Enable is on.

Fence control, later:

```yaml
fence_control: true
fence_pin: !secret fence_pin
```

`secrets.yaml` holds `fence_pin: "...."` as digits only. Do not commit it.

## Stage 1 — one idle reply

1. Set `tx_enabled: true` and reflash.
2. Start `esphome logs your.yaml > tx-stage1.log` and leave it running.
3. Pick slot `79` (or another silent poll, not `4F`).
4. Turn **Nemtek Transmit Enable** on. Expect `Transmit on, slot 79, input+pull-up level=...`. The boot log also has `Reset reason:`.
5. Press **Nemtek Send Idle Reply** once.
6. The boot dump repeats `Reset reason:` next to `Direction: receive`. Look for `TX gate pass idle` and then `TX sent idle slot=79 latency=3.0ms readback=ok level-before=0`. After the poll the pad falls once, about 0.8 ms after the stop bit, and stays low. That edge is the panel's turnaround, not another device. The same gate is used for a test digit. The reply starts from that low: start and 0 pull the pad low, 1 and stop release it. `level-before=0` is expected. A read-back abort means a released bit did not rise; the line is released and transmit turns off. Do not retry until that is understood. When Transmit Enable turns on, the edge decoder's next 14 polls are `TX baseline polls: 79 F8 27 F7 4F FB 7A …` and `TX baseline check: all 7 slots present`. After a send, `TX post-send polls slot=79:` is the next 14 with `*` on our slot. `TX ack repoll` means our slot came back within the next 3 polls. The status frame should stay whatever it was.
7. If the log says `read-back` or `watchdog`, the line was released and transmit is off. Stop and check the MOSFET board before trying again.
8. Turn Transmit Enable off before the next stage.

## Stage 2 — one digit, no hash

1. Set **Nemtek Test Digit** to `0`.
2. Transmit Enable on. Press **Nemtek Send Test Digit** once.
3. Expect `TX sending test digit 0` and `TX sent test-digit slot=79`.
4. A following `TX ack repoll` means the panel accepted the address. The fence state should not change, because there is no `#`.
5. Turn Transmit Enable off.

## Stage 3 — idle responder

1. Transmit Enable on, then **Nemtek Idle Responder** on.
2. Every poll of the chosen slot should log `TX sent idle`.
3. Watch `CAP chg` and the fence sensors for a few minutes. Turn the responder off, then Transmit Enable off, if the panel starts re-polling forever or the fence state moves.
4. If `79` never gets a re-poll, turn everything off and try one other silent slot the same way. Do not use `4F`.

## Stage 4 — fence and power

Only after a slot re-polls, and only when the fence is safe to arm or drop.

1. Add `fence_control: true` and `fence_pin: !secret fence_pin`. Reflash.
2. Transmit Enable on. Idle responder off.
3. **Nemtek Fence** sends the secret digits and `#`, one key per poll of your slot, then one idle reply. It does nothing if the fence is already in the requested state.
4. Success is `TX confirmed` and the status sensor matching the switch within about 2 seconds. One automatic retry, then it stops. Three fence or power attempts in 5 minutes are refused.
5. **Nemtek Toggle Power** sends `*3#` with no PIN. With the fence on, expect a short voltage-low reading of about 3 seconds, then high or low power. Do not alarm on that transient. Whether `*3#` needs a PIN first is still unknown.
6. Turn Transmit Enable off when finished.

Voltage-low right after a power change is the panel, not a fence fault, if it clears within about 3 seconds.
