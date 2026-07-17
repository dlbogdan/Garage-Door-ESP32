# Gate Profile format and import

## Purpose and security boundary

A Gate Profile is a portable, versioned JSON document containing the complete
persistent functional Gate configuration for a target controller. Unlike a full
backup, it intentionally excludes Wi-Fi credentials, administrator verifier
state, setup-AP credentials, HomeKit identity and pairings, OTA state, transient
runtime state, and logic-analyzer trace history.

The canonical format has:

```json
{
  "format": "garage-door-gate-profile",
  "version": 1,
  "project": "garage-door-esp32",
  "target": {
    "vendor": "",
    "model": "",
    "name": "Gate Gate Profile",
    "notes": ""
  },
  "compatibility": { "appSchemaVersion": 4 },
  "operator": {},
  "timing": {}
}
```

## Complete configuration content

The `operator` object includes:

- sequential or directional operator strategy;
- STEP, OPEN, and CLOSE output GPIO, active level, and pulse duration;
- minimum command interval;
- single or dual feedback topology and single-input endpoint meaning;
- endpoint stability timing;
- single, opened, and closed feedback GPIO, active level, pull, and debounce;
- endpoint-preset or custom decoder profile;
- every declared decoder input ID, label, and electrical definition;
- every rule ID, label, enabled flag, semantic outcome, lifecycle timing, and
  match-age behavior;
- every OR group and AND predicate, including stable-level and periodic-edge
  timing fields.

The `timing` object includes opening travel time, closing travel time, and sensor
release timeout in milliseconds. Active and inactive strategy/topology fields
are serialized so a round trip preserves the complete persistent model.

## Export

From the **Gate** tab, select **Download profile**. Export requires an
authenticated session but no administrator-password re-entry because the file
contains no device secrets. Firmware—not browser state—produces the canonical
JSON.

Target vendor/model/name/notes are metadata and do not weaken electrical or
semantic validation. Empty metadata remains valid.

## Import transaction

Import is a mandatory two-step server transaction:

1. **Preview** uploads the JSON with session and CSRF protection. Firmware
   performs strict bounded parsing, compatibility checks, whole-configuration
   safety validation, GPIO collision/pull checks, and decoder compilation. It
   normalizes the accepted profile and returns current/candidate data plus a
   SHA-256 digest.
2. **Confirm complete replacement** submits the exact reviewed digest and an
   explicit confirmation value. The server accepts it only from the same active
   session and only while the current Gate configuration still matches the base
   digest captured during preview.

Any mismatch, intervening settings change, new session, invalid confirmation,
or different digest requires a new review. Apply preserves non-Gate settings,
atomically saves the replacement Gate operator/timing configuration, and
restarts so GPIO ownership and compiled decoder state are rebuilt consistently.

## Safety

Review both the concise before/after summary and complete normalized JSON before
confirmation. Import can change physical output GPIOs, active levels, pulse
durations, and gate behavior. Incorrect wiring definitions can energize an
unintended controller input.

Commission imported profiles with actuator contacts disconnected, verify each
logical input and decoder rule, then test under direct observation with all
required obstruction detection and emergency controls operating. A profile is
not a substitute for supervised installation or compliant safety hardware.

## Compatibility policy

Version 1 currently requires exact project identity and application schema 4.
Unknown/newer profile versions and incompatible application schemas are rejected
rather than guessed. A future schema change must add an explicit, tested
migration before older profiles can be accepted.
