# Full backup and disaster recovery

## Scope

A full backup is an encrypted byte-for-byte snapshot of the complete `nvs`
partition. It includes application configuration, Wi-Fi credentials,
administrator verifier state, HomeSpan accessory identity, HomeKit pairings, and
any other namespace stored in NVS. It is intended for replacement-board disaster
recovery, not for sharing a gate configuration.

The file does not contain the application image, bootloader, partition table,
OTA metadata, core dump, or restore-staging partition. Flash a compatible
firmware build on the destination board before restoring.

## Encryption and password requirements

Export requires an authenticated dashboard session, CSRF token, and the current
administrator password. The raw NVS image is encrypted with AES-256-GCM using a
key derived from that password with PBKDF2-HMAC-SHA-256, a random salt, and
120,000 iterations. Envelope metadata is authenticated as associated data.

Restore requires the administrator password that protected the backup. On a
replacement board this is the **old backup password**, not a password belonging
to the replacement board. There is no recovery mechanism if both the password
and a working source device are lost.

Treat `.gdbak` files as sensitive credentials even though they are encrypted.
Use offline storage with appropriate access controls and retain more than one
known-good generation.

## Export from a provisioned device

1. Open the management dashboard and select **Backup**.
2. Enter the current administrator password.
3. Download and securely retain `garage-door-full.gdbak`.
4. Do not interrupt power while the snapshot is being created.

The firmware temporarily deinitializes NVS while reading the raw partition and
reinitializes it before encrypting the image. Do not perform other configuration,
pairing, or network-management operations concurrently with export.

## Restore on a fresh replacement board

1. Flash a firmware build with the same NVS size and compatible partition
   layout/project identity.
2. Join the setup access point and open the initial setup page.
3. Select **Restore full backup** instead of **Configure as new**.
4. Choose the `.gdbak` file and enter its old administrator password.
5. Confirm the destructive restore.

No new Wi-Fi or administrator settings are required. After validation, the
decrypted image is written and verified in `backup_stage`, then the board
restarts. Before normal NVS initialization, early boot applies and verifies the
staged image. The board then reconnects using the restored Wi-Fi settings, so
the setup access point may disappear and its station address may differ.

Fresh-board restore accepts one upload at a time and applies progressive delays
after failures. Wrong passwords, corrupted files, and incompatible files use
generic errors so the endpoint does not disclose which check failed.

## Restore from the dashboard

The **Backup** dashboard tab also supports restore on a provisioned board. It
requires a valid session and CSRF token, the uploaded backup, its old password,
and destructive-action confirmation. The old password is not required to match
the administrator password of the currently running board.

Restore replaces the entire current NVS partition, including the destination
board's Wi-Fi, administrator state, HomeKit identity, pairings, and all current
application settings.

## Failure and power-loss behavior

The staging partition uses redundant manifests, a payload SHA-256 digest, and
`ready`, `applying`, and `failed` states. A manifest is marked applying and its
attempt counter incremented before NVS is erased. Early boot verifies both the
staged payload and NVS readback. Successful application erases staging. Repeated
failure is capped to prevent an infinite restore boot loop.

Avoid power interruption during upload, staging, restart, and early application.
The state machine is designed to fail safely, but physical recovery drills are
required before relying on it for a real installation.

## HomeKit identity warning

A full restore clones the original HomeKit accessory identity and pairings. Do
not run the source and restored boards simultaneously on the same network or in
the same Apple Home. Retire, erase, or isolate the old board before bringing the
replacement online. A Gate Profile does not clone HomeKit identity.

## Compatibility and current limitations

- Project identity, NVS offset/size, and the staging-layout fingerprint must
  match the backup envelope.
- Downgrade/migration compatibility is limited to what the flashed firmware can
  load from the restored application configuration.
- The management service is HTTP-only; use it only on a trusted local network.
- Automated crypto/staging fault-injection coverage and physical replacement-
  board/power-interruption drills are still required for production confidence.
