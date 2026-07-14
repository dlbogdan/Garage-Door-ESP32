# Hardware baseline

- Board: classic ESP32-WROOM-32 DevKit.
- Output: one UI-configurable momentary dry-contact relay.
- Input: one UI-configurable closed-position sensor.
- Recovery: GPIO0/BOOT; never used as gate control.

No GPIO has a compiled operational default. Provisioning validates and persists
safe, non-conflicting pins before the hardware runtime starts.

The current sensor-first runtime uses native ESP-IDF GPIO APIs to set the
configured relay output latch to its inactive level before enabling output
mode, then keeps it inactive. It samples
the configured closed-position sensor every 10 ms and reports only states that
remain stable for the configured debounce interval. Command-triggered relay
pulses are deliberately disabled until boot-glitch and sensor behavior have
been validated on physical hardware.
