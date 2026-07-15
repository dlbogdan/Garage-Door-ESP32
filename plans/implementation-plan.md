# ESP32 Apple Home Garage Gate Controller — Architecture and Implementation Plan

## 1. Document contract

This document is the implementation source of truth for a new repository. It is deliberately explicit so that a capable local coding model can implement the project without inventing requirements.

Normative terms:

- **MUST**: required for acceptance.
- **MUST NOT**: prohibited.
- **SHOULD**: preferred unless a documented technical constraint prevents it.
- **MAY**: optional.

Project constraints:

- Target: classic ESP32-WROOM-32 DevKit.
- SDK: ESP-IDF **5.5.4 only**.
- Environment activation script: `/Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh`.
- Apple integration: HomeSpan and the standard Apple Home **Garage Door Opener** service.
- Apple HomeKey MUST NOT be implemented, linked, advertised, or exposed in the UI.
- Web UI: Svelte, compiled to embedded static assets. No cloud service is required at runtime.
- Hardware: one configurable momentary dry-contact relay output and one configurable closed-position digital input.
- Operator behavior: sequential/step-by-step single-button gate operator.
- Runtime settings MUST be entered through the web UI and stored in NVS. Wi-Fi credentials, Apple Home pairing code, GPIOs, polarity, pulse duration, travel durations, AP settings, and administrator credentials MUST NOT be hardcoded.
- The firmware MUST never emit an automatic second pulse to force or correct direction. One accepted target-changing command causes at most one relay pulse.

## 2. Reference projects and source acquisition boundary

Use these repositories as implementation references, not as requirements to copy their architecture verbatim:

- `https://github.com/ixy05/homespan-garage`
- `https://github.com/dlbogdan/HomeKey-ESP32`

During implementation, clone references only into `_references/` beneath this repository. Do not inspect any other local directory. Do not copy HomeKey functionality. Inspect the HomeKey reference only for useful ESP-IDF component layout, provisioning, embedded web assets, NVS patterns, and build conventions.

Before coding firmware, record in `docs/reference-notes.md`:

1. exact cloned commit SHAs;
2. reusable architectural patterns found in each reference;
3. code or licenses that constrain reuse;
4. explicit confirmation that no HomeKey service, key material, NFC flow, or related entitlement is imported.

Third-party source and component versions MUST be pinned to immutable versions or commit SHAs. Do not track floating branches.

## 3. Product behavior

### 3.1 First boot

On an unprovisioned device:

1. Derive a unique AP SSID such as `GateSetup-XXXXXX` from the device MAC suffix.
2. Start an open captive-portal AP, DNS catch-all responder, DHCP, and the setup web server. The AP has no product-wide or per-device password because first boot requires no serial-terminal access.
3. Present one compact Svelte form containing only station Wi-Fi SSID/password and the new administrator password.
4. Generate valid nontrivial HomeKit setup identity values and apply conservative defaults for the sequential operator, single feedback input, GPIOs, pulse timing, endpoint stability, and travel timeouts.
5. Validate all values before committing the versioned application configuration and synchronized bootstrap Wi-Fi credentials to NVS.
6. Reboot into normal mode, connect to station Wi-Fi, then start HomeSpan and the authenticated management UI.
7. Configure Apple Home identity, operator profile, feedback topology, hardware GPIOs/polarities, and timing later through the authenticated management UI.

No compile-time secret, universal product password, or serial-disclosed setup secret is permitted.

### 3.2 Normal boot and Wi-Fi fallback

Normal boot order MUST be deterministic:

1. Initialize NVS and migrate validated configuration if necessary.
2. Configure the relay immediately to its inactive safe level without producing a pulse.
3. Configure and debounce the closed sensor.
4. Inspect the BOOT button recovery condition.
5. Attempt station connection using stored credentials.
6. Start HomeSpan only after configuration is valid and network initialization is ready.
7. Start the authenticated management UI.

If station authentication fails, credentials are absent, or connection cannot be established within a bounded timeout, automatically start the configured captive-portal AP. The fallback MUST distinguish authentication failure from transient disconnects in logs, but both may enter fallback after the configured retry policy is exhausted.

Default fallback policy:

- station connection deadline: 45 seconds;
- retry with bounded exponential backoff;
- start AP fallback after the deadline;
- continue low-frequency station reconnect attempts while the AP remains available;
- stop the fallback AP immediately after the station obtains an IP address;
- do not erase credentials merely because authentication failed.

The setup/recovery AP is open at the Wi-Fi layer. On a provisioned device, all management and Wi-Fi migration actions remain protected by administrator login, session, CSRF, and re-authentication rules. Legacy generated AP passwords are cleared during bootstrap-credential load without changing the persisted blob layout.

### 3.3 BOOT button recovery

GPIO0/BOOT is a recovery input, not a normal gate control:

- Hold during boot for 3–9 seconds: enter setup AP mode without erasing settings.
- Hold during boot for at least 10 seconds: arm factory reset, signal the pending destructive action with a serial message and LED pattern if an LED is configured, then require a release followed by a second 3-second hold within 15 seconds to confirm.
- A factory reset from the web UI requires re-entering the administrator password and a second confirmation screen.
- Factory reset erases application configuration, Wi-Fi credentials, HomeSpan pairing data, generated setup data, and sessions, then reboots to first-boot mode.
- A short BOOT press MUST do nothing.

The UI confirmation requirement applies to UI destructive actions. The deliberate two-stage physical sequence is the offline equivalent when the UI cannot be reached.

## 4. Apple Home accessory model

Implement exactly one primary gate accessory using HomeSpan:

- Accessory Information service.
- Garage Door Opener service.
- Required characteristics:
  - Current Door State;
  - Target Door State;
  - Obstruction Detected.
- Use standard HAP garage-door values only.
- Do not model the gate as a lock, switch, door, or HomeKey accessory.

HomeKit value mapping:

| Internal state | Current Door State | Target Door State |
|---|---:|---:|
| `CLOSED` | closed | closed |
| `OPENING` | opening | open |
| `OPEN` | open | open |
| `CLOSING` | closing | closed |
| `STOPPED_OPENING` | stopped | last requested target |
| `STOPPED_CLOSING` | stopped | last requested target |
| `UNKNOWN_STOPPED` | stopped | persisted last target, otherwise open |

Pairing requirements:

- The HomeKit setup code and Setup ID are supplied in the setup UI and stored securely in NVS.
- Validate the setup code format and reject known trivial/invalid HAP codes.
- The authenticated UI displays a scannable QR and the formatted numeric code.
- Pairing state, paired-controller count if HomeSpan exposes it safely, and a dedicated “reset Apple Home pairing” action appear in the UI.
- Resetting Apple pairing MUST NOT erase Wi-Fi, GPIO, timing, or administrator configuration.
- Changing the setup code or Setup ID while paired is blocked until Apple pairing data is reset, or is performed as one clearly confirmed operation that resets pairing.
- HomeSpan identity data must remain stable over ordinary reboot and firmware update.

## 5. Gate state machine

### 5.1 Inputs and outputs

Inputs:

- debounced closed sensor;
- accepted Apple Target Door State write;
- authenticated UI target command;
- authenticated UI maintenance pulse command;
- opening timer expiry;
- closing timer expiry;
- relay pulse completion;
- boot/reboot;
- configuration change.

Outputs:

- one nonblocking relay pulse;
- HomeSpan characteristic updates;
- persisted last stable state, target, and transition metadata;
- event log entries;
- UI status updates.

All events MUST be serialized through one controller task or event loop. HTTP handlers, GPIO callbacks, timers, and HomeSpan callbacks MUST enqueue events rather than mutate gate state directly.

### 5.2 Safety invariants

1. Relay initialization, reboot, crash, OTA, Wi-Fi changes, and HomeSpan restart MUST NOT activate the relay.
2. Relay pulses are nonblocking and use a one-shot timer.
3. A relay pulse cannot begin while another pulse is active.
4. Enforce a default 1.5-second minimum interval between pulse starts; make it configurable within safe UI bounds.
5. A target command that does not change the effective requested target is idempotent and emits no pulse.
6. Commands received during the pulse lockout are rejected as busy and do not queue a delayed physical action.
7. Each accepted target-changing command emits exactly one pulse and never an automatic follow-up pulse.
8. Firmware must never claim direction correction that one pulse cannot guarantee on a sequential operator.
9. Sensor input always has authority to assert `CLOSED`.
10. Reboot with an inactive closed sensor MUST NOT move the gate; report `UNKNOWN_STOPPED`.

### 5.3 Default sequential-operator transitions

The controller tracks the previous direction because a single closed sensor cannot observe stopped or fully open position directly.

| State | Requested target | Physical action | Next inferred state |
|---|---|---|---|
| `CLOSED` | open | one pulse | `OPENING` after sensor releases; fault if it does not release within the start timeout |
| `CLOSED` | closed | none | `CLOSED` |
| `OPEN` | closed | one pulse | `CLOSING` |
| `OPEN` | open | none | `OPEN` |
| `OPENING` | closed | one pulse | `STOPPED_OPENING`; target remains closed |
| `OPENING` | open | none | `OPENING` |
| `CLOSING` | open | one pulse | `STOPPED_CLOSING`; target remains open |
| `CLOSING` | closed | none | `CLOSING` |
| `STOPPED_OPENING` | closed | one pulse | `CLOSING` |
| `STOPPED_OPENING` | open | one pulse | `OPENING`; this reflects the default operator sequence assumption and must be documented in UI help |
| `STOPPED_CLOSING` | open | one pulse | `OPENING` |
| `STOPPED_CLOSING` | closed | one pulse | `CLOSING`; this reflects the default operator sequence assumption and must be documented in UI help |
| `UNKNOWN_STOPPED` | either changed target | one pulse | `UNKNOWN_STOPPED` until sensor/timing evidence permits a safer inference |

Important limitation: different gate operators have different step sequences. The UI MUST show a warning that the controller never emits corrective multi-pulses and that commands issued while moving normally stop the operator first. The user may need to issue a later second command intentionally. Do not conceal this behavior from Apple Home state reporting.

### 5.4 Sensor and timer rules

- Debounce both edges using a timer-based stable-level check; do not debounce with a blocking delay.
- Active sensor at boot means `CLOSED`.
- Inactive sensor at boot means `UNKNOWN_STOPPED`; restore the persisted target for display but do not restore motion.
- Sensor becoming inactive from `CLOSED` without a local relay event indicates an external/manual opening. Enter `OPENING` and start the opening timer.
- Sensor becoming active from any state immediately cancels travel timers, enters `CLOSED`, clears closing obstruction, and updates HomeSpan.
- Opening travel timer completion with sensor inactive enters inferred `OPEN`.
- Closing travel timer completion with sensor inactive enters `STOPPED_CLOSING`, sets obstruction detected, and records a close timeout.
- If the sensor remains active after an open pulse beyond a configurable start timeout, enter `CLOSED`, flag a start fault/obstruction, and do not pulse again.
- Clear obstruction after a successful closed-sensor assertion or an explicit authenticated acknowledgement when safe.
- External closing cannot be observed until the closed sensor asserts. This limitation MUST appear in UI help and project documentation.
- Persist only stable/diagnostic metadata. Never resume a persisted movement timer after reboot.

### 5.5 Maintenance pulse

The authenticated UI MAY provide a deliberately separated “single maintenance pulse” control:

- require administrator re-authentication or a short-lived elevated session;
- display the current sensor and inferred state;
- require explicit confirmation;
- emit exactly one pulse subject to the same lockout;
- pass the pulse through the state-machine event path and log it;
- never offer repeated pulse, press-and-hold, or automatic sequence controls.

## 6. Firmware architecture

Recommended repository layout:

```text
/
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  idf_component.yml
  components/
    gate_controller/
    app_config/
    board_io/
    provisioning/
    web_server/
    homespan_bridge/
    event_log/
    embedded_ui/
  main/
    CMakeLists.txt
    app_main.cpp
  webui/
    package.json
    package-lock.json
    svelte.config.js
    vite.config.js
    src/
  scripts/
    build-ui.sh
    build-firmware.sh
    verify-environment.sh
  docs/
    reference-notes.md
    hardware.md
    provisioning.md
    state-machine.md
    testing.md
  test/
    host/
    target/
```

Component responsibilities:

- `app_config`: versioned typed configuration, validation, defaults, NVS repository, migration, secret redaction.
- `board_io`: safe relay driver, pulse timer, closed-sensor debounce, BOOT recovery gesture, optional status LED.
- `gate_controller`: pure deterministic state reducer plus side-effect adapter; sole owner of gate state.
- `homespan_bridge`: Arduino/HomeSpan initialization, sole Wi-Fi/netif ownership,
  HomeSpan accessory construction, characteristic callbacks, pairing metadata,
  and event translation.
- `provisioning`: fallback-AP policy expressed through Arduino's public `WiFi`
  API, captive DNS, setup-mode state, and observation of station events. It MUST
  NOT initiate station connections, initialize `esp_netif`, create default
  netifs, or initialize/start the ESP-IDF Wi-Fi driver directly. HomeSpan alone
  owns station `WiFi.begin()` and reconnect behavior.
- `management_server`: management HTTP server lifecycle, embedded assets,
  setup/management route registration, common HTTP responses, and
  captive-portal redirects. Route families migrate into narrow API components.
- `web_auth`: opaque sessions, cookie parsing, expiration, CSRF checks, and
  administrator re-authorization shared by management API components.
- `ota_api`: authenticated firmware status/upload HTTP adapter; it owns no
  flash state and calls only the public `ota_manager` interface.
- `ota_manager`: A/B flash state machine, image policy, boot selection, and
  delayed rollback confirmation; it owns no HTTP/session concerns.
- `embedded_ui`: generated Svelte static asset manifest and compressed binary assets.
- `event_log`: bounded in-memory diagnostic ring buffer with redaction; optional compact persisted fault counters.

Use C++ where needed by HomeSpan and expose narrow C-compatible boundaries only where useful. The gate reducer SHOULD be platform-neutral C++ with no direct ESP-IDF, HomeSpan, GPIO, HTTP, or NVS calls so it can be host tested.

The management HTTP server owns TCP port 80. HomeSpan HAP owns dedicated TCP
port 1201 and advertises that port through `_hap._tcp` mDNS. These servers MUST
never be configured to bind the same port; otherwise Apple pair-setup requests
will be routed to the management HTTP server instead of HomeSpan.

The supported operator is a single-input step-by-step controller: endpoint pulse
starts movement, movement pulse pauses, and the next pulse reverses the previous
direction. Each accepted Apple command emits at most one pulse. Opposite target
while moving pauses only; a second explicit Apple command performs reversal.
Feedback electrical polarity and ACTIVE endpoint meaning are independent settings.
Feedback must remain unchanged beyond the configured movement-blink filter before
it proves OPEN or CLOSED. Travel durations are obstruction timeouts and MUST NOT
be used to infer successful endpoint arrival.

## 6.1 Management-plane cleanup plan

The current `components/provisioning/provisioning.cpp` is a temporary god
component. It combines bootstrap storage, fallback networking, captive DNS,
embedded asset serving, authentication, first-time setup, and the permanent
management REST API. Refactor it without changing routes or externally visible
behavior in this order:

1. ✅ Extracted `bootstrap_credentials` with the `gate_boot` NVS format and
   station credential synchronization. The persisted blob layout remains
   compatible; obsolete generated AP passwords are cleared on load.
2. ✅ Extracted `network_manager` as the sole application policy layer for Arduino
   AP+STA mode, fallback `softAP`, and observation of station events. HomeSpan
   remains the sole owner of station connect/reconnect.
3. ✅ Extracted `captive_dns` into a task-owning component with explicit start/stop
   lifecycle and socket cleanup.
4. ✅ Extracted `web_auth` with opaque sessions, cookie parsing, expiration,
   constant-time token checks, CSRF enforcement, login/logout, and password
   rotation. Route handlers consume an authentication interface rather than
   global session state.
5. ✅ `management_server` now owns the ESP-IDF HTTP server, embedded assets, common
   response/security headers, body parsing, and route registration.
6. ✅ Extracted firmware routes into `ota_api`, unprovisioned status/save/staged
   Wi-Fi routes into `setup_api`, and authenticated configuration, runtime,
   HomeKit, relay-test, access, Wi-Fi migration, logout, and reboot routes into
   `management_api`. Gate logic remains behind public runtime, repository,
   hardware-status, and HomeSpan-bridge APIs.
7. ✅ Reduced `gate::provisioning::start()` to a network/setup coordinator. Rename
   the top-level subsystem to `management` in a later isolated commit
   after all callers and documentation have migrated.

Each extraction MUST preserve the existing REST paths, Svelte payloads, NVS
compatibility, fallback recovery, HomeSpan port separation, authentication/CSRF
semantics, and relay safety behavior. Run host tests, UI checks, firmware build,
and a boot/pairing smoke test after every extraction; do not combine this
structural refactor with gate-state behavioral fixes.

## 7. ESP-IDF and dependency policy

Every firmware command MUST run in a shell where this exact script has been sourced first:

```sh
source /Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh
idf.py --version
```

Build scripts MUST:

1. fail unless `IDF_VERSION` resolves to exactly `v5.5.4`;
2. set target to `esp32`;
3. avoid downloading or selecting a different ESP-IDF;
4. use a reproducible dependency lock;
5. build with warnings treated seriously and no unresolved version override.

HomeSpan is normally consumed in an Arduino environment. Integrate Arduino-ESP32 as an ESP-IDF component and run HomeSpan on top of it. Arduino/HomeSpan is the sole owner of Wi-Fi initialization, default netif creation, and Arduino network-event translation. Application provisioning retains policy ownership (credentials, AP+STA mode, fallback availability, and reconnect behavior) but applies that policy only through Arduino's public `WiFi` API. Native ESP-IDF HTTP, DNS, and socket services run on the Arduino-created interfaces. Before feature implementation, create a compatibility spike that proves all of the following under ESP-IDF 5.5.4 on target hardware:

- Arduino component initializes without replacing the required application lifecycle unexpectedly;
- HomeSpan starts and advertises a minimal Garage Door Opener accessory;
- Wi-Fi policy can be managed by the application through Arduino APIs without a
  second ESP-IDF Wi-Fi/netif owner, HomeSpan overwriting credentials, or
  HomeSpan starting its own conflicting provisioning flow;
- HomeSpan pairing code and Setup ID can be supplied from validated NVS before HomeSpan begins;
- HomeSpan pairing storage can be reset independently;
- the web server and HomeSpan coexist without port/task/watchdog conflicts.

If the newest HomeSpan or Arduino-ESP32 release is incompatible with ESP-IDF 5.5.4, select and pin the newest compatible commits. Do not change ESP-IDF version. Do not modify the pinned HomeSpan submodule without explicit approval. Document any approved compatibility patch under `patches/`, its upstream source, and why it is necessary.

Do not use HomeSpan’s serial CLI or built-in Wi-Fi provisioning as the product’s primary configuration mechanism. Serial output is diagnostics plus first-boot AP bootstrap information only.

## 8. Configuration schema

Store one namespaced, versioned configuration model. Separate secrets where practical, but commit related wizard settings atomically.

Required fields and initial defaults:

| Group | Field | Default / rule |
|---|---|---|
| schema | version | explicit integer, migrated on boot |
| device | display name | `Gate` |
| Wi-Fi | SSID/password | no default; UI required |
| fallback AP | SSID suffix | MAC-derived |
| fallback AP | password | generated random, UI editable; WPA2-valid length |
| fallback AP | connection deadline | 45 s |
| admin | password verifier/salt | UI required; never store plaintext |
| HomeKit | setup code | UI required; valid nontrivial eight-digit HAP code |
| HomeKit | Setup ID | UI required; four allowed characters |
| relay | GPIO | UI required; no unsafe default |
| relay | active level | UI required, preview text explains active-low/high |
| relay | pulse duration | 500 ms; valid range 100–2000 ms |
| relay | minimum pulse interval | 1500 ms; valid range 500–10000 ms |
| sensor | GPIO | UI required; no unsafe default |
| sensor | active level | UI required |
| sensor | pull mode | pull-up by default |
| sensor | debounce | 50 ms; valid range 10–500 ms |
| timing | opening travel | 20 s; valid range 3–180 s |
| timing | closing travel | 20 s; valid range 3–180 s |
| timing | sensor release start timeout | 3 s; valid range 1–15 s |

Reject relay/sensor GPIO collisions and flash, PSRAM, input-only-for-relay, boot-strapping, or otherwise unsafe pins. GPIO0 is reserved for BOOT recovery. The UI should recommend safe ESP32 DevKit pins but require explicit user selection rather than silently choosing one.

Configuration updates:

- Validate on both client and firmware; firmware validation is authoritative.
- Return field-specific machine-readable errors.
- Stage, validate, and atomically commit changes.
- GPIO changes first force the old relay inactive, then safely initialize the new pin.
- Wi-Fi changes provide a rollback/recovery path through fallback AP.
- Pairing-sensitive changes follow the reset rule in section 4.
- Never return station password, administrator verifier, session secrets, or raw NVS secrets through the API.

## 9. Svelte web UI

Build a small responsive single-page application with no runtime CDN, external fonts, analytics, or cloud calls. Compile hashed production assets, gzip or Brotli where supported, generate an embedded manifest, and serve from flash with cache headers. The root HTML must remain no-cache so asset upgrades are reliable.

Required routes/views:

1. First-boot setup wizard.
2. Login.
3. Dashboard: current sensor, inferred state, Apple target, obstruction, relay busy state, Wi-Fi status, uptime.
4. Gate settings: GPIOs, polarity, pull, debounce, pulse lockout, opening/closing durations.
5. Network and fallback AP settings, with scan and connection test.
6. Apple Home: pairing status, setup code display, locally rendered QR, reset pairing action.
7. System: firmware/build versions, redacted diagnostics, reboot, setup mode, factory reset.
8. Limitations/help: single-sensor inference and sequential-operator command behavior.

The setup wizard ordering MUST reduce lockout risk:

1. Welcome and hardware warning.
2. Create administrator password.
3. Configure/test Wi-Fi while setup AP remains active.
4. Configure relay and sensor; allow read-only live sensor preview, but do not allow relay actuation in the wizard by default.
5. Configure travel timing.
6. Choose Home name, setup code, and Setup ID.
7. Review redacted settings.
8. Save atomically and reboot.

Accessibility and robustness:

- usable on a phone at 320 CSS pixels;
- keyboard operable and visibly focused;
- semantic labels and status text, not color alone;
- no UI assumption that an API request succeeded before its response;
- prevent duplicate command submission;
- show reconnect behavior during reboot/network transition.

## 10. HTTP API and security

Use a versioned API prefix such as `/api/v1`. Define the exact OpenAPI-like contract in `docs/api.md` before implementation. Minimum resources:

- bootstrap/setup status;
- authenticated session login/logout/status;
- device/gate status;
- validated configuration read/update with secrets redacted;
- Wi-Fi scan/test/apply;
- Apple pairing status and reset;
- target command and separately protected maintenance pulse;
- reboot, enter setup mode, and factory reset;
- redacted event log and build metadata.

Security requirements:

- Store administrator password as PBKDF2-HMAC-SHA-256 or another ESP-IDF-supported password KDF with per-device random salt and a documented iteration count measured to avoid watchdog starvation. Never store plaintext.
- Generate random opaque session tokens; store only token hashes where practical; expire idle and absolute sessions.
- Set `HttpOnly` and `SameSite=Strict` cookies. Set `Secure` when HTTPS is implemented.
- Protect all mutating authenticated requests against CSRF with a session-bound token and origin/host validation.
- Rate-limit login, setup-code display, maintenance pulse, and destructive actions.
- Permit unauthenticated access only to captive detection endpoints and the incomplete first-boot wizard while connected to the password-protected setup AP.
- Once provisioned, setup-mode UI still requires the administrator password.
- Do not expose relay command endpoints merely because the client is on the setup AP.
- Redact Wi-Fi passwords, administrator material, HomeSpan long-term keys, session tokens, and full setup codes from logs.
- Add standard no-sniff, frame-deny, referrer, and restrictive content-security headers.
- Bind DNS captive interception only while the fallback AP is active.

HTTPS with a self-signed per-device certificate MAY be added, but must not block the streamlined initial release. The documentation MUST accurately disclose if local HTTP is used. WPA2 protects first-boot AP traffic; administrator authentication protects the station-side UI.

## 11. Partitioning, persistence, and updates

- Provide a custom partition table with sufficient application space for Arduino, HomeSpan, web server, and embedded UI.
- Reserve NVS for application/HomeSpan data without namespace collision.
- Include `nvs_keys` and NVS encryption only if a production flash-encryption workflow is fully documented; do not pretend plaintext flash is encrypted.
- Prefer an OTA-capable partition layout if binary size permits. If OTA is implemented, use signed images and expose upload only to authenticated administrators with CSRF protection.
- If OTA does not fit safely, omit the UI control and document serial flashing. Do not ship a nonfunctional placeholder.
- Keep configuration schema migrations forward-only and test upgrades from every released schema.

The investigated OTA architecture, security boundary, rollback policy,
implementation sequence, and acceptance tests are specified in
`docs/ota-update.md`. Native ESP-IDF A/B web OTA is the selected product path;
HomeSpan/ArduinoOTA is not the management transport.

## 12. Observability and failure handling

Log structured, redacted events for:

- boot reason and firmware version;
- configuration migration/validation result;
- Wi-Fi state and reason class without credentials;
- setup AP start/stop;
- sensor stable edges;
- accepted/rejected gate commands and source;
- relay pulse start/end;
- inferred state transitions;
- travel timeout/obstruction;
- HomeSpan start and pairing-state changes;
- administrator login success/failure rate-limit events;
- reset/reboot actions.

Expose a bounded recent log in the authenticated UI. Do not continuously write ordinary state transitions to flash. Persist compact counters and last-fault metadata only when useful and wear-safe.

On unexpected errors, fail safe:

- relay inactive;
- no automatic pulse replay;
- gate state stopped/unknown unless closed sensor proves closed;
- fallback AP available when station access is unavailable;
- no configuration erasure without explicit reset confirmation.

## 13. Build and developer workflow

Create scripts that make the correct environment difficult to misuse:

- `scripts/verify-environment.sh`: verify macOS-compatible shell behavior, exact ESP-IDF 5.5.4, `esp32` target, required Node/npm version, and pinned component state.
- `scripts/build-ui.sh`: perform reproducible npm install from lockfile, lint/typecheck/test, production build, compression, and asset embedding.
- `scripts/build-firmware.sh`: source the required activation script, assert version, build UI if needed, and run `idf.py build`.

Document commands for:

```sh
source /Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh
idf.py set-target esp32
idf.py build
idf.py -p DEVICE flash monitor
```

Do not assume a serial device path. Do not access tools, repositories, or files outside the current project except the explicitly supplied ESP-IDF activation script and toolchain paths it configures.

## 14. Testing strategy

### 14.1 Host unit tests

Test the pure gate reducer exhaustively:

- every state/target pair in the transition table;
- idempotent repeated target writes;
- one-pulse maximum invariant;
- busy/lockout rejection;
- sensor edge precedence;
- opening completion and closing timeout;
- reboot recovery with sensor active/inactive;
- external/manual opening;
- obstruction set/clear;
- no delayed pulse after rejected command.

Also unit test configuration validation/migration, secret redaction, setup-code validation, API authorization decisions, and captive fallback policy.

### 14.2 Firmware integration tests

Use mocked drivers where possible and target hardware where required:

- relay never glitches at boot, reset, flash, Wi-Fi reconnect, web restart, or HomeSpan restart;
- pulse width and minimum interval are within configured tolerance;
- sensor debounce rejects bounce on both edges;
- station authentication failure starts the open fallback AP after the bounded connection deadline;
- fallback AP remains usable while station retries occur;
- provisioned fallback access requires administrator login before Wi-Fi migration;
- configuration commit survives power loss without a partial schema;
- BOOT recovery gestures match section 3.3;
- HomeSpan pairing reset is independent from application settings;
- web assets fit partition and load without external network access.

### 14.3 Apple Home acceptance tests

On a real iPhone/Home hub where available:

- scan the UI-generated QR and pair successfully;
- accessory appears specifically as a Garage Door Opener;
- open from `CLOSED` emits one pulse and reports opening/open by sensor/timer evidence;
- close from inferred `OPEN` emits one pulse and reports closing/closed;
- opposite target during motion emits one pulse, reports stopped, and emits no corrective pulse;
- closing timeout reports stopped plus obstruction;
- reboot preserves pairing and does not move the gate;
- pairing reset removes only Apple pairing state and permits re-pairing with configured setup data.

### 14.4 Security tests

- unauthenticated station clients cannot read configuration secrets or actuate/reset the device;
- CSRF attempts fail;
- brute-force rate limits engage;
- cookies and security headers are correct;
- API responses and logs redact secrets;
- first-boot routes close after provisioning;
- setup fallback after provisioning requires admin login;
- malformed configuration cannot select unsafe/colliding pins;
- destructive actions require the specified confirmations.

## 15. Implementation sequence

Execute in this order. Do not start broad feature work until the compatibility gate passes.

1. Initialize repository metadata, license decision, ignore rules, ESP-IDF 5.5.4 project skeleton, target assertion, partition draft, and build scripts. ✅
2. Clone and inspect the two references under `_references/`; write reference and license notes. ✅
3. Pin Arduino-ESP32 and HomeSpan versions; implement the compatibility spike; verify Garage Door Opener service compiles with `Service::GarageDoorOpener` (ID 41). ✅
4. Define typed configuration schema, validators, NVS namespaces, migrations, secret handling, and tests. ✅
5. Implement safe relay, debounced sensor, BOOT gesture using Arduino-style GPIO APIs (`pinMode`, `digitalWrite`, `digitalRead`), hardware tests. ✅
6. Implement the pure gate state reducer, transition table with exhaustive host tests (21 cases). ✅
7. Implement Wi-Fi station lifecycle, open bootstrap/recovery AP, captive DNS/portal, authentication-failure fallback, and reconnect behavior. ✅
8. Implement HomeSpan bridge, NVS-driven setup code/Setup ID, characteristic mapping, stable identity, and isolated pairing reset. 🟡 Core bridge and pairing work; isolated pairing reset/QR completion remains.
9. Specify and implement authenticated REST API, sessions, CSRF, rate limits, redaction, and event log. 🟡 Sessions and CSRF exist; rate limits, event log, and §6.1 extraction remain.
10. Scaffold Svelte UI; implement compact first-boot setup, authentication, dashboard, settings, operator profiles, Apple pairing help, and destructive confirmations. 🟡 Core UI exists; QR and remaining destructive flows remain.
11. Build/embed compressed UI assets and enforce flash partition budgets. ✅
12. Integrate all subsystems and verify boot sequencing and no-relay-glitch behavior. 🟡 Host/build/bench checks pass; real-gate validation remains.
13. Run host, firmware, provisioning, Apple Home, recovery, power-loss, and security acceptance suites. 🟡 Automated and bench checks pass; full real-gate, Apple STOPPED rendering, power-loss, and security suites remain.
14. Complete operator documentation, wiring diagram, known limitations, flashing instructions, and release checklist. ⏳

## 16. Definition of done

The project is complete only when all statements below are true:

- A clean build succeeds only with ESP-IDF 5.5.4 after sourcing the specified activation script.
- Dependencies are pinned and license obligations documented.
- A user can provision an erased ESP32 entirely through an open captive setup AP without a serial terminal, entering only Wi-Fi and administrator credentials on first boot.
- All required operational values are UI-configurable and persisted; none are hardcoded.
- Failed station connection automatically exposes the open captive fallback AP without erasing settings, and provisioned recovery requires administrator login before Wi-Fi changes.
- The device pairs through an UI-generated QR as an Apple Garage Door Opener using HomeSpan.
- No HomeKey implementation or artifact is present.
- One accepted target-changing command produces no more than one physical relay pulse; no automatic correction pulse exists anywhere in code.
- The single closed sensor and travel timers drive the documented conservative state inference.
- Reboot and failures never actuate the relay or resume motion.
- Pairing, configuration, recovery, authentication, CSRF, redaction, and destructive-action tests pass.
- The Svelte UI is self-contained, responsive, accessible, and requires no internet connection.
- Documentation clearly warns about one-sensor uncertainty and step-by-step operator behavior.

## 17. Explicit non-goals

- Apple HomeKey, NFC keys, Wallet passes, lock services, or reader hardware.
- Cloud account, cloud relay, telemetry service, or remote vendor API.
- Camera, intercom, vehicle detection, geofencing, or automatic opening.
- Automatic multi-pulse direction correction.
- Claiming exact physical position from one closed sensor.
- Supporting ESP32-S2/S3/C3 or non-ESP32 targets in the initial project.
- Hardcoded Wi-Fi, pairing, GPIO, timing, AP, or administrator settings.
