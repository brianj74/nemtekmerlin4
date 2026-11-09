# Changelog

## 0.3.0 — 2026-11-09

- Opt-in open-drain transmit on GPIO21. `tx_enabled` defaults off, and **Nemtek Transmit Enable** boots off. The pin is released on shutdown, and a 15 ms deadline releases it during a frame. Slot `4F` is refused.
- Stage buttons send one idle reply or one test digit. An idle-responder switch answers the chosen silent slot with `FE 04 03`. Fence on/off and `*3#` stay behind `fence_control` and `!secret fence_pin`, which is never logged.
- Keypad edge decode now uses non-inverted 8E1 (`EDGE kp`). Capture windows stay off while transmit is armed; the edge interrupt stays on to time the poll stop bit.
- A queued reply is started from that stop-bit time by a one-shot timer, inside a 2.5–5.0 ms window (`tx_reply`, `tx_reply_min`, `tx_reply_max`). Misses are summarised every 5 s (`TX skip xN`, `TX wait: slot XX not seen`) instead of being dropped silently.

## 0.2.2 — 2026-11-09

- Capture an input-only GPIO edge window after each poll byte and print pulse timings plus 2400 baud 8E1, 8O1, 8N1, and 9-bit decode attempts. Quiet windows are counted, not printed.
- Optional `parity: ODD` (or `EVEN`). The 5 s summary reports parity and framing error counts for the parity in use.
- Idle status frames and poll bytes are counted instead of logged. Marks are logged immediately and repeated on the next summary line.
- Example logger uses `task_log_buffer_size: 4096` with `nemtek` at DEBUG.

## 0.2.1 — 2026-11-08

- Accept the 3-byte idle frame `80 FB FF` (no `DF`) as fence off, so Communications stays on while those frames arrive. Main required 5 bytes and only saw off when a slow read glued later bytes onto the frame.
- Stop installing the UART parity/framing hook and stop forcing `rx_full_threshold` / `rx_timeout`. The hook replaced ESPHome's RX wake callback and stamped a parity error on every byte. The driver already keeps those bytes.

## 0.2.0 — 2026-11-08

- Added optional receive-only bus capture. A Home Assistant switch (default off) logs every burst with per-byte microsecond gaps, parity/framing/overflow counts, and idle repeat coalescing. Fence decoding keeps running.
- Added a text entity that writes `MARK: <label>` into the log for keypad notes.
- On communications timeout, fence sensors become unavailable. The communications entity still turns off, and entity IDs are unchanged.
- Moved the example sensors directly under `nemtek:` so they match the component schema.
- Documented OTA install, the capture checklist, and PIN redaction in `docs/capture.md`.

## 0.1.0 — 2026-07-29

- Initial public project structure.
- Added ESPHome external component with YAML schema.
- Added receive-only Merlin 4 burst decoder.
- Added debounced normal, low-power, low-voltage, alarm and off states.
- Added communications timeout and provisional `EB` buzzer marker.
- Added example configuration and Home Assistant dashboard documentation.
- Removed unsupported battery, mains, gate and tamper entities.
