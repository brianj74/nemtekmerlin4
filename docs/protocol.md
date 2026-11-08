# Protocol Notes

Merlin 4 frames normally begin with `80 FB`. Capture mode ([bus capture](capture.md)) logs every burst, including ones that do not start with `80 FB` and ones this table does not map. Bursts are split on a gap longer than 25 ms. Bytes inside a burst carry the microsecond gap from the previous byte so a short poll-to-reply hole stays visible.

Other Nemtek keypads (Druid notes published elsewhere) use different bytes, with polls near `40`/`48` and a keypad reply near `FE 06 <key>` about 5 ms after the poll. Treat that only as a hint. Do not assume Merlin 4 uses those values. Capture will show what this energiser actually sends.

Current experimental decoding:

| Pattern | Probable state |
|---|---|
| `80 FB FC ... DF` | Healthy / high power |
| `80 FB FC ...` | Low power |
| `80 FB FF ... DF` | Voltage low |
| `80 FB F4 FE ...` | Fence alarm |
| `80 FB FF ...` | Fence off |
| `EB` present | Probable buzzer |

Battery, mains, gate and tamper are not decoded yet.
