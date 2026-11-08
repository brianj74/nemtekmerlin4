# Bus capture (receive only)

Capture mode logs every byte on the Merlin keypad bus so a later phase can learn how keypresses are framed. This phase only receives. The firmware never writes the UART, and the example does not set `tx_pin`. Pressing keys is still done on the physical Nemtek keypad.

Compiled against ESPHome **2026.10.2**. Home Assistant needs the fence-sensor `unavailable` state, which requires ESPHome **2025.7** or newer (`binary_sensor` `invalidate_state`).

## Privacy

A full PIN entry is on the wire, so it will be in the log as raw bytes. The encoding is not known yet, which means the digits cannot be stripped automatically.

- Keep ESPHome logs and Home Assistant history private.
- Do not type the PIN into **Nemtek Capture Mark**. Label the action (`fence on`, `digit 4`), not the code.
- Redact before sharing. Steps are at the end of this page.
- Turn capture off when the checklist is finished. The switch boots off unless you change `restore_mode`.

## Flash from the ESPHome dashboard

The dashboard device you already have can stay. Edit its YAML rather than replacing Wi-Fi, API, or OTA.

1. Open the device in the ESPHome dashboard and choose **Edit**.
2. Point `external_components` at this branch:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/brianj74/nemtekmerlin4
      ref: cursor/nemtek-capture-d374
    components: [nemtek]
```

3. Keep the UART receive-only. GPIO21, inverted, 2400 8E1. Do not add `tx_pin`.

```yaml
uart:
  id: nemtek_uart
  rx_pin:
    number: GPIO21
    inverted: true
  baud_rate: 2400
  data_bits: 8
  parity: EVEN
  stop_bits: 1
  rx_buffer_size: 256
  rx_full_threshold: 1
  rx_timeout: 1
```

4. Put sensors directly under `nemtek:`. A nested `binary_sensor:` block does not match the component and the config will fail validation. Add the capture switch and the mark text:

```yaml
nemtek:
  uart_id: nemtek_uart
  capture_coalesce: true
  capture:
    name: "Nemtek Capture"
    restore_mode: ALWAYS_OFF
  mark:
    name: "Nemtek Capture Mark"
    mode: TEXT
  communications:
    name: "Nemtek Communications"
  # ...the other sensors, same names as before
```

`capture_coalesce` defaults to true. Leave it on for the checklist.

5. **Install** → **Wirelessly**. The first install of this branch should be a normal OTA update from the dashboard.
6. If the build still uses the previous component, run **Clean Build Files** and install again. ESPHome caches git external components.

A full board config is in [examples/nemtek-merlin.yaml](../examples/nemtek-merlin.yaml). Copy it only for a new device. An existing device keeps its own `esp32:`, `wifi:`, `api:`, and `ota:` blocks.

## Turn capture on

In Home Assistant: **Settings → Devices & services → ESPHome → the Nemtek device**.

Capture and Mark are configuration entities on that device page (they are not fence-status sensors).

1. Open **Logs** for the device in the ESPHome dashboard. Leave the level at **INFO**.
2. Turn **Nemtek Capture** on.
3. Confirm a warning line: `Capture ON. Logs may contain keypad PIN digits...`
4. Fence decoding keeps running. Communications and the fence sensors still update.

The switch starts off after every reboot (`restore_mode: ALWAYS_OFF`).

## Marks

Before each physical keypress, set **Nemtek Capture Mark** to a short label and submit it. The log gains a line:

```text
MARK: digit 4 t_us=123456789
```

The field clears itself so the same label can be sent again. `t_us` is microseconds since boot, the same clock as `CAP t_us=`.

If capture is off, the line is still written and ends with `(capture is off)`.

Use labels such as `idle`, `digit 0` … `digit 9`, `hash`, `star`, `fence on`, `fence off`, `power`. Do not put the PIN in the label.

## Read a capture line

The logger's own timestamp is when the line was printed. Byte timing is the `t_us` / `+` fields.

```text
CAP t_us=1000000 gap_us=18000 n=6 sync=1 map=off drop=0 P=0 F=0 O=0
CAP | 80+0 FB+4583 FC+4590 FF+4610 11+4588 00+4592
CAP repeat x40 gap_us=16000-19000 n=6 80 FB FC FF 11 00
```

| Field | Meaning |
|---|---|
| `t_us` | `micros()` when the first byte of the burst was read |
| `gap_us` | Time since the previous burst's last byte. `0` on the first burst after boot |
| `n` | Bytes stored (maximum 48) |
| `sync` | `1` when the burst starts `80 FB` |
| `map` | Decoder result: `normal`, `low-power`, `voltage-low`, `alarm`, `off`, or `none` |
| `drop` | Extra bytes past the 48-byte buffer |
| `P` `F` `O` | Parity, framing, and RX-FIFO overflow counts for the burst |
| `err=n/a` | This build could not see UART error flags |
| `batch=1` | At least one byte was already queued when the loop read it |
| `80+4583` | Byte `0x80`, then microseconds since the previous byte in the burst. The first byte is `+0` |
| `!P` `!F` `!O` | Error flag attached to that byte (best effort; the header counts are exact) |
| `*` | Delta under 250 µs, so the byte was sitting in the driver buffer. That number is not wire time |
| `CAP repeat xN` | N further copies of the same bytes were counted instead of logged in full |

Bursts split after **25 ms** of idle. A reply that follows a poll by a few milliseconds stays inside the same `CAP` record. Look for a `+` delta larger than the back-to-back byte time.

At 2400 baud, 8 data bits, even parity, and 1 stop bit, one byte is 11 bit times: **4583 µs**. Bytes that follow each other on the wire land near that number. An extra idle of about 5 ms (the sort of poll-to-reply gap seen on other Nemtek keypads) shows up as a delta several milliseconds larger than 4583.

`map=none` is still logged. Those are the bursts the state decoder ignores, which is the point of capture.

Identical bursts are coalesced when `capture_coalesce` is true. The first copy is logged in full. Further copies increment a counter, with a `CAP repeat` line at least every 5 seconds and again when the bytes change. A copy whose internal gaps differ by more than 2 ms is logged in full, so a delayed reply is not swallowed. Near-zero `*` deltas are ignored for that comparison.

Parity, framing, and RX overflow come from the ESP32 UART status seen in the driver interrupt. The chip does not store the flag next to the byte, so `!P` / `!F` / `!O` may sit on the byte read just after the error. `P` / `F` / `O` on the header are the counts for the burst. Overflow means at least one byte was lost in the hardware FIFO.

## Save the log

In the ESPHome dashboard, open **Logs** and use the download control on the log window after the checklist. Copy from the `Capture ON` line through `Capture off`.

From a shell, if you use the ESPHome CLI against the same YAML:

```bash
esphome logs examples/nemtek-merlin.yaml > nemtek-capture.log
```

Stop the command after `Capture off`. The redirect file is local and should be treated as secret until it is redacted.

## Checklist

Do this with the physical keypad. The ESP32 does not send keys. Arm or disarm the real fence only when it is safe for the fence to energise or drop.

Wait about a second between steps so each action is its own gap in the log. Submit the mark, then press the key.

1. Turn **Nemtek Capture** on and confirm the privacy warning.
2. Mark `idle`. Leave the keypad alone for about 30 seconds.
3. For each digit, mark `digit 0`, press `0`, then `digit 1`, press `1`, and so on through `digit 9`.
4. Mark `hash`, press `#`.
5. Mark `star`, press `*`.
6. Mark `fence on`. Enter the full PIN and press `#` so the fence turns on.
7. Mark `fence off`. Enter the full PIN and press `#` so the fence turns off.
8. Mark `power`. Enter `*3#` (Merlin power mode).
9. Mark `done`. Turn **Nemtek Capture** off.
10. Save the log. Redact it before sending it anywhere.

Normal fence sensors should keep updating during the capture. After the bus has been quiet for 5 seconds they become **unavailable** in Home Assistant. **Nemtek Communications** turns off. Both recover when valid `80 FB` bursts return (three matching bursts for the fence state).

## Redact before sharing

Work on a copy. Keep the original somewhere private.

1. Read every `MARK:` line. Replace any that contain a PIN with the action name only (`fence on`, `digit 4`).
2. On `CAP |` lines, replace each `HH+` byte with `XX+`. On `CAP repeat` lines, replace the hex fingerprint after `n=` the same way. Leave `t_us`, `gap_us`, and the `+` timings.

```text
CAP | 80+0 FB+4583 FC+4590 FF+4610
CAP repeat x40 gap_us=16000-19000 n=6 80 FB FC FF 11 00
```

becomes:

```text
CAP | XX+0 XX+4583 XX+4590 XX+4610
CAP repeat x40 gap_us=16000-19000 n=6 XX XX XX XX XX XX
```

From the directory that contains the saved log:

```bash
python3 - <<'PY'
import re
from pathlib import Path
src = Path("nemtek-capture.log")
dst = Path("nemtek-capture-redacted.log")
out = []
for line in src.read_text(errors="replace").splitlines():
    if "CAP |" in line:
        line = re.sub(r"\b[0-9A-Fa-f]{2}\+", "XX+", line)
    elif "CAP repeat" in line:
        head, sep, tail = line.rpartition(" n=")
        if sep:
            count, _, finger = tail.partition(" ")
            finger = re.sub(r"\b[0-9A-Fa-f]{2}\b", "XX", finger)
            line = f"{head}{sep}{count} {finger}".rstrip()
    out.append(line)
dst.write_text("\n".join(out) + "\n")
print(f"wrote {dst}")
PY
```

Open the redacted file and search for `CAP |` and `CAP repeat`. The only hex-looking pairs left should be inside timestamps, not payload bytes.

3. If a reviewer needs real idle bytes, send only the `idle` section, and only after you have confirmed no digit was pressed in that window. Send `digit`, `fence on`, and `fence off` as timings only.
4. Do not post the unredacted log on a public issue, chat, or email thread.

## What to send back

With the redacted log, include:

- Energiser model (Merlin 4, single zone) and that the keypad is the original Nemtek keypad
- ESPHome version from the dashboard
- The checklist steps you completed, and any you skipped
- Whether `P`, `F`, or `O` were ever non-zero
- Whether fence on / fence off actually changed the energiser
