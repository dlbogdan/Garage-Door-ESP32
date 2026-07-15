# OTA update investigation and implementation design

## Implementation status

The firmware now implements the functional A/B OTA path described here:

- `ota_manager` streams into the inactive app slot with ESP-IDF OTA APIs;
- application rollback is enabled and Arduino's premature confirmation hook is
  overridden;
- pending images receive a 15-second local-health soak before confirmation;
- the gate runtime enters command-interlocked maintenance mode and requires no
  movement or active pulse;
- authenticated firmware status and upload routes require CSRF plus
  administrator-password re-authorization;
- uploaded images must fit the slot, pass ESP-IDF image validation, identify as
  project `garage_door_esp32`, and differ from the running version;
- the embedded Svelte management UI provides firmware selection, progress,
  partition/version status, and automatic restart;
- firmware versions are sourced from the ESP-IDF application descriptor and
  are also reported to HomeKit. The normal build script derives a deterministic
  development version from the base semantic version, Git revision, and current
  firmware/UI source changes (for example `0.3.2-dev-a1b2c3d4-e5f6a7b8`).

Development fingerprints are derived by CMake, so the ESP-IDF extension's
**Build Project** button and `scripts/build-firmware.sh` use the same versioning.
Identical source produces the same version on repeated builds, while changed
firmware/UI source produces a different version that passes the OTA same-version
guard. CMake watches those inputs and reconfigures before an incremental build
when one changes. The management-server CMake component directly runs the npm
install/check/test/build pipeline and copies stale generated assets before
embedding them. The extension button therefore packages the current Svelte
output without invoking a project shell script.

Release builds provide an explicit semantic version without modifying tracked
files. They use a separate build directory so a release override cannot remain
cached and accidentally affect later GUI development builds:

```sh
FIRMWARE_VERSION=0.3.3 ./scripts/build-firmware.sh
```

The release OTA artifact is
`build-release/0.3.3/garage_door_esp32.bin`; development artifacts remain at
`build/garage_door_esp32.bin`.

The OTA writer uses `OTA_WITH_SEQUENTIAL_WRITES`. This is important on the
dual-core classic ESP32: erasing the complete ~1.5 MiB destination range inside
`esp_ota_begin()` can keep flash-cache coordination in the IPC task long enough
to starve the CPU0 idle task and trigger the five-second task watchdog. With
sequential mode, sectors are erased incrementally as upload chunks are written.
The HTTP receive buffer is heap-allocated rather than consuming 4 KiB of the
HTTP server task's stack, preventing stack corruption during flash/network
activity.

The HTTP transport is implemented by the dedicated `ota_api` component. It
owns firmware status/upload route validation, streams request bodies into the
narrow `ota_manager` interface, and delegates authentication to `web_auth`.
Neither OTA flash logic nor OTA HTTP handling is part of `provisioning`.

The production signing/eFuse stage remains deliberately disabled. ESP-IDF
validates image structure and checksum, but this development build does not yet
cryptographically authenticate the publisher. Do not treat the current build
as production-secure OTA until an offline signing key, public verification key,
release-signing pipeline, key backup/revocation procedure, and (if selected)
Secure Boot provisioning procedure are established and tested. No private key
has been generated or committed by this implementation.

## Decision

Implement firmware-only OTA through the authenticated management web UI, using
ESP-IDF's native `esp_ota_*` API and the existing `ota_0`/`ota_1` partition
layout. Do not use HomeSpan/ArduinoOTA as the product OTA transport.

HomeSpan's OTA implementation is useful as a rollback reference, but its
Arduino IDE/`espota` transport creates a second management endpoint on port
3232 with a separate password and no integration with the product's existing
administrator session, CSRF checks, UI, or audit policy. The product already
has an authenticated HTTP management plane, so adding one protected streaming
upload route is a smaller and more coherent attack surface.

The first implementation must update only the application image. The current
web assets are embedded in the application binary, so a separate SPIFFS image
upload is unnecessary and would make firmware/filesystem versions non-atomic.
The `storage` partition must not be overwritten by OTA.

## Current readiness

The repository is already physically partitioned for A/B OTA:

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| `otadata` | `0xF000` | 8 KiB | boot-selection and OTA state |
| `ota_0` | `0x20000` | 1,900,544 bytes | application slot A |
| `ota_1` | `0x1F0000` | 1,900,544 bytes | application slot B |
| `coredump` | `0x3C0000` | 64 KiB | crash diagnostics |
| `storage` | `0x3D0000` | 192 KiB | reserved data partition |

The inspected build artifact is 1,543,952 bytes, leaving 356,592 bytes
(18.8%) in each OTA slot. OTA fits now, but this margin should be enforced by
CI/build scripts because Arduino, HomeSpan, OTA code, and future UI assets can
consume it quickly.

The missing prerequisites are:

- bootloader application rollback is not enabled;
- signed-app verification is not enabled;
- no boot health/confirmation policy exists;
- no OTA manager, authenticated upload route, status API, or UI exists;
- firmware version metadata is currently duplicated as a literal HomeKit
  characteristic instead of being sourced from the application descriptor.

## Reference findings

### HomeSpan

HomeSpan supports password-protected ArduinoOTA, checks that an uploaded image
contains a HomeSpan marker, and records an `OTA_REQUIRED` flag so a replacement
that omits OTA support rolls back. Its `SpanRollback.h` overrides Arduino's
weak `verifyRollbackLater()` function, preventing Arduino initialization from
prematurely marking a pending image valid. It then expects the application to
call `homeSpan.markSketchOK()` after meaningful startup validation.

Useful patterns to retain:

1. enable bootloader rollback and do not validate a new image immediately;
2. reject an image that is not intended for this product;
3. confirm the image only after application initialization has proven healthy;
4. retain a reachable update mechanism after a successful update.

Patterns not to adopt:

- ArduinoOTA/`espota` as the primary product transport;
- the default `homespan-ota` password;
- an independently managed OTA password;
- validation based only on a HomeSpan magic cookie;
- validation based only on reaching the first HomeSpan poll.

### HomeKey-ESP32

The reference uses an asynchronous authenticated HTTP upload, streams 4 KiB
chunks into `esp_ota_write()`, serializes updates with an atomic in-progress
flag, exposes progress and running/next partition information, and enables
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.

These are useful implementation patterns. Its code should not be copied
unchanged because it does not provide the required CSRF/origin policy in the
OTA handler, accepts any image accepted by `esp_ota_end()`, provides no
product/chip/version policy, and does not visibly implement post-boot health
confirmation. Its separate LittleFS update also creates a non-atomic two-file
release, whereas this project embeds its UI.

## Proposed architecture

Add a dedicated `ota_manager` component after the management-plane extraction.
It owns the OTA state machine and ESP-IDF OTA calls; HTTP handlers only perform
request authentication and stream bytes into it.

Suggested public operations:

- `begin(content_length, expected_sha256, metadata)`;
- `write(bytes)`;
- `finish()`;
- `abort()`;
- `status()`;
- `inspect_boot_state()`;
- `confirm_running_image()`.

Only one update may exist at a time. State transitions should be explicit:
`idle -> receiving -> verifying -> ready_to_reboot`, with every failure going
through `failed -> idle` after `esp_ota_abort()` and cleanup.

Add management routes after `web_auth` and `web_server` are separated:

- `GET /api/v1/system/firmware` returns current version, build/project ID,
  running partition, next partition, image state, maximum image bytes, and OTA
  availability;
- `POST /api/v1/system/firmware` streams one application binary;
- optionally, `GET /api/v1/system/firmware/update` reports progress if upload
  progress is not kept solely client-side.

The upload route must require all of the following:

- valid administrator session;
- session-bound CSRF token;
- same-origin/Host validation;
- recent password re-authentication (recommended five-minute elevation);
- `Content-Length` greater than zero and no larger than the next OTA slot;
- `application/octet-stream` content type;
- no other OTA operation in progress.

Do not buffer the image in RAM. Read bounded chunks with a finite receive
timeout, feed the task watchdog while progressing, abort on disconnect or
timeout, and never retry writes automatically. The management server must cap
request-body size independently of the declared content length.

## Image identity and signature policy

There are two distinct controls and both are required:

1. **Cryptographic authenticity**: enable ESP-IDF signed application
   verification. For development, signed apps without Secure Boot can detect
   accidental or unauthorized web uploads while retaining ordinary serial
   recovery. For production, Secure Boot v1 on classic ESP32 is the stronger
   enforcement boundary, but enabling it is an irreversible provisioning
   decision and must have a documented signing-key backup, eFuse procedure,
   serial recovery policy, and release workflow before any device is fused.
2. **Product compatibility**: inspect the incoming image before accepting the
   first chunk as an application image and compare its `esp_app_desc_t` fields
   with an allow-list: project name must match this firmware, chip must be
   ESP32, and the secure version/version policy must be acceptable. A signature
   alone proves the signer, not that the operator selected the correct product
   binary.

The UI may optionally submit a release manifest containing file size and
SHA-256. The device should calculate SHA-256 while streaming and compare it at
finish. This catches truncation and wrong-file mistakes, but the digest is not
a substitute for a signature unless the manifest itself is signed and verified
against a built-in release public key.

Downgrade handling should initially reject the same version and older semantic
versions in the UI/API, with an explicitly documented recovery override only
if needed. Production anti-rollback via secure-version eFuses should not be
enabled until the irreversible versioning and key-management process is
defined.

Never store a private signing key in this repository, firmware, CI artifact, or
device. The firmware contains only the public verification material required by
the selected ESP-IDF signing mode.

## Safe update sequence

1. Authenticate, re-authorize, validate CSRF/origin, and acquire the global OTA
   lock.
2. Reject the operation unless the gate runtime is active, no relay pulse is in
   progress, and the gate is not inferred to be moving. Prefer a stable closed
   sensor assertion; otherwise require an explicit warning/confirmation that
   reboot will not move the gate but temporarily removes monitoring.
3. Put the runtime in OTA maintenance mode. This must reject new HomeKit and UI
   target commands and force/confirm the relay inactive. OTA must never queue a
   command for later execution.
4. Select `esp_ota_get_next_update_partition(nullptr)`, ensure it differs from
   the running partition, and validate declared size against that partition.
5. Begin sequential writing with `esp_ota_begin()` and stream with
   `esp_ota_write()`. Hash concurrently and report bounded progress.
6. Call `esp_ota_end()` to perform image validation, verify product metadata,
   digest, signature policy, and version policy, then call
   `esp_ota_set_boot_partition()` only after every check passes.
7. Send and flush a success response, record a redacted audit event, wait
   briefly, then restart. Do not expose a general `skipReboot` option in the
   product UI.
8. On every failure, call `esp_ota_abort()` when a handle exists, release
   maintenance mode and the OTA lock, leave the running boot partition
   unchanged, and return a non-secret error code.

Power loss during writes affects only the inactive slot. The current slot and
configuration remain intact. `otadata` changes only after the complete image
has passed validation and is selected for boot.

## Rollback and boot confirmation

Enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Also include
`SpanRollback.h` in exactly one application translation unit, or provide an
equivalent strong `verifyRollbackLater()` override, because Arduino otherwise
marks a pending image valid during its own initialization. The latter should
be wrapped in this project's OTA component rather than spread through the
HomeKit bridge.

At boot, inspect the running partition state. If it is not
`ESP_OTA_IMG_PENDING_VERIFY`, normal startup is unchanged. If it is pending,
start a bounded health-confirmation window and mark it valid only after all
local, firmware-dependent checks pass:

- NVS opens and the configuration schema loads or migrates successfully;
- relay GPIO has been initialized and confirmed at its inactive level;
- sensor input/debouncer and serialized gate runtime have started;
- management HTTP server has started;
- HomeSpan has initialized and its polling task has completed at least one
  pass when the configuration is valid;
- the application has remained alive without watchdog reset/panic for a short
  soak period (recommended 10–30 seconds).

Do not require station Wi-Fi or Apple Home connectivity to confirm the image.
Network infrastructure may be unavailable for reasons unrelated to firmware,
and forcing rollback in that case can make recovery less predictable. Starting
the local management server and HomeSpan task is sufficient; successful remote
connections are not.

If a required local subsystem fails while pending, call
`esp_ota_mark_app_invalid_rollback_and_reboot()`. If the firmware crashes or the
watchdog resets before confirmation, the bootloader rolls back automatically.
Never emit a relay pulse as a boot-health test.

After rollback, expose the running partition, reset reason, and a compact
"previous update rolled back" diagnostic in the authenticated UI. Preserve NVS
configuration and HomeSpan pairing data across both update and rollback.

## Configuration and release metadata

Add to `sdkconfig.defaults` after validating the complete workflow:

```text
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

The exact signed-app/Secure Boot settings must be generated and tested using
ESP-IDF 5.5.4's configuration because they affect bootloader compatibility and
release signing. Do not casually add Secure Boot options to defaults before the
key/eFuse workflow exists.

Set the ESP-IDF project version reproducibly (for example from an explicit
release version injected by CMake) so `esp_app_get_description()` is the single
firmware-version source. Use that value for the HomeKit Firmware Revision,
management status API, logs, filenames, and release manifest.

CI must fail when:

- the application binary exceeds an agreed limit below 1,900,544 bytes;
- an OTA release artifact is unsigned;
- project/chip metadata is wrong;
- a manifest digest does not match the binary;
- the factory image and OTA application artifact were not built from the same
  immutable source revision and configuration.

Keep at least 64 KiB of enforced slot headroom initially; more is preferable.
The current build has adequate but not generous margin.

## UI behavior

Add the OTA page only when the backend and rollback path are functional. Show:

- current version and running partition;
- selected file name, version, size, and digest/verification status;
- explicit warning not to interrupt power;
- gate state and whether OTA maintenance admission is allowed;
- upload/verification/reboot progress;
- rollback status after reconnect.

Do not accept filesystem images, factory/merged images, bootloaders, partition
tables, or arbitrary addresses. The accepted artifact should have an
unambiguous release name such as `garage-door-esp32-vX.Y.Z-ota.bin`.

## Implementation order

1. Establish reproducible project version metadata and artifact naming.
2. Enable/test rollback with a deliberately crashing and deliberately hanging
   image before exposing uploads.
3. Add `ota_manager` with host-testable policy helpers and target-only ESP-IDF
   adapter.
4. Add runtime OTA maintenance admission and command rejection without changing
   gate-state semantics.
5. Add authenticated/CSRF-protected streaming route after the planned
   `web_auth`/`web_server` extraction, or carefully isolate it if OTA must land
   earlier.
6. Add boot health confirmation and rollback diagnostics.
7. Define and test signed release generation and verification.
8. Add the Svelte OTA view and release documentation.
9. Only after a separate production-security review, decide whether to provision
   Secure Boot eFuses on deployed hardware.

## Acceptance tests

- valid signed application uploads to the inactive slot, reboots, confirms, and
  preserves configuration, pairing, and gate state safety;
- wrong project, wrong chip, unsigned, corrupted, truncated, oversized, same
  version, and disallowed downgrade images are rejected without changing boot
  selection;
- unauthenticated, stale-session, missing-CSRF, cross-origin, and concurrent
  uploads are rejected;
- client disconnect and receive timeout abort cleanly;
- power loss at several write percentages boots the previous image;
- panic and watchdog hang before confirmation roll back to the previous image;
- a healthy image confirms without requiring Internet, router, or Home hub
  availability;
- rollback preserves NVS configuration and HomeKit pairing;
- OTA admission and reboot never activate the relay and reject all gate commands
  while maintenance mode is active;
- binary-size enforcement leaves reserved headroom in both equal OTA slots;
- serial recovery remains documented and tested for development devices;
- production Secure Boot provisioning, if adopted, is tested on sacrificial
  hardware before deployment.

## Conclusion

OTA is feasible on the current 4 MiB layout without repartitioning. The correct
product design is native ESP-IDF A/B web OTA integrated with the existing
authenticated management plane, signed application verification, delayed boot
confirmation, and hardware-safe maintenance mode. The partition table is ready;
the security, rollback, management-plane, and release-pipeline work must be
completed together before an OTA control is shipped.
