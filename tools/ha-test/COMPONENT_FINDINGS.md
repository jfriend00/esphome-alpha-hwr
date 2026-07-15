# ALPHA HWR Component — Findings from the HA Test Harness

Behaviors the test harness surfaced at the **Home Assistant interface** while
building snapshot/restore + profile load. Written to help decide what to file
with the component author. Categorized as **BUG**, **DESIGN GAP**, or
**CONFIRMED BEHAVIOR** (context, not necessarily a defect). Log excerpts are at
the bottom.

Environment: v0.10.1 build, single pump model (ALPHA HWR, Product Version 0x02).

---

## 1. Fused on/off + setpoint write can clobber a just-written setpoint  — **BUG (data loss)**

**What happens:** the pump start/stop command is *fused with the setpoint* — it
re-sends the component's **cached** setpoint. A setpoint write does not update
that cache until its pump read-back lands (~1.6s; see #3). So if an on/off
command is issued within that window after a setpoint write, the on/off command
carries the **stale** cached setpoint and overwrites the value that was just
written.

**Reproduced (restore path):** wrote `constant_speed_setpoint = 1650`, then
~0.05s later wrote `pump_enabled = off`. The stop command fired *before* the
1650 read-back updated the cache, so it carried the old `2000`, and the pump
ended at 2000. See log excerpt A.

**Why it matters (beyond the test tool):** a perfectly ordinary sequence — "set a
setpoint, then turn the pump off" — silently loses the setpoint. This isn't
test-only; any consumer (an automation, a dashboard script) that changes a
setpoint and then toggles the pump within ~2s hits it. It's also **non-obvious**
that turning the pump *off* is coupled to the setpoint at all (even more so when
the pump is already off).

**Design tension worth naming:** the component deliberately does *not* cache the
written value immediately — it waits for the true pump read-back so the reported
value reflects what the pump actually accepted (catching clamp/reject). That's a
good property. But combined with "on/off re-sends the cached setpoint," it opens
this clobber window. The two reasonable choices interact badly.

**Possible fix directions (author's call):**
- Don't re-send the setpoint on an on/off command; or
- Have the on/off command use the just-written (pending) value rather than the
  last-confirmed cache; or
- Expose readiness (see #2) so a consumer can wait rather than guess.

---

## 2. `ready_status` does not reflect setpoint/value readiness  — **DESIGN GAP**

**What happens:** after a mode change, mode-specific setpoints are stale/NaN for
~5s (see #3), but `ready_status` stays **ON** the entire time.

**Author-confirmed reason:** `ready_status` is driven by `is_state_synchronized()`
→ `is_cache_valid()`, which checks core state (`mode_valid_`,
`pump_enabled_valid_`, …) but **intentionally not** the mode-specific setpoints
(`cached_speed_setpoint_`, etc.); and `set_mode` does not invalidate the core
cache, so `ready_status` never toggles off during a mode change.

**Impact:** a consumer *cannot* gate reads/writes of setpoints on `ready_status`
— it's always ON — so it's forced to hard-code delays matched to the component's
internal timing. Any change to that timing silently breaks the consumer.

**Possible directions:**
- Broaden `ready_status` to also drop while a value is settling; **or** add a
  companion "busy"/"syncing" signal so consumers gate on `ready AND NOT busy`.
- **Important coordination requirement:** whatever the signal, when it reads
  "ready," the **HA-exposed value must already reflect the settled value.** The
  setpoint `number` entities are `template` sensors polled every 5s, so a
  readiness flag that flips ON while the value entity is still up to 5s stale
  doesn't actually help — you'd gate correctly and still read the old value.

---

## 3. Post-write / post-mode-change timing consumers must reverse-engineer  — **CONFIRMED BEHAVIOR (context)**

Author-confirmed timing (useful, but the point is consumers shouldn't have to
know it — see #2):

**After a setpoint write:**
- ~0.4s — component optimistically updates its C++ cache to the requested value
  (so an immediate read-back always "agrees" — a false confirmation).
- ~1.6s — a **true read-back** from the pump overwrites the cache with the
  actually-accepted value (catches clamp/reject).
- 5.0s — the HA `number` entities are `template` sensors (`update_interval: 5s`);
  HA only sees the updated cache on this poll.
- ⇒ real value not reliably visible in HA until **~7s** after a write.

**After a mode change:**
- `set_mode` schedules its read-back (`sync_cache_async`) ~**5.0s** after the
  switch (pump is slow to update passive notifications). Setpoint is NaN for ≥5s
  if it's the first switch to that mode since boot.
- ⇒ ~**11s** to verify auto-loaded setpoints (5.0 read-back + 5.0 poll).

**Consequence for the harness (and any consumer):** verifying a write requires
waiting past these fixed delays, and to avoid the #1 clobber, successive writes
must be serialized with pauses. The harness currently carries **three** hard-coded
delays (mode-settle 8s, per-write-settle 5s, verify-settle 8s) purely to match
component internals — which is the core argument for the readiness contract in #2.

---

## 4. "Poll until the setpoint reads a valid float" is insufficient  — **REFINEMENT of prior advice**

The suggested workaround (poll the HA entity until it goes from
unknown/unavailable to a valid float after a mode change) works **only** when the
setpoint actually goes NaN first — i.e. the first switch to a mode since boot.
When the setpoint is **already cached** (a mode entered before), the entity shows
the **stale-but-valid** value immediately, so the poll returns at once and a
too-early write races the 5s sync. (This is exactly how the #1 clobber slipped
through our valid-float wait.) So a poll-for-valid-float is not a substitute for a
real readiness signal.

---

## 5. Setpoint writes require remote mode ON  — **CONFIRMED BEHAVIOR (ordering constraint)**

In the logs, the `constant_speed_setpoint` write succeeded only while remote mode
was still ON; "Disabling remote mode (returning to Auto)" happened in the same
batch just after. So a consumer restoring a state with remote mode OFF must
order: **write the setpoint while remote is still ON, then disable remote** — it
can't simply write setpoints last. (Worth confirming this is the intended
contract, and documenting it.)

---

## 6. Constant Flow / Constant Pressure setpoints unreadable until first set  — **KNOWN LIMITATION (confirm status)**

In this build, `constant_flow_setpoint` and `constant_pressure_setpoint` read
NaN/unavailable until they've been set at least once. The harness handles it
(captures null, skips on restore), but flag it to confirm whether it's tracked /
intended.

---

## Suggested issues to file (your call)

- **BUG** — Fused on/off command clobbers a recently-written setpoint (item 1).
  Strongest, most concrete, with log evidence. Data loss on an ordinary sequence.
- **ENHANCEMENT / DESIGN** — A readiness contract for value/setpoint safety
  (items 2 + 3 + 4): "tell consumers when it's OK to read/write, and make the
  exposed value consistent with that signal," so consumers stop hard-coding the
  component's internal timing.
- **DOCS/CONFIRM** — Setpoint-write-requires-remote-mode ordering (item 5), and
  the Constant Flow/Pressure readout status (item 6).

---

## Log excerpts

### A. Fused on/off clobber (ESP32 component log)

```
[23:17:24.835][I][number:278]: Setting constant speed setpoint to 1650 RPM
[23:17:24.840][I][alpha_hwr.control:889]: Setting constant speed to 1650 RPM...
[23:17:24.845][I][switch:087]: Disabling remote mode (returning to Auto)...
[23:17:24.858][I][switch:089]: Disable remote result: SUCCESS
[23:17:24.863][I][switch:069]: Disabling pump schedule...
[23:17:24.876][I][switch:071]: Disable schedule result: SUCCESS
[23:17:24.880][I][switch:046]: Disabling pump...
[23:17:24.884][I][alpha_hwr.control:456]: Stopping pump...
[23:17:24.889][I][alpha_hwr.control:512]: Pump stop command sent (mode=2)   <-- carries the CACHED (stale 2000) setpoint
[23:17:25.245][I][alpha_hwr.control:920]: ✓ Constant speed set to 1650 RPM  <-- our 1650 write's read-back, too late
[23:17:25.250][I][number:281]: ✓ Speed setpoint updated successfully
[23:17:25.484][I][alpha_hwr.control:330]: Control mode updated to 2 (Constant Speed), setpoint=2000.00  <-- stop's stale 2000 wins
[23:17:25.987][S][number]: 'Constant Speed Setpoint' >> 2000.00
```

Net: we wrote 1650; the pump ended at 2000 because the `off` command's fused,
cache-sourced setpoint (still 2000 at that instant) landed after our write.

### B. Matching HA-side timing (pyscript harness log)

```
23:17:16.404  restore: mode current='Temperature Control' target='Constant Speed' changed=True
23:17:16.411  restore: settling 8.0s after mode change   (mode sync)
23:17:24.417  restore: 'number,constant_speed_setpoint' reads '2000.0' before write, writing '1650.0'
              -> then the on/off writes fire (excerpt A), clobbering it
23:17:32      verify: expected '1650.0', read back '2000.0'
```
