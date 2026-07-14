# Current Wi-Fi provisioning workflow

This is the first complete provisioning portal. It collects station Wi-Fi,
administrator, Apple Home identity, GPIO, polarity, and timing settings. The
firmware validates the complete configuration before saving it.

## Build, flash, and monitor

Use ESP-IDF v5.5.4 and replace `DEVICE` with the serial device selected by the
user:

```sh
./scripts/build-firmware.sh
source /Users/dlbogdan/.espressif/tools/activate_idf_v5.5.4.sh
idf.py -p DEVICE flash monitor
```

The serial log prints output similar to:

```text
Setup AP SSID: GateSetup-A1B2C3
Setup AP password: generated-per-device-value
Setup URL: http://192.168.4.1/
```

The AP password is generated from the ESP32 hardware random source on first
boot and retained in NVS across ordinary firmware restarts.

## Configure the device

1. Join the printed `GateSetup-XXXXXX` network from a phone or computer.
2. Open the operating system's captive portal prompt. If no prompt appears,
   browse to `http://192.168.4.1/`.
3. Enter an administrator password of at least 10 characters. The firmware
   stores a PBKDF2-HMAC-SHA-256 verifier with a random salt, not the password.
4. Enter the station Wi-Fi SSID and password.
5. Enter the Apple Home display name, nontrivial eight-digit setup code, and
   four-character uppercase Setup ID.
6. Select safe, distinct relay and closed-sensor GPIOs and their active levels.
7. Configure relay pulse and gate travel timing.
8. Select **Validate, save, and restart**.
9. The ESP32 saves the complete validated application configuration and its
   bootstrap recovery credentials, restarts, and attempts to connect while
   keeping the setup AP available.
10. The serial log reports `Station connected; setup AP remains active` after a
    successful connection.

Open networks are supported by leaving the password empty. Secured networks
require an 8-63 character password. SSIDs are limited to 32 bytes.

## Authenticated management

After initial setup, open the portal and sign in with the administrator
password. The dashboard is organized into five tabs:

- **Status** shows connectivity, safe controller state, and recovery access.
- **Access** changes the administrator password. The current password is
  verified again, a fresh random salt and PBKDF2 verifier are persisted, and the
  active session is invalidated. Sign in again with the new password.
- **Network** migrates the controller to another Wi-Fi network. The
  administrator password is required again. Both application and bootstrap
  credential stores are updated before restart; the saved Wi-Fi password is
  never returned to the browser.
- **Gate** edits validated relay, feedback sensor, pulse, polarity, and travel
  timing settings without changing secrets.
- **Logs** identifies the location reserved for a future redacted event log.

If a newly configured Wi-Fi network cannot be reached, join the persistent
`GateSetup-XXXXXX` fallback AP and open `http://192.168.4.1/`. The fallback AP
credentials are not changed during station Wi-Fi migration.

## Current limitations

- The captive DNS responder and setup AP remain active after station connection
  in this development milestone. A later authenticated management flow will
  stop the fallback AP after a stable connection.
- The Svelte portal does not yet scan nearby networks or test credentials before
  restarting.
- Bootstrap recovery credentials are stored separately and are also copied into
  the complete validated application configuration.
- Static IP, custom DNS, hostname, Apple Home pairing QR management, and
  persistent event logs are displayed as planned areas but are not implemented.
- Local portal traffic is HTTP. The WPA2 setup AP protects the first-boot radio
  link, but HTTPS is not implemented.
- No relay or sensor GPIO is initialized. Flashing this milestone cannot issue
  a gate command.
