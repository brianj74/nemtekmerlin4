# Home Assistant Dashboard

Create a dashboard card using the Nemtek binary sensors exposed by ESPHome.

Status priority:

1. Communications lost
2. Alarm active
3. Fence voltage low
4. Low-power mode
5. Fence off
6. Healthy

Replace the example entity IDs with those created by your ESPHome device.

After 5 seconds without a valid Merlin frame, **Communications** turns off and the fence sensors become `unavailable`. Entity IDs do not change. When valid frames return, the fence sensors publish again after three matching bursts. Check Communications before treating a fence sensor as on or off, because an unavailable sensor is a stale reading.

**Nemtek Capture** and **Nemtek Capture Mark** are configuration entities on the ESPHome device page. See [capture.md](capture.md).
