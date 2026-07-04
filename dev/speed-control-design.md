# ALPHA HWR speed / flow control: design, models, and the open experiment

Context (as of 2026-07-03) for an eventual conversation with eman about fixing
speed/flow control upstream, and for John's pending decision on which model to adopt.

## The two operating models

1. **HA-driven, on-demand (John's fork today)**
   - Speed setpoint lives in Home Assistant.
   - Turning on uses the Class 10 on-command carrying the real speed value.
   - John's fork: `pump_start(desired_speed_rpm)` injects the HA value into the Class 10
     turn-on command instead of eman's hard-wired table value. Built this because it was
     SIMPLER than fixing the existing code: we knew exactly where the speed is injected (the
     Class 10 on-command), so we just sourced the value from our HA control rather than the
     hard-coded table.
   - Preference (NOT a requirement): an HA-stored setpoint persists across pump replacement
     or pump reset (HA remembers it even if the pump is swapped or factory-reset).

2. **Pump-scheduled, unattended (eman's model)**
   - The setpoint MUST live in the pump, because the pump's built-in scheduler runs
     unattended and needs the setpoint locally.
   - Turning on from outside (HA/ESPHome) must NOT clobber the pump's stored setpoint.

## The architectural wart

- Class 10 `start()` ALWAYS writes a setpoint suffix. Even a "bare" start sends a hard-coded
  default (`{0x45, 0x65, 0x70, 0x00}`). So on/off and setpoint-write are fused into one command.
- For the pump-scheduled model that is a trap: you can't "just turn on" without either
  (a) clobbering the stored setpoint (eman's current behavior = broken, no speed control), or
  (b) reading the pump's setpoint back and echoing it into the on-command (awkward
      read-then-echo, with a startup timing race between the setpoint sync and the on-command).
- Nobody would architect it this way on purpose; it's an artifact of fusing on/off with
  setpoint-write.

## The clean-fix hypothesis: Class 3 bare on/off

- A Class 3 command `[0x03, 0xC1, <id>]` is a pure command with NO setpoint payload.
- If a Class 3 bare on/off works, it decouples on/off from setpoint: turn the pump on and it
  runs at its own stored setpoint, untouched. That cleanly serves the pump-scheduled model and
  eliminates the read-then-echo and its timing race.
- John found a specific Class 3 on/off frame in the GENI protocol doc. The reference Python lib
  (control.py) does NOT use a Class 3 bare on/off (it's all Class 10), so this is a
  GENIbus-standard command eman didn't wire up.
- **FOUND (2026-07-03)** in eman's `src/alpha_hwr/constants.py`, section commented
  "Class 3 Commands (not used in BLE captures)":
  ```
  RESET = 0x0301   STOP = 0x0305   AUTO = 0x0306
  REMOTE = 0x0307  CONSTANT_FLOW = 0x0315   CONSTANT_PRESSURE = 0x0318
  ```
  Encoding: `0x03XX` = class 3, command byte XX, sent as `[0x03, 0xC1, XX]`.
  - **STOP = 0x05** -> `[0x03, 0xC1, 0x05]`: a real pump OFF (needs a new method; not implemented).
  - **AUTO (0x06) and REMOTE (0x07) are control-SOURCE switches, NOT pump on/off** (CORRECTION
    2026-07-03 per John's HA experience; an earlier note here wrongly called AUTO the "ON" command):
    - REMOTE (0x07 = existing `enable_remote_mode()`): external HA/BLE drives; the pump ignores its
      local controls. Upstream keeps this ON so HA can command the pump, and turns it on when you
      enable pump_enabled (remote-on is required to control from HA).
    - AUTO (0x06 = existing `disable_remote_mode()`): hands control BACK to the pump's own internal
      logic/schedule. The OPPOSITE of HA control, not a pump-on.
  - So there is NO Class 3 bare "ON at stored setpoint" for HA-driven control. STOP is the only
    clean on/off-ish Class 3 command (an off). The "divorce on/off from speed" only truly exists in
    the PUMP-SCHEDULED model: set the setpoint once, then AUTO, and the pump runs its own
    schedule/setpoint (HA gives up on/off). For HA-driven on/off there is no clean Class 3 command;
    Class 10 (embeds a setpoint, which John sources from HA) is the only path, and John's fork
    already does that cleanly.
  - **TWO CAVEATS for the experiment:** (1) These are labeled "not used in BLE captures", eman
    documented them (likely from the GENIbus spec) but the app never sends them, so whether they
    even work over the ALPHA HWR's BLE is UNVERIFIED and is the first thing the experiment must
    confirm. (2) AUTO defers to the pump's own logic/schedule ("run per your internal state"), so
    it's "let the pump govern" rather than "force ON" - it suits the pump-scheduled model but may
    not force the pump on if its schedule/state says off. (3) KEY unknown / the real go-no-go:
    AUTO disables remote control (hands control to the pump), so after sending AUTO, HA/BLE may be
    locked out of sending any further command (e.g. a later STOP) until REMOTE is re-enabled. So
    the clean-looking STOP(off)/AUTO(on) pair may NOT compose into "HA turns it on and off",
    turning it on via AUTO could sever the ability to turn it off, forcing a REMOTE->STOP dance.
    Whether a reliable HA-triggerable pump-hosted on/off is achievable hinges entirely on this
    remote-mode interaction over BLE. THIS is what "assuming it all works reliably" means, and it
    is the actual thing the experiment would have to establish.

## The experiment (to run, ~20-30 min build)

- Add a method that sends `{0x03, 0xC1, <on-id>}` (and one for off) - a copy of
  `ControlService::enable_remote_mode()` with the ID swapped:
  ```cpp
  const uint8_t apdu[3] = {0x03, 0xC1, <id>};
  uint8_t raw[32];
  size_t len = protocol::build_geni_packet(0xE7, 0xF8, apdu, 3, raw);
  std::vector<uint8_t> pkt(raw, raw + len);
  transport_.send_command(pkt);
  ```
  (`build_geni_packet` handles 0x27 framing + CRC.)
- Wire each to a template button in the controls package.
- Test: set a known speed V (via `set_constant_speed` / the setpoint register), press Class 3 ON,
  read the running speed. Holds V = clean (stored setpoint governs, the fix). Reverts to a
  default = still resets.
- Nuance: the pump may require `enable_remote_mode()` first before honoring a bare start; if a
  standalone ON does nothing, try `enable_remote_mode()` then ON.
- Relevant existing methods: `enable_remote_mode()` `[0x03,0xC1,0x07]` / `disable_remote_mode()`
  `[0x03,0xC1,0x06]` (the Class 3 send precedent); `set_constant_speed_async` (writes the speed
  setpoint); `start()`/`stop()` (Class 10 on/off).

## Python reference library (github.com/eman/alpha-hwr) findings

- Repo is active (last commit 2026-06-30). Control code: `alpha_hwr/services/control.py`.
- control.py does speed control CORRECTLY via `set_constant_speed(rpm)`: it passes the real RPM
  into the control request AND writes the dedicated setpoint register. Only bare `start()`
  (setpoint=None) uses the hard-coded suffix. So the hard-coded-speed bug is a PORT artifact;
  eman's own reference does it right, just not via the path the ESPHome port uses.
- Flow/pressure "nonsense numbers" = a unit-conversion bug eman ALREADY fixed in Python:
  commit 2026-05-15 "fix: SetpointInfo.control_mode type and double Pa->m conversion". The
  ESPHome port never received it. (Speed is essentially unitless, no conversion, which is why
  John switched to speed control.)
- Caveat: these findings are from a fast WebFetch read of the repo; spot-check the control.py
  specifics before quoting them to eman.

## John's pending decision (before raising with eman)

- Decide whether he is OK with a pump-stored setpoint model (assuming a working, bug-free Class 3
  bare on/off). If yes, the library could support just that ONE model and John would live with it
  (giving up the HA-persistence-across-pump-replacement nicety, which he prefers but does not
  require).
- If John keeps HA-stored: his Class 10 + real-value approach already matches control.py's
  `set_constant_speed`; the upstream fix is to route ESPHome speed through that path instead of
  the hard-coded `start()`.

## Framing for eman (when ready)

- Two legit models. HA-driven (John's) is fine with Class 10 + real value. Pump-scheduled
  (eman's) fundamentally wants a bare on/off so on/off doesn't disturb the stored setpoint.
- Pitch the Class 3 decoupling as serving HIS model: "your on/off is fused with setpoint-write,
  which forces a read-then-echo + startup race for the scheduled model; here's a Class 3 bare
  on/off that decouples them, tested on my pump." Zero-ego, serves his use case.
- Separately: the flow/pressure unit fix already exists in your control.py (May 15); the port
  is missing it.
- Do NOT surface prematurely or expect eman to rework shared files mid-stream (coordination
  caution in the eman-profile notes).
