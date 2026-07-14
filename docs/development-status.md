# Development status and restart handoff

Last updated: 2026-07-14

## Repository state

- Repository: `https://github.com/dlbogdan/Garage-Door-ESP32`
- Branch: `main`
- Latest pushed commit: `6dfa18e` — `Add safe sensor hardware monitoring`
- Previous management milestone: `c4334db` — `Add tabbed credential management`
- Worktree now contains the uncommitted serialized controller runtime milestone
  described below. `6dfa18e` remains the latest pushed commit.
- Target hardware: classic ESP32-WROOM-32 DevKit.
- Required SDK: ESP-IDF v5.5.4 exactly.
- Arduino-ESP32 component: 3.3.5.
- HomeSpan submodule commit: `107ffc07f4455754ea89068d8cf2e992de3583e6`.

## Current functional behavior

### Provisioning and networking

- Arduino/HomeSpan is the sole owner of Wi-Fi driver initialization, default
  netif creation, and Arduino network-event translation.
- Application provisioning retains AP+STA, credential, fallback, and reconnect
  policy, but now applies it through Arduino's public `WiFi` API. It no longer
  initializes `esp_netif`/`esp_wifi` or creates default netifs directly.
- Creates a WPA2 setup AP named `GateSetup-XXXXXX`.
- Generates a random per-device AP password and persists it in NVS.
- Runs captive DNS and an HTTP portal at `http://192.168.4.1/`.
- Operates in AP+STA mode; the fallback setup AP remains available after station
  connection.
- Supports authenticated migration to another Wi-Fi network.
- Wi-Fi migration updates both application and bootstrap credential stores,
  restarts the controller, and preserves fallback AP recovery.
- Saved Wi-Fi passwords are never returned to the browser.

### Svelte management UI

- Embedded Svelte 5/Vite UI with stable `app.js` and `app.css` asset names.
- HTML, JavaScript, and CSS are served with `Cache-Control: no-store` to prevent
  stale firmware/UI combinations.
- Authenticated dashboard tabs:
  - **Status**
  - **Access**
  - **Network**
  - **Gate**
  - **Logs**
- **Access** supports administrator-password rotation.
- Password rotation requires the current password, creates a new random salt and
  verifier, invalidates the active session, and requires signing in again.
- **Network** supports changing station SSID/password with administrator
  re-authentication.
- **Gate** supports validated editing of display name, relay GPIO/polarity/pulse,
  sensor GPIO/polarity/pull, and opening/closing durations.
- **Status** polls `/api/v1/runtime` once per second while visible and displays
  the stable closed-sensor state without a manual browser refresh.
- Passive runtime polling validates the session but does not refresh its idle
  timer.
- Static IP, custom DNS, hostname, Apple pairing QR management, and persistent
  event logs are shown as planned—not implemented controls.

### Authentication and security

- Administrator passwords use PBKDF2-HMAC-SHA-256 with a random 16-byte salt,
  32-byte verifier, and 60,000 iterations.
- Plaintext administrator passwords are never persisted.
- Password comparison is constant-time.
- PBKDF2 derivation now yields every 128 rounds on ESP32. This preserves existing
  verifier compatibility and prevents the HTTP task from starving the CPU idle
  task and triggering the task watchdog during login.
- Sessions use a random opaque in-memory cookie and per-session CSRF token.
- Cookie properties: `HttpOnly`, `SameSite=Strict`, `Path=/`.
- Session limits: 30-minute idle timeout and 8-hour absolute timeout.
- CSRF is required for logout, reboot, settings updates, password changes, and
  Wi-Fi migration.
- APIs do not return Wi-Fi passwords, setup AP passwords, administrator
  salt/verifier, or session tokens. The authenticated Apple Home endpoint now
  returns the pairing code and Setup ID to the signed-in administrator.

### Hardware runtime

- New `gate_hardware` component uses native ESP-IDF GPIO APIs. Do not switch it
  back to Arduino GPIO calls before the Arduino runtime is explicitly started.
- The configured relay output latch is set to the inactive level before output
  mode is enabled, then set inactive again.
- External relay command submission remains deliberately disabled.
- The configured closed-position sensor is sampled every 10 ms.
- Sensor changes must remain stable for the configured debounce interval before
  becoming visible.
- Sensor status is exposed through a read-only authenticated runtime endpoint.
- Serial diagnostics report sensor GPIO, raw electrical level, configured active
  polarity, and interpreted stable state.
- Physical validation completed successfully:
  - relay remained inactive during boot/reboot;
  - native GPIO sensor levels changed correctly;
  - debounced sensor status updated automatically in the browser;
  - login worked without task-watchdog warnings.

### Serialized controller runtime

- New `gate_runtime` component owns the reducer snapshot in one FreeRTOS task.
- Debounced closed-sensor transitions are sent through a 16-entry event queue;
  the sensor task never mutates controller state.
- Opening, closing, sensor-release, and pulse-completion deadlines are owned and
  processed by the controller task.
- The pure reducer remains the only source of controller state transitions and
  requested effects.
- Relay pulse admission is checked before a reducer transition is committed, so
  a rejected/failed pulse cannot falsely advance target or movement state.
- The relay driver rejects overlap, enforces the configured minimum interval
  with wrap-safe elapsed-time arithmetic, and always attempts fail-safe
  deactivation at pulse completion.
- Relay output still initializes inactive before output mode is enabled, and no
  pulse is resumed after restart.
- No public target-command API exists yet. HomeSpan and HTTP cannot request a
  normal gate movement. An authenticated bench-test endpoint can request a
  state-neutral relay pulse through the serialized controller task.
- The Gate tab now provides **Test relay pulse**. It energizes the configured
  relay GPIO for the configured pulse duration and reports accepted, busy,
  unavailable, or GPIO-failure results.
- Bench pulses require an authenticated session and CSRF token. They retain the
  overlap and minimum-interval guards but do not require password re-entry or a
  confirmation phrase.
- Runtime startup replaced the temporary reducer demonstration in `app_main`.
- Normal target requests are now accepted through the same serialized queue for
  HomeKit. Read-only runtime snapshots are copied under a critical section for
  characteristic publication.
- An authoritative active closed sensor now aligns both boot state and target to
  closed, avoiding a contradictory CLOSED/current and OPEN/target pair.

### Apple Home / HomeSpan

- `app_main` invokes `initArduino()` through the bridge before provisioning.
  HomeSpan's normal initialization creates the Wi-Fi driver, default station
  netif, and Arduino network-event infrastructure exactly once.
- Provisioning uses Arduino `WiFi.mode(WIFI_AP_STA)` and `WiFi.softAP()` to
  preserve the custom fallback AP. HomeSpan alone calls `WiFi.begin()` and owns
  station connect/reconnect after its polling task is ready to consume Arduino
  network events; provisioning only observes those events for UI status.
- Native ESP-IDF HTTP and captive-DNS services continue running on the
  Arduino-created interfaces; REST APIs, sessions, CSRF, embedded UI, and
  bootstrap credential storage are unchanged.
- The management HTTP server remains on TCP port 80. HomeSpan HAP uses dedicated
  TCP port 1201 and advertises it through `_hap._tcp`; this prevents Apple
  `POST /pair-setup` requests from reaching the management server and receiving
  `405 Method Not Allowed`.
- The pinned HomeSpan source is unmodified. Project code contains no
  external-Wi-Fi extension or synthetic connection notification.
- A live HomeSpan Garage Door Opener service now publishes current state, target
  state, and obstruction from the serialized reducer runtime.
- Apple Home target writes are queued into the controller runtime. Rejected busy
  requests are returned to HomeKit instead of being delayed or replayed.
- The Access tab shows HomeSpan readiness/pairing state, the formatted pairing
  code, Setup ID, and numeric-code pairing instructions to authenticated users.
- Pairing QR rendering and pairing reset/removal controls remain unimplemented.

### Gate state reducer

- Pure deterministic reducer exists in `components/gate_controller`.
- Configuration schema v2 separates feedback electrical polarity from whether
  ACTIVE means OPEN or CLOSED. Schema-v1 configurations migrate in memory with
  ACTIVE=CLOSED and a 2000 ms endpoint-stability default.
- Boot remains `UNKNOWN_STOPPED` until one feedback level remains stable for the
  configured endpoint-stability interval.
- Feedback edges restart endpoint stability timing, filtering the Ducati
  controller's approximately one-second movement blink. Only a proved stable
  feedback level changes state to OPEN or CLOSED.
- OPENING and CLOSING are used only for commands originated by this firmware.
  External/radio operation synchronizes when feedback proves an endpoint without
  inventing an intermediate direction.
- An opposite Apple target while moving emits one pause pulse, reports STOPPED,
  and retains the previous directional target. Selecting the opposite target
  again emits one new pulse and begins reverse movement. No automatic second
  pulse is allowed.
- Opening/closing durations are fault timeouts only. Expiry reports STOPPED,
  sets obstruction, retains the requested destination, and never infers endpoint
  success or emits another pulse.
- Supports target requests, sensor transitions, movement timers, pulse
  completion, obstruction, busy rejection, and idempotency.
- Safety properties already tested:
  - at most one pulse per accepted target-changing command;
  - no delayed replay of rejected commands;
  - no automatic correction pulse;
  - closed sensor is authoritative;
  - reboot does not resume movement.
- The reducer is integrated into the serialized runtime task, while remaining
  pure and independently host-tested.

## Build and test workflow

### Incremental firmware build

Run:

```sh
/bin/zsh ./scripts/build-firmware.sh
```

The wrapper now:

- selects the ESP32 target only if `sdkconfig` is missing or targets something
  else;
- preserves the Ninja cache for normal builds;
- skips npm/Svelte work when embedded UI assets are newer than UI sources;
- performs an ordinary incremental ESP-IDF build.

Do not reintroduce unconditional `idf.py set-target esp32`; it forces
`fullclean` and caused every build to recompile roughly 1,800 objects.

The first incremental build after changing component dependencies may rerun
CMake, but source-only changes should compile only affected objects.

### Host tests

Run:

```sh
/bin/zsh ./scripts/test-host.sh
```

Current suites:

1. `gate_controller_tests`
2. `app_config_tests`
3. `sensor_debouncer_tests`
4. `relay_pulse_guard_tests`

All four passed after the uncommitted serialized runtime implementation.

### UI validation

Run:

```sh
/bin/zsh ./scripts/build-ui.sh
```

The authoritative `svelte-check` result was zero errors and zero warnings.
VS Code may display false diagnostics claiming no Svelte configuration exists;
trust the command-line checker unless it actually fails.

### Firmware result

- Full ESP-IDF build passed after the native-GPIO conversion.
- Last observed application image size was `0x1757a0` bytes.
- 20% of the smallest application partition remained free. The increase is from
  linking the live HomeSpan HAP/SRP/crypto server that was previously dead code.

## Important bugs fixed in the latest milestone

1. Arduino `digitalWrite()` was called before Arduino GPIO initialization and
   logged `IO 5 is not set as GPIO`. The hardware component now uses native
   ESP-IDF `gpio_set_level`, `gpio_set_direction`, `gpio_set_pull_mode`, and
   `gpio_get_level` APIs.
2. Login PBKDF2 ran synchronously without yielding and triggered the task
   watchdog in the HTTP server task. The compatible chunk-yielding PBKDF2
   implementation fixed this.
3. Sensor status initially required manual page refresh. The Status tab now
   polls a minimal read-only runtime endpoint every second.
4. Runtime polling initially used the full config endpoint and would have kept
   idle sessions alive indefinitely. The dedicated endpoint validates without
   refreshing session activity.
5. The firmware build script forced a full rebuild and reran npm every time. It
   now preserves incremental caches and skips current UI assets.
6. A password-change success path accessed `event.currentTarget` after `await`,
   causing a browser error after the password had already changed. The form
   reference is captured before awaiting.

## Current limitations

- Bench validation confirms endpoint commands, feedback stability filtering, and
  Apple Home control mostly work. A remaining defect is that an opposite target
  command issued mid-travel pulses the operator but Apple Home does not reliably
  publish CurrentDoorState=STOPPED for the resulting pause. Preserve this as the
  next controller/HomeSpan synchronization bug; do not infer that the physical
  pause failed solely from the stale Home status.

- Bench relay pulses are available from the authenticated Gate tab. Normal gate
  target commands are not connected yet.
- Relay pulse behavior has not yet been physically checked on the current bench
  hardware.
- Live HomeSpan startup and characteristic mapping compile but still require
  physical boot, discovery, pairing, and command validation on the ESP32.
- Apple Home pairing QR and pairing reset/removal controls are not implemented;
  numeric-code pairing is available.
- Event logging is not implemented beyond serial diagnostics.
- Static IP, custom DNS, and hostname configuration are not implemented.
- The fallback setup AP remains active continuously.
- Local management uses HTTP; there is no HTTPS layer.
- BOOT-button recovery/factory-reset behavior is not implemented.

## Exact next milestone

Flash and validate live HomeSpan discovery, numeric-code pairing, state updates,
and relay commands on the bench.

Recommended sequence:

1. Flash the current firmware and capture boot logs through HomeSpan readiness.
2. Confirm both the station connection and `GateSetup-XXXXXX` fallback AP remain
   active after HomeSpan starts.
3. Sign in and open **Access**; verify the Apple Home card reports Ready and
   displays the formatted pairing code.
4. On iPhone/iPad, use Home → Add Accessory → More Options, select the configured
   garage opener, and enter the displayed numeric code.
5. Confirm the Access card changes to Paired.
6. Exercise Open and Close from Apple Home and confirm exactly one relay click
   for each accepted target-changing command.
7. Move the closed-sensor jumper and confirm current state changes in both the
   dashboard and Apple Home.
8. Verify close timeout publishes STOPPED plus obstruction when the closed sensor
   does not become active.
9. After numeric pairing works, add QR rendering and pairing reset/removal UI.

## Resume prompt

For the next conversation, use:

> Read `docs/development-status.md` and continue from the exact next milestone.
> Read `docs/development-status.md` and continue from the exact next milestone.
> Flash the live HomeSpan build, verify AP+STA coexistence, pair using the Access
> tab numeric code, and bench-test Apple Home commands plus sensor state. Record
> the boot/pairing results before adding QR and pairing reset controls.
