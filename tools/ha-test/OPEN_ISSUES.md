# ALPHA HWR Test Harness -- Open Issues

Open questions and unresolved component behaviors that affect the test harness,
tracked until settled. Several need input from the component author. Mark items
**Resolved** (with the answer) as they close, and prune when no longer relevant.

---

## 1. How long to wait for the pump's real value before verifying a write?

**Status:** RESOLVED (eman, 2026-07-14)
**Affects:** restore verification, and any test that sets a value and checks it

The component **caches** a value you write, so an *immediate* read-back returns
the value you just wrote -- even if the pump later **clamps or rejects** it. The
pump reports the actual accepted value only after some delay:

- An out-of-range setpoint is **clamped** (e.g. writing 1400 RPM clamps to
  1650 RPM on John's pump).
- An invalid temperature-range write (low bound > current high bound) is
  **rejected**.

Consequence: verifying too soon is a **false pass** -- we read our own cached
write, not what the pump actually did. Verification must wait for the pump's
real response before comparing.

**RESOLVED (eman, 2026-07-14):** three overlapping delays after a setpoint write:
1. **~0.4s** -- the component optimistically updates its C++ cache to the
   requested value (so an immediate read-back always "agrees").
2. **~1.6s** -- a TRUE read-back from the pump overwrites the cache with the
   actually-accepted value (catches clamp / reject).
3. **5.0s** -- the HA `number` entities are template sensors with
   `update_interval: 5s`; HA only sees the updated cache when this poll fires.

Reading before ~7s risks the optimistic cached value (false pass). Safe
deterministic waits:
- **After a setpoint write:** >= **7s** (1.6 read-back + 5.0 poll). Harness uses
  `VERIFY_SETTLE = 8` (7 + margin).
- **After a mode change, to verify auto-loaded setpoints:** >= **11s** (5.0
  read-back + 5.0 poll). The harness instead writes its OWN setpoints *after* the
  mode read-back (`MODE_SETTLE`), so its verify follows the 7s setpoint rule.

---

## 2. Does a mode change blank the setpoint before repopulating it?

**Status:** RESOLVED (2026-07-14) -- understood + handled by MODE_SETTLE; pump_ready confirmed NOT applicable (by design)
**Affects:** the restore readiness wait (`wait_until_valid_floats`)

The plan waits until a mode's setpoint reads a valid float before writing the
restore value. This only works if, right after a mode change, the setpoint
entity goes **invalid** (unknown / NaN / unavailable) and then flips valid once
the component syncs. If it instead keeps showing the **old** mode's value (stale
but valid), "wait for valid" would fire immediately and we'd write too early.

**Needed:** watch a setpoint entity through a mode change and confirm it blanks
out before repopulating. If it stays stale, gate on
`binary_sensor.<controller>_pump_ready` (or a mode-changed readback) instead.

Note: the component reportedly has a built-in ~5s settle after a mode change
before it reads the new setpoint -- which is why the readiness timeout is 15s,
not 5s. Worth confirming the exact value with the author.

**Update (v0.10.1):** `binary_sensor.<controller>_pump_ready` now EXISTS -- "on"
means OK to read/write and reads are valid; any other value (including
`unavailable`) means not OK. Right now it only reflects **component restart**
readiness, NOT mode-change readiness yet. Plan: gate **test start** on it now;
when the author extends it to mode changes (and ideally to individual
reads/writes), switch the post-mode-change readiness wait from the valid-float
heuristic to gating on `pump_ready`. Requested extension: apply `pump_ready` to
mode changes / reads / writes.

**Confirmed (2026-07-14):** the valid-float readiness heuristic is NOT enough on
its own. Loading a profile after a temperature -> constant-speed change restored
the mode but left the setpoint at the pump's live value (wrote 1650, read back
2000) -- our write raced the component's post-mode-change sync and got clobbered.
Workaround in `snapshot.py`: `MODE_SETTLE` (fixed ~8s wait after a *real* mode
change, before writing setpoints). eman (2026-07-14) confirms `set_mode`
schedules its read-back **~5.0s** after the switch (the pump is slower to update
passive notifications post-mode-change), so 8s clears it. Two component delays:
C++ `set_mode` waits 5000ms before `sync_cache_async` queries the pump (setpoint
is `NAN` for >=5s if it's the first switch to that mode since boot); ESPHome
`number` template entities poll every 5s.

**`pump_ready` will NOT help here -- BY DESIGN (eman, 2026-07-14).** ready_status
is driven by `is_state_synchronized()` -> `is_cache_valid()`, which checks core
state (`mode_valid_`, `pump_enabled_valid_`) but INTENTIONALLY not the
mode-specific setpoints; and `set_mode` doesn't invalidate the core cache -- so
ready_status stays ON the whole time through a mode change. This SUPERSEDES the
"gate on pump_ready once it covers mode changes" expectation noted earlier: the
fixed `MODE_SETTLE` delay (past the ~5.0s C++ query) is the correct approach, not
a stopgap. Also: eman's "poll until the setpoint reads a valid float" suggestion
works only when the setpoint goes `NAN` first (first switch to a mode since boot);
when it's already CACHED it shows the stale value immediately, the poll returns at
once, and a too-early write is clobbered -- exactly the bug we hit. Hence the
fixed pre-write delay, not just the poll. (Worth relaying to eman -- his
valid-float suggestion has this gap.)

---

## 3. Constant Flow / Constant Pressure setpoints unreadable until first set

**Status:** Known component limitation (John's current build)
**Affects:** snapshot capture + restore + verify for those two modes

In John's build, `constant_flow_setpoint` and `constant_pressure_setpoint` have
no readable value until they've been set at least once, so they snapshot as
`null`. The harness handles this gracefully (restore skips `null` values), but
those modes' setpoints can't be captured / restored / verified until the
underlying readout is fixed.

**Needed:** track whether/when this is fixed upstream; revisit once those
setpoints read reliably.

---

## 4. When should we reboot the controller to read TRUE in-pump state?

**Status:** OPEN -- design question (also for component author input)
**Affects:** the whole verification strategy; "settings survive a reboot" tests

Because the component caches, the ONLY way the test tool can read the actual
"in-pump" state (the pump is the source of truth) rather than the component's
cached view is to **reboot the controller** -- which clears the cache and forces
a fresh fetch from the pump on reconnect. That's authoritative but **messy and
slow** (reboot + wait for reconnect/ready on every check), so it can't be the
default for every value.

This is really a weakness in the test-tool <-> component **contract**: there is
no guaranteed "read straight from the pump" path short of a reboot.

Open questions:
- For which checks is a cache read-back trustworthy, and for which do we need a
  reboot to be certain?
- A reboot IS the natural mechanism for a deliberate "settings survive a reboot /
  power cycle" test -- so some tests will want it on purpose.

**Needed:** author's view on whether the component could expose a "force refresh
from pump" (bypass cache, no full reboot); and a harness decision on when to use
reboot-based verification vs. cache read-back.

---

## 5. Cross-pump-model profile safety

**Status:** OPEN -- noted, NOT implemented
**Affects:** profiles (save/load)

A profile is a saved snapshot of pump settings. Loading a profile saved on one
pump **model** onto a different model could apply wrong or unsafe settings. eman
targets multiple models; he and John currently share the same pump and have no
others to test, so this is deferred.

Idea: gate `load_profile` on pump identity from the diagnostic sensors.
- **Product Name + Product Version** -- likely the right grain (model / family).
- **Serial Number** -- per-unit; too strict (would block sharing a profile
  between two *identical* pumps, which we want to allow).
- **Software Version** (firmware) -- probably a soft WARN, not a hard block.
- **Hardware Version** -- finer than needed.

**Needed:** decide (with eman) the comparison field(s). Cheap forward step: save
those identity fields into each profile's JSON now (metadata) so a future gate
has the data for old profiles too. (Requires the diagnostic entities' leaf
names.) A code comment marks the spot in `profiles.py`.

---

## 6. Pump on/off command is FUSED with the setpoint (clobbers a recent write)

**Status:** worked around; interface concern for eman
**Affects:** restore, load_profile, any test that sets a setpoint then toggles on/off

The pump start/stop command **carries the setpoint** (it re-sends the component's
cached setpoint). A setpoint write's cache update lands at ~0.4s (optimistic) /
~1.6s (true read-back), so toggling `pump_enabled` sooner sends a fused command
with the **stale** setpoint, clobbering the just-written value.

Observed (2026-07-14, ESP32 logs): restore wrote `constant_speed_setpoint = 1650`,
then ~0.05s later wrote `pump_enabled = off`; the stop command carried the stale
`2000` (the 1650 read-back hadn't landed), so the pump ended at 2000. Component
log: `Setting constant speed to 1650` -> `Pump stop command sent (mode=2)` ->
`Control mode updated ... setpoint=2000.00`.

Also note: the setpoint write requires **remote mode ON**, and it succeeds only
while remote is still on -- so we can't simply reorder setpoints-last (remote
gets disabled in the globals step). Hence "settle *between* setpoint and on/off".

Workaround: `WRITE_SETTLE` (~5s, pessimistic, past the ~1.6s read-back) after
EVERY write, so each command settles into the cache before the next -- fused
commands then carry our value. Slow but deterministic; tune down / make it a
per-command coupling model once the interface is cleaner.

**Interface concern (for eman):** the fused on/off+setpoint plus write-cache
timing makes deterministic external control racy -- every consumer that sets a
setpoint then toggles the pump must insert timing-coupled delays. Cleaner
options: don't re-send the setpoint on on/off; or expose readiness so the fused
command waits for the cache; or provide an atomic state-set. Same theme as #1/#2.
