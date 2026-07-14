# Garage-Door-ESP32

A local-first ESP32-WROOM-32 gate controller using HomeSpan's standard Apple
Home Garage Door Opener service. The physical interface is one momentary
dry-contact relay and one closed-position sensor.

## Status

Initial implementation is in progress. The firmware now starts a password-
protected captive setup AP and accepts a complete first-boot configuration:
administrator password, station Wi-Fi, Apple Home identity, relay and sensor
GPIO settings, polarity, and travel timing. It validates and persists that
configuration before restarting. It also initializes a platform-neutral,
host-tested gate state reducer in a safe stopped/unknown state. It does not yet
configure GPIO, start HomeSpan, or operate a relay. See `docs/provisioning.md`
for the setup workflow. The normative design and safety rules are in
`plans/implementation-plan.md`.

## Fixed toolchain

ESP-IDF v5.5.4 is mandatory:

```sh
source /Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh
./scripts/verify-environment.sh
./scripts/build-firmware.sh
./scripts/test-host.sh
```

Do not connect a relay to a gate operator until the hardware safety tests are
complete. The bootstrap firmware does not configure or drive a relay.

## Current setup portal

After flashing, open the serial monitor to obtain the unique `GateSetup-XXXXXX`
SSID and generated password. Join that network and use the captive prompt or
open `http://192.168.4.1/`. The development portal currently remains active
after station Wi-Fi connects so incorrect credentials can be replaced.
