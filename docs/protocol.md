# Protocol Notes

Merlin 4 frames normally begin with `80 FB`. Capture mode ([bus capture](capture.md)) counts idle frames and, while capture is on, records GPIO edges for 30 ms after each poll byte. Bursts are split on a gap longer than 25 ms. Bytes inside a burst carry the microsecond gap from the previous byte so a short poll-to-reply hole stays visible.

Other Nemtek keypads (Druid notes published elsewhere) use different bytes, with polls near `40`/`48` and a keypad reply near `FE 06 <key>` about 5 ms after the poll. Treat that only as a hint. Do not assume Merlin 4 uses those values. Capture will show what this energiser actually sends.

Current experimental decoding:

| Pattern | Probable state |
|---|---|
| `80 FB FC ... DF` | Healthy / high power |
| `80 FB FC ...` | Low power |
| `80 FB FF ... DF` | Voltage low |
| `80 FB F4 FE ...` | Fence alarm (needs the `FE` byte) |
| `80 FB FF` | Fence off. An idle fence-off Merlin 4 sends this as 3 bytes. Any longer `80 FB FF ...` burst with no `DF` is the same state |
| `EB` present | Probable buzzer |

Battery, mains, gate and tamper are not decoded yet.
