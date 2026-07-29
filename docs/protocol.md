# Protocol Notes

Merlin 4 frames normally begin with `80 FB`.

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
