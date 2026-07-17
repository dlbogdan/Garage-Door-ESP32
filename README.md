# Garage Door ESP32

Local-first firmware that turns a classic ESP32-WROOM-32 into an Apple Home
Garage Door Opener and a browser-managed gate controller. The project combines
ESP-IDF, Arduino-ESP32, the pinned HomeSpan library, a serialized safety-focused
controller runtime, configurable GPIO/signal decoding, and an embedded Svelte
management UI.

> [!WARNING]
> This project can energize outputs connected to moving machinery. Commission
> feedback with actuator contacts disconnected, preserve the gate operator's
> hardwired STOP and safety circuits, and validate inactive output levels across
> boot, reboot, disconnect, and failure conditions before connecting a real
> operator. Never connect a 12 V status or lamp terminal directly to an ESP32
> GPIO; use a properly designed isolated interface.

## What is implemented

- A standard HomeKit Garage Door Opener exposed through HomeSpan, including
  current state, target state, obstruction, and target commands.
- Two operator strategies:
  - **Sequential** — one momentary `STEP` output.
  - **Directional** — independent momentary `OPEN` and `CLOSE` outputs.
- A serialized FreeRTOS gate runtime with a pure, host-tested reducer, guarded
  relay pulses, minimum pulse intervals, movement deadlines, no command replay,
  and maintenance interlocking.
- Configurable feedback decoding for up to four isolated digital inputs and
  eight rules. Rules can classify position, movement, stopped state, and
  obstruction from stable levels or periodic edge timing.
- A Svelte 5 management application embedded in the firmware, with status,
  access, network, gate, firmware, and logs views.
- Local browser controls for opening and closing the gate, subject to the same
  runtime strategy and safety interlocks used by HomeKit.
- First-boot captive setup, authenticated administration, password rotation,
  Wi-Fi migration, HomeKit pairing reset, and reboot controls.
- Native ESP-IDF A/B application OTA with upload-time maintenance mode,
  image/project validation, delayed health confirmation, and boot rollback.
- NVS-backed configuration schema migration and deterministic development
  firmware versions derived from Git and firmware/UI source content.

The firmware is functional, but it should still be treated as a development
project requiring installation-specific electrical and mechanical validation.
See [development status](docs/development-status.md) for detailed checkpoints
and known limitations.

## Hardware

### Target

- Classic ESP32-WROOM-32 DevKit with 4 MiB flash.
- One isolated dry-contact relay for the sequential profile, or two isolated
  relays for directional OPEN/CLOSE control.
- One or more properly conditioned, 0–3.3 V feedback inputs.
- GPIO0/BOOT is reserved for recovery and is rejected for gate control.

GPIOs, active levels, pull modes, pulse durations, and timing are configurable.
The current first-time UI seeds a bench-oriented sequential configuration using
GPIO26 for the relay and GPIO27 for feedback; these are not universal wiring
recommendations. Review and change them before connecting hardware. Firmware
validation rejects unsafe, unavailable, or conflicting pin assignments.

For electrical and decoder commissioning guidance, read:

- [Hardware baseline](docs/hardware.md)
- [Signal decoder commissioning](docs/signal-decoder-commissioning.md)
- [Ducati gate signal notes](docs/ducati-gate-logic.txt)

## Software stack

| Layer | Technology |
|---|---|
| Firmware SDK | ESP-IDF **v5.5.4 exactly** |
| Arduino compatibility | Arduino-ESP32 3.3.5, managed by the IDF component manager |
| Apple Home | HomeSpan, pinned as a Git submodule |
| Firmware language | C++17 |
| Management UI | Svelte 5, Vite 7, TypeScript checking, Vitest |
| Host tests | CMake/CTest |

The exact ESP-IDF version is enforced at both CMake configure time and compile
time. Other ESP-IDF versions are intentionally unsupported.

## Getting started

### 1. Clone with HomeSpan

```sh
git clone --recurse-submodules https://github.com/dlbogdan/Garage-Door-ESP32.git
cd Garage-Door-ESP32
```

For an existing clone:

```sh
git submodule update --init --recursive
```

### 2. Install and activate ESP-IDF 5.5.4

Install ESP-IDF v5.5.4 using Espressif's standard installer. The repository's
local build wrappers currently expect the activation script at:

```text
/Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh
```

If your installation lives elsewhere, update `ACTIVATE_IDF` in
[`scripts/build-firmware.sh`](scripts/build-firmware.sh) or activate ESP-IDF
manually and use `idf.py` directly. Verify the selected environment with:

```sh
./scripts/verify-environment.sh
```

Additional local requirements are Node.js/npm for the embedded UI, Python 3,
CMake, and a C++17 host compiler.

### 3. Build

```sh
./scripts/build-firmware.sh
```

The wrapper verifies ESP-IDF, runs the UI checks/tests/build when its generated
assets are stale, preserves ESP-IDF's incremental build cache, and selects the
classic `esp32` target when necessary. The development OTA image is written to:

```text
build/garage_door_esp32.bin
```

To create an explicitly versioned release build in an isolated build directory:

```sh
FIRMWARE_VERSION=0.3.3 ./scripts/build-firmware.sh
```

That produces `build-release/0.3.3/garage_door_esp32.bin`.

### 4. Flash and monitor

After activating ESP-IDF, replace `DEVICE` with the ESP32 serial device:

```sh
idf.py -p DEVICE flash monitor
```

You can leave the serial monitor with `Ctrl-]`.

## First-time setup

On an unprovisioned device, the firmware creates an open access point named
`GateSetup-XXXXXX`, where the suffix is derived from the device MAC address.
Join it and follow the captive portal, or browse directly to:

```text
http://192.168.4.1/
```

The current streamlined setup page asks for:

1. Station Wi-Fi SSID and password.
2. An administrator password containing 10–128 characters.

It then creates an initial sequential gate configuration and random HomeKit
identity values. After the controller restarts, sign in to the management UI and
review **Gate → Edit settings** before attaching an actuator. In particular,
verify every GPIO, active level, pull mode, pulse duration, travel timeout, and
feedback rule against the actual installation.

When station Wi-Fi receives an address, the setup AP stops. If station Wi-Fi is
later lost, the open recovery AP returns after the configured fallback delay.
Management remains protected by the administrator login; local HTTP traffic is
not encrypted.

## Management UI

Open the ESP32's station address in a browser and sign in with the administrator
password.

- **Status** — connectivity, decoded gate state, controller state, feedback,
  obstruction, and recovery information.
- **Access** — rotate the administrator password, inspect HomeKit pairing
  details, and remove all HomeKit controller pairings.
- **Network** — migrate to another Wi-Fi network. DHCP is currently mandatory;
  static IP, custom DNS, and hostname settings are not implemented.
- **Gate** — select sequential or directional actuation, configure output
  polarity and pulse timing, build custom feedback rules, inspect live decoder
  evidence, learn timing predicates, and issue guarded local open/close commands.
- **Firmware** — inspect application/partition state and upload an OTA
  application image with administrator re-authorization.
- **Logs** — reserved UI area; persistent event logging is not implemented.

Configuration changes that affect GPIO ownership or decoder rules are validated,
persisted, and applied after an automatic restart.

### Apple Home pairing

The **Access** tab displays the numeric HomeKit setup code and Setup ID. In the
Apple Home app, add an accessory, choose the option indicating that no QR code is
available, and enter the displayed code. The same tab can delete all controller
pairings while preserving Wi-Fi and gate configuration.

HomeSpan serves HAP on TCP port 1201; the management application remains on
TCP port 80.

## Gate and feedback model

All target requests enter one serialized runtime queue. A pure reducer decides
state changes and effects; relay admission is checked before a proposed state
transition is committed. The runtime does not resume pulses after reboot, replay
busy commands, infer endpoint success from a travel timeout, or automatically
emit corrective/reversal pulses.

Feedback is normalized into semantic observations rather than being coupled
directly to a particular sensor. Custom decoder rules support:

- stable logical levels held for a configured duration;
- periodic edges constrained by minimum/maximum intervals;
- AND predicates inside a rule group and OR between groups;
- entry/loss confirmation and match-age expiry;
- outcomes for OPENED, CLOSED, OPENING, CLOSING, STOPPED, and OBSTRUCTED.

Ambiguous or contradictory evidence can interlock commands. Carefully commission
the decoder using the gate's own controls while actuator relay contacts remain
disconnected. The detailed procedure is in
[signal decoder commissioning](docs/signal-decoder-commissioning.md).

## OTA updates

The firmware uses the inactive `ota_0`/`ota_1` application slot and accepts only
an application OTA binary—not a merged factory image, bootloader, partition
table, or filesystem image. Upload requires an authenticated session, CSRF
token, and administrator-password re-authorization. The gate must not be moving
and no output pulse may be active; commands are interlocked during the upload.

After reboot, a pending image must pass local startup checks and a 15-second
health soak before it is confirmed. A crash, watchdog reset, or failed local
startup check causes bootloader rollback. Configuration and HomeKit pairing data
remain in NVS across application-slot changes.

Current development OTA validates ESP-IDF image structure, slot size, project
identity, and version difference, but **does not cryptographically authenticate
the publisher**. Signed-app verification/Secure Boot and their key-management
workflow are not enabled, so this is not yet a production-secure update chain.
Read [the OTA design and status](docs/ota-update.md) before deployment.

## Tests and development workflow

### Host firmware tests

```sh
/bin/zsh ./scripts/test-host.sh
```

This configures, builds, and runs CTest suites covering HTTP route capacity,
operator-domain behavior, the gate reducer, signal decoding, configuration
validation, sensor debouncing, relay pulse guarding, and HomeKit projection.

### UI checks and tests

```sh
./scripts/build-ui.sh
```

This performs a clean npm install, runs `svelte-check`, executes Vitest, builds
the production UI, and copies stable `index.html`, `app.js`, and `app.css` assets
into the management-server component for firmware embedding.

For UI-only work from the `webui` directory:

```sh
npm ci
npm run check
npm test -- --run
npm run build
```

### Recommended pre-flash validation

```sh
/bin/zsh ./scripts/test-host.sh
./scripts/build-ui.sh
./scripts/build-firmware.sh
```

## Architecture

The firmware is split into small ESP-IDF components with explicit ownership:

| Component | Responsibility |
|---|---|
| `app_config` | Schema-v4 configuration, validation, password derivation, and NVS persistence/migration |
| `bootstrap_credentials` | Minimal staged/recovery Wi-Fi credentials |
| `network_manager` / `captive_dns` | Setup/recovery AP policy and captive DNS |
| `management_server` | Native HTTP server, embedded static assets, and route registration |
| `setup_api` | Unauthenticated first-time setup routes |
| `management_api` / `web_auth` | Authenticated APIs, sessions, CSRF, and re-authorization |
| `gate_hardware` | Native GPIO input sampling and fail-safe output operation |
| `signal_decoder` | Bounded configurable feedback-rule engine |
| `gate_controller` | Pure reducer, operator strategy, and relay guards |
| `gate_runtime` | Serialized FreeRTOS event/effect/deadline owner |
| `homespan_bridge` | HomeKit service and characteristic projection |
| `ota_api` / `ota_manager` | Authenticated upload transport and A/B OTA state machine |
| `provisioning` | Bootstrap orchestration across network, DNS, and management services |

Arduino/HomeSpan is the sole owner of station Wi-Fi initialization and
reconnection. Native ESP-IDF HTTP, DNS, GPIO, NVS, FreeRTOS, and OTA services run
alongside it. The pinned HomeSpan source under `third_party/HomeSpan` should not
be modified as part of normal project development.

## Security posture and limitations

- Administrator passwords are stored as salted PBKDF2-HMAC-SHA-256 verifiers,
  not plaintext.
- Browser sessions use random in-memory cookies and session-bound CSRF tokens;
  sensitive mutations require CSRF, and selected operations require password
  re-authorization.
- Wi-Fi passwords and password-verifier material are not returned by APIs.
- The management server is HTTP-only. Use it only on trusted local networks.
- The recovery AP is open when active; the management login remains required
  after provisioning.
- OTA publisher signatures and Secure Boot are not enabled.
- Static addressing, persistent event logs, and BOOT-button factory reset are
  not implemented.
- This repository does not replace compliant obstruction detection, safety
  edges, photo-eyes, emergency stops, or supervised installation.

## Repository map

```text
components/   ESP-IDF firmware components
main/         Application entry point
webui/        Svelte/Vite management UI
test/host/    Native C++ and policy tests
scripts/      Environment, build, test, and capacity-check wrappers
docs/         Operational notes, commissioning, status, and OTA documentation
plans/        Historical design and implementation plans
third_party/  Pinned HomeSpan submodule
```

The implementation is authoritative when older planning/status documents differ
from current behavior.

## License

The original project code is proprietary and source-available under the
[Garage Door ESP32 Source-Available License](LICENSE). Individual DIY users may
download, modify, build, install, and use the project without contacting the
author when it is solely for their own private, non-commercial use on devices
they own and control.

Personal-use permission does not include redistribution, installing or building
devices for others, paid work, hosted services, organizational use, or use in
another project, product, service, or commercial/OEM offering.

DIY users may also publicly fork, modify, and redistribute non-commercial
source-code derivatives under the same license. Forks must provide complete
source, preserve attribution and notices, identify their changes, remain under
this unmodified license, and clearly state that they are unofficial. This
permission does not cover distributing compiled firmware, programmed or
assembled devices, paid installations, or hosted services.

Product developers and other parties seeking authorization must obtain a
separate written license from Bogdan Dumitru at
[bogdan.dumitru@me.com](mailto:bogdan.dumitru@me.com).

Third-party dependencies, submodules, and managed components remain under their
respective licenses. In particular, the pinned HomeSpan source under
[`third_party/HomeSpan`](third_party/HomeSpan) is not covered by the project's
proprietary license.

## Further documentation

- [Development status](docs/development-status.md)
- [Provisioning and management boundaries](docs/provisioning.md)
- [Hardware baseline](docs/hardware.md)
- [Signal decoder commissioning](docs/signal-decoder-commissioning.md)
- [OTA update design and status](docs/ota-update.md)
- [Operator profile architecture](plans/operator-profile-architecture.md)
- [Implementation plan and safety invariants](plans/implementation-plan.md)
