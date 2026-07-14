# ALPHA HWR Test Harness -- Open Issues

Open questions and unresolved component behaviors that affect the test harness,
tracked until settled. Several need input from the component author. Mark items
**Resolved** (with the answer) as they close, and prune when no longer relevant.

---

## 1. How long to wait for the pump's real value before verifying a write?

**Status:** OPEN -- needs component author
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

**Needed:** the expected time for the pump's response to arrive after a write,
OR a signal/entity that indicates "the pump has reported back," so verification
can wait on that instead of guessing a fixed delay. (John to ask the author.)

---

## 2. Does a mode change blank the setpoint before repopulating it?

**Status:** OPEN -- confirm on the bench
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
