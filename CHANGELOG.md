# Changelog

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
