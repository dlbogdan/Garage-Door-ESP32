# Signal decoder board commissioning

This procedure validates the configurable decoder on an ESP32 without allowing
feedback to actuate the operator. It does not replace electrical characterization.

## Safety prerequisites

- Never connect a 12 V status/lamp terminal directly to an ESP32 GPIO.
- Use the final isolated interface (for example an optocoupler stage) and verify
  its ESP32-side output remains within 0-3.3 V before attaching the board.
- Disconnect the actuator relay contacts from the gate operator during initial
  decoder commissioning. Feedback is software-isolated from actuation, but this
  also protects against wiring mistakes and manual test-pulse use.
- Keep the operator's hardwired STOP and safety chains unchanged.

## Flash and configure

1. Build with `zsh scripts/build-firmware.sh` and flash the generated development
   image using the ESP-IDF command printed by the build.
2. Open the local management UI, sign in, and choose **Gate** → **Edit settings**.
3. Select **Custom signal rules**, then choose **Load one-wire signal example**.
4. Change GPIO 27 to the GPIO receiving the isolated logic signal. Confirm active
   level, pull mode, and debounce against the actual interface schematic.
5. Treat every example timing as uncharacterized. Save the configuration; the
   controller restarts because GPIO ownership and compiled rules are startup-owned.

## Characterize and tune

1. Return to **Gate** and observe **Live decoder diagnostics**.
2. Exercise CLOSED, OPENING, OPENED, and CLOSING using the gate's own controls or
   radio remote—not the ESP32 relay.
3. Record logical polarity, edge intervals, estimated cycles, qualifying edge
   counts, rule phases, health, selected position/movement, and obstruction state.
4. Set periodic minimum/maximum edge intervals outside measured jitter with safe
   margin. Opening and closing ranges must not overlap.
5. Set stable-level holds longer than ordinary pulse gaps with margin, then verify
   movement never briefly becomes an endpoint between pulses.
6. Verify OPENING/CLOSING appears before an endpoint, prolonged movement reaches
   the configured UNKNOWN/OBSTRUCTED result, and later endpoint proof clears only
   the rule-derived obstruction.
7. Verify ambiguity interlocks UI/HomeKit commands and emits no relay pulse.

Only reconnect actuator relay contacts after polarity and timing have been measured,
rules have been tuned, and every feedback-only test shows no output activation.
