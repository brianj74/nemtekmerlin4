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

After `*3#` with the fence on, the bus can show fence-voltage-low for about 3 seconds before high power settles. Treat voltage-low as an alert only if it stays for longer than 3 seconds. The component does not add a Home Assistant automation for that.

Transmit controls, if you enable them, are documented in [transmit.md](transmit.md).
