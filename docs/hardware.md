# Hardware baseline

- Board: classic ESP32-WROOM-32 DevKit.
- Output: one UI-configurable momentary dry-contact relay.
- Input: one UI-configurable closed-position sensor.
- Recovery: GPIO0/BOOT; never used as gate control.

No GPIO has a compiled operational default. Provisioning must validate and
persist safe, non-conflicting pins before the output driver can be enabled.
