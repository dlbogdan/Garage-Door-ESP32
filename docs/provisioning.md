# Current Wi-Fi provisioning workflow

This is an early, deliberately limited provisioning portal. It configures
station Wi-Fi only. HomeKit, administrator, GPIO, sensor, and timing settings
will be added to the full setup wizard later.

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

## Configure Wi-Fi

1. Join the printed `GateSetup-XXXXXX` network from a phone or computer.
2. Open the operating system's captive portal prompt. If no prompt appears,
   browse to `http://192.168.4.1/`.
3. Enter the station Wi-Fi SSID and password.
4. Select **Save and restart**.
5. The ESP32 saves the credentials to its bootstrap NVS namespace, restarts,
   and attempts to connect while keeping the setup AP available.
6. The serial log reports `Station connected; setup AP remains active` after a
   successful connection.

Open networks are supported by leaving the password empty. Secured networks
require an 8-63 character password. SSIDs are limited to 32 bytes.

## Current limitations

- The captive DNS responder and setup AP remain active after station connection
  in this development milestone. A later authenticated management flow will
  stop the fallback AP after a stable connection.
- The current HTML portal configures only station Wi-Fi. It does not yet scan
  nearby networks or test credentials before restarting.
- Bootstrap credentials are stored separately from the complete validated
  application configuration. This avoids inserting fake HomeKit, administrator,
  or GPIO values merely to save Wi-Fi.
- Local portal traffic is HTTP. The WPA2 setup AP protects the first-boot radio
  link, but HTTPS is not implemented.
- No relay or sensor GPIO is initialized. Flashing this milestone cannot issue
  a gate command.
