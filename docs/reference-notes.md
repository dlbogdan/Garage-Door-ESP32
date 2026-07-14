# Reference review

Reviewed on 2026-07-14. Reference trees are cloned only under the ignored
`_references/` directory and are not firmware dependencies.

## `ixy05/homespan-garage`

- URL: `https://github.com/ixy05/homespan-garage`
- Reviewed commit: `9d4c1bf2b4e6fbd7e27c355520b478c44561898b`
- License: no license file or license declaration was present at the reviewed
  commit. Its source must therefore not be copied into this project.
- Useful concepts: HomeSpan's `Category::GarageDoorOpeners`,
  `Service::GarageDoorOpener`, `CurrentDoorState`, `TargetDoorState`, and
  `ObstructionDetected` are the correct Apple Home model.
- Rejected patterns: hardcoded credentials and GPIOs, blocking delays,
  characteristics attached to the wrong service, optimistic end-state
  reporting, and automatic two-pulse reversal. This project independently
  implements a nonblocking, one-sensor, one-pulse state machine.

## `dlbogdan/HomeKey-ESP32`

- URL: `https://github.com/dlbogdan/HomeKey-ESP32`
- Reviewed commit: `0f766e26618bfc6616b021557a71f8f92031186b`
- License: MIT, copyright 2026 rednblkx. If substantial code is ever reused,
  its copyright and permission notice must accompany that reuse.
- Useful concepts: ESP-IDF component organization, Arduino-ESP32 as a managed
  component, typed configuration ownership, NVS lifecycle, event-loop
  separation, embedded Svelte/Vite assets, and web/firmware build separation.
- Version evidence: the reviewed project pins `espressif/arduino-esp32` 3.3.5
  and accepts ESP-IDF 5.3 or newer. Compatibility still has to be proven under
  this project's exact ESP-IDF 5.5.4 constraint.

## HomeKey exclusion

No HomeKey, NFC, Wallet key, lock service, reader driver, secure-element flow,
HomeKey cryptographic material, or HomeKey entitlement is imported. Files
related to `HomeKitLock`, NFC readers, reader data, and HomeKey services were
not selected for reuse. This project exposes only Apple's standard Garage Door
Opener service through HomeSpan.

## Reuse policy

The implementation may reproduce public API usage required to integrate
ESP-IDF, Arduino-ESP32, and HomeSpan. New product logic must be written from
the architecture specification and covered by this project's tests. Any later
source-level reuse requires a documented provenance entry here before merge.

## Pinned product dependencies

- HomeSpan 2.1.8, commit `107ffc07f4455754ea89068d8cf2e992de3583e6`,
  included as a Git submodule under `third_party/HomeSpan` (MIT).
- Espressif Arduino-ESP32 3.3.5, immutable component-manager release whose
  upstream tag resolves to `11bc7ac1a458f1f4e7afc8fd4de1dfa710f31562`.
- Espressif libsodium 1.0.20~1 and mDNS 1.10.1, pinned through the ESP-IDF
  component manager.

HomeSpan 2.1.8 requires Arduino-ESP32 3.3.0 or newer and reports 3.3.8 as its
latest fully tested core. Version 3.3.5 is selected because the reference
ESP-IDF project demonstrates it as a managed component and it remains within
HomeSpan's documented supported range. The project build is the authoritative
compatibility check under ESP-IDF 5.5.4.
