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

3. Keep the UART receive-only. GPIO21, inverted, 2400 baud. Do not add `tx_pin`. Use odd parity first; switch the same key to `EVEN` only to compare the `P=` count.

```yaml
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
```

4. Logger. `task_log_buffer_size` is the ESPHome 2026.10 key (default 768, which dropped lines). `level: DEBUG` keeps nemtek byte timings in the binary. Idle frames are not printed.

```yaml
logger:
  level: DEBUG
  task_log_buffer_size: 4096
  logs:
    nemtek: DEBUG
    wifi: INFO
    api: INFO
    uart: INFO
    mdns: INFO
```

Keep `baud_rate: 0` if that is how this board already logs (USB). Do not point the logger UART at GPIO21.

5. Put sensors directly under `nemtek:`. A nested `binary_sensor:` block does not match the component and the config will fail validation. `edge_pin` only enables an input edge interrupt on the existing RX pad.

```yaml
nemtek:
  uart_id: nemtek_uart
  parity: ODD
  edge_pin: 21
  edge_window: 30ms
  edge_after: ALL   # or 4F, to arm a window only after poll 4F
  edge_inverted: true
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

`capture_coalesce` defaults to true. Leave it on. `edge_after: 4F` is enough for the known keypad slot; `ALL` also arms the other poll bytes and only prints a window when extra edges appear.

6. **Install** → **Wirelessly**. The first install of this branch should be a normal OTA update from the dashboard.
7. If the build still uses the previous component, run **Clean Build Files** and install again. ESPHome caches git external components.

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
MARK: digit 4 t_us=123456789
CAP chg map=off n=5 80 FB FF FF FF
CAP sum st=45 poll=40 4F=6 other=0 edge_q=34 edge_hit=1 uart=ODD P=0 F=0 marks=1
CAP sum mark: digit 4 t_us=123456789
EDGE poll=4F t_us=123400000 n=18 mark=digit 4
EDGE dt 4200L 4610H 5020L 5440H
EDGE try E1=7E/bad O1=7E/ok N1=7E/bad 9=07E
```

| Field | Meaning |
|---|---|
| `t_us` | `micros()` when the first byte of the burst was read |
| `gap_us` | Time since the previous burst's last byte. `0` on the first burst after boot |
| `n` | Bytes stored (maximum 48) |
| `sync` | `1` when the burst starts `80 FB` |
| `map` | Decoder result: `normal`, `low-power`, `voltage-low`, `alarm`, `off`, or `none` |
| `drop` | Extra bytes past the 48-byte buffer |
| `batch=1` | At least one byte was already queued when the loop read it |
| `80+4583` | Byte `0x80`, then microseconds since the previous byte in the burst. The first byte is `+0` |
| `*` | Delta under 250 µs, so the byte was sitting in the driver buffer. That number is not wire time |
| `CAP chg` | Status bytes changed. Repeated idle frames are not printed |
| `CAP sum` | Counts for the last 5 seconds, including UART parity (`P`) and framing (`F`) errors for the parity in `uart=` |
| `CAP sum mark` | Copy of a `MARK:` line, so a dropped immediate line is still in the file |
| `EDGE poll` | A poll byte was followed by extra edges inside the window. `dt` values are microseconds from the UART poll byte, then `H` or `L` for the raw pad level |
| `EDGE try` | Same edges sampled at 2400 baud. `E1` even parity, `O1` odd parity, `N1` 8 data bits plus stop, `9` nine data bits. `/ok` means the parity or stop bit matched |

The energiser sends a status frame about every 110 ms, then one poll byte (`79 F8 27 F7 4F FB 7A`). The real keypad answers `4F`, starting about 8–10 ms after the poll byte starts. The edge window opens when the UART delivers that poll byte and stays open for `edge_window` (30 ms). Windows with no later edges increment `edge_q` and are not printed.

Bursts still split after **25 ms** of idle. Fence-off can be the 3-byte frame `80 FB FF` or the longer `80 FB FF FF FF`. Either maps to off when there is no `DF`. Communications stays on while those frames arrive. After 5 seconds without one, Communications turns off and the fence sensors become unavailable.

Per-byte UART timings (`CAP t_us` / `CAP |`) are DEBUG. They are emitted for a status change and for a non-poll burst, not for every idle frame. Set `logger` `level: DEBUG` and `logs: nemtek: DEBUG` or those lines are compiled out.

`P=` and `F=` are counted from the UART error interrupt. The count is labeled with the parity actually programmed (`uart=ODD` or `uart=EVEN`). A parity error does not discard the byte and does not reset the FIFO. The RX wake callback is still invoked.

## Save the log

In the ESPHome dashboard, open **Logs** and use the download control on the log window after the checklist. Copy from the `Capture ON` line through `Capture off`.

Save from the CLI in one unbroken session. A dashboard log window dropped lines when the task buffer was 768 bytes, including a Mark.

```bash
esphome logs nemtek-merlin.yaml > nemtek-capture.log
```

Leave that command running for the whole checklist. Stop it only after `Capture off`. The file is secret until it is redacted.

## Checklist

Do this with the physical keypad. The ESP32 does not send keys. Arm the fence only when that is safe. Do not type the PIN into **Nemtek Capture Mark**. Use `digit 4`, not the code.

Leave about 3 seconds between keys so each press has its own edge window and the log can keep up.

1. Start `esphome logs` into a file, as above.
2. Turn **Nemtek Capture** on. Confirm `Capture ON` and a `CAP sum` line with `uart=ODD`.
3. Mark `idle`. Leave the keypad alone for about 30 seconds. `edge_hit` should stay 0 if nobody presses a key.
4. For each digit, mark `digit 0`, press `0`, wait 3 seconds, then `digit 1`, and so on through `digit 9`.
5. Mark `hash`, press `#`, wait 3 seconds.
6. Mark `star`, press `*`, wait 3 seconds.
7. With the fence **on**, mark `power` and enter `*3#`. Power mode is not visible on the status frame while the fence is off.
8. Mark `done`. Turn **Nemtek Capture** off. Stop `esphome logs`.
9. Redact the file before sending it anywhere.

Fence-on and fence-off PIN entries are optional and only when the fence is safe to arm or drop. Mark those `fence on` and `fence off`. The PIN stays on the keypad, not in the mark text.

Normal fence sensors should keep updating during the capture. After the bus has been quiet for 5 seconds they become **unavailable** in Home Assistant. **Nemtek Communications** turns off. Both recover when valid `80 FB` bursts return (three matching bursts for the fence state).

## Redact before sharing

Work on a copy. Keep the original somewhere private.

1. Read every `MARK:` line. Replace any that contain a PIN with the action name only (`fence on`, `digit 4`).
2. On `CAP |` lines, replace each `HH+` byte with `XX+`. On `CAP chg`, `CAP other`, and `EDGE try` lines, replace hex payload bytes with `XX`. Leave `EDGE dt` timings and `H`/`L` levels: those are the pulse record. They still describe the bits, so the redacted file is for the people analysing this bus, not for a public post.

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
    elif "CAP chg" in line or "CAP other" in line:
        line = re.sub(r"\b[0-9A-Fa-f]{2}\b", "XX", line)
    elif "EDGE try" in line:
        line = re.sub(r"(?:=)[0-9A-Fa-f?]{2,4}", r"=XX", line)
    elif "EDGE poll=" in line:
        line = re.sub(r"poll=[0-9A-Fa-f]{2}", "poll=XX", line)
    out.append(line)
dst.write_text("\n".join(out) + "\n")
print(f"wrote {dst}")
PY
```

`EDGE dt` lines are unchanged. Check `MARK:` lines and `CAP sum mark:` lines do not contain a PIN.

3. If a reviewer needs real idle bytes, send only the `idle` section, and only after you have confirmed no digit was pressed in that window. Send `digit`, `fence on`, and `fence off` as timings only.
4. Do not post the unredacted log on a public issue, chat, or email thread.

## What to send back

With the redacted log, include:

- Energiser model (Merlin 4, single zone) and that the keypad is the original Nemtek keypad
- ESPHome version from the dashboard
- The checklist steps you completed, and any you skipped
- The `uart=` value and whether `P=` or `F=` in `CAP sum` was zero
- Whether any `EDGE` window appeared for digit, `*`, and `#` presses
- Whether fence on / fence off actually changed the energiser
