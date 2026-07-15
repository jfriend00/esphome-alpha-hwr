"""Snapshot and restore of pump state for the ALPHA HWR test harness.

Captures the CURRENT operating mode, that mode's writable parameters, and the
global writable settings into a plain dict -- and restores them. Only the
active mode's parameters are captured (other modes' settings are not readable
on this component); this is an agreed simplifying assumption.

controller_name is passed in (from __init__, where app_config is readable) and
threaded down to the entity helpers.

Note on data shapes: the SNAPSHOT stays a plain dict (dynamic, ref-keyed, and
its "global" field can't be an attribute -- reserved word). The RESTORE result
is a fixed-shape record, so it's a small class with dot access.
"""

from . import entities

# --- Restore tuning (see OPEN_ISSUES.md) ------------------------------------
# #2: how long to wait for a mode's setpoints to become readable after a mode
#     change (component has a built-in ~5s settle before it reads them).
READINESS_TIMEOUT = 15.0
# #1 RESOLVED (eman, 2026-07-14): a setpoint write updates the C++ cache
#    optimistically at ~0.4s, then a TRUE read-back from the pump lands at ~1.6s
#    (catching clamp/reject), and the HA number entities are template sensors
#    polled every 5.0s -- so HA only sees the real value at ~1.6 + 5.0 = ~7s.
#    Reading sooner risks the optimistic cached value (false pass). 8s = 7 + margin.
VERIFY_SETTLE = 8.0
# After an actual mode CHANGE the component runs a post-change sync (reads the
# pump's setpoints for the new mode) taking a few seconds; writing our setpoints
# before it finishes lets the sync CLOBBER them (observed: loaded profile put the
# mode back but left the setpoint at the pump's live value). Wait this out after
# a real mode change. eman: set_mode schedules its read-back ~5.0s after the
# switch, so a setpoint written sooner gets clobbered -- wait past it. 8s margin.
# NOTE: pump_ready CANNOT gate this -- eman confirms ready_status stays ON through
# a mode change BY DESIGN (is_cache_valid checks core state, not mode setpoints;
# set_mode doesn't invalidate the core cache). So this fixed delay is THE approach.
# And a "poll until valid float" alone is insufficient: a CACHED setpoint reads
# valid immediately, so the poll returns too soon (that was the bug). See ISSUE #2.
MODE_SETTLE = 8.0
# PESSIMISTIC settle after EVERY ordinary write, before the next command. The
# component doesn't cache a write until its pump read-back lands (~1.6s), and
# some commands are FUSED (the on/off command re-sends the cached setpoint), so
# firing the next command sooner can carry a STALE value and clobber ours
# (OPEN_ISSUES #6). Until the component exposes readiness -- or we build a proper
# per-command coupling model -- we serialize writes with a generous pause.
# Deliberately long/safe for now; tune down once the interface is cleaner.
WRITE_SETTLE = 5.0

# Temperature range refs need safe-order handling on restore (pump rejects any
# intermediate state where min > max). See restore_mode_params (TODO).
TEMP_MODE = "Temperature Control"
TEMP_MIN = "number,temperature_range_min"
TEMP_MAX = "number,temperature_range_max"


class RestoreResult:
    """Outcome of a restore -- which refs were written, skipped, or errored."""
    def __init__(self):
        self.restored = []   # refs written OK
        self.skipped = []    # refs with no value, or read-only -> not written
        self.errors = []     # [{"ref":, "reason":}] readiness timeout / verify mismatch / ...

    def to_dict(self):
        return {
            "restored": self.restored,
            "skipped": self.skipped,
            "errors": self.errors,
        }


def has_value(value):
    """True if a captured value is present (not None/unknown/unavailable)."""
    return value not in (None, "unknown", "unavailable")


def get_snapshot(controller_name):
    """Capture pump state into a dict. Reads only; changes nothing.

    Structure:
        {
          "mode":        "<active mode>",
          "global":      { "domain,leaf": value, ... },   # excl. the mode selector
          "mode_params": { "domain,leaf": value, ... },   # active mode's params
        }
    A value of None means that entity did not resolve (see the warning log) --
    handy for spotting a wrong leaf name while bringing the registry up.
    """
    mode = entities.get_value(entities.MODE_ENTITY, controller_name)

    snap = {
        "mode": mode,
        "global": {},
        "mode_params": {},
    }

    for ref in entities.SNAPSHOT_GLOBAL:
        if ref == entities.MODE_ENTITY:
            continue  # captured separately as snap["mode"]
        snap["global"][ref] = entities.get_value(ref, controller_name)

    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        snap["mode_params"][ref] = entities.get_value(ref, controller_name)

    return snap


def restore_snapshot(snap, controller_name):
    """Restore a snapshot captured by get_snapshot(). Returns a RestoreResult.

    Flow: set the mode -> wait for the mode's setpoints to become readable ->
    write the mode params -> write the globals -> settle -> verify everything
    read back.
    """
    result = RestoreResult()
    mode = snap.get("mode")
    mode_params = snap.get("mode_params", {})

    if not has_value(mode):
        result.errors.append({"ref": entities.MODE_ENTITY,
                              "reason": f"snapshot mode was '{mode}'; cannot restore"})
        return result

    # 1. Return to the original mode first (recorded so verify checks it too).
    #    On a real mode CHANGE the component runs a post-change sync that would
    #    clobber setpoints written too soon, so wait out a fixed settle first.
    current_mode = entities.get_value(entities.MODE_ENTITY, controller_name)
    mode_changed = current_mode != mode
    log.info(f"restore: mode current='{current_mode}' target='{mode}' changed={mode_changed}")
    wrote_mode = write_restore(entities.MODE_ENTITY, mode, controller_name, result)
    if mode_changed:
        # Mode change needs the component's ~5s sync -- longer than an ordinary write.
        log.info(f"restore: settling {MODE_SETTLE}s after mode change")
        task.sleep(MODE_SETTLE)
    elif wrote_mode:
        task.sleep(WRITE_SETTLE)

    # 2. Wait for this mode's NUMBER setpoints (that we have values for) to
    #    become readable -- the component needs time to sync after a mode change.
    number_refs = []
    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        if entities.domain_of(ref) == "number" and has_value(mode_params.get(ref)):
            number_refs.append(ref)
    not_ready = entities.wait_until_valid_floats(number_refs, controller_name,
                                                 timeout=READINESS_TIMEOUT)
    for ref in not_ready:
        result.errors.append({"ref": ref, "reason":
            f"never became a valid float within {READINESS_TIMEOUT}s -- not written"})

    # 3. Restore this mode's params, settling after EACH write (writes race /
    #    fuse otherwise -- OPEN_ISSUES #6).
    restore_mode_params(mode, mode_params, controller_name, not_ready, result)

    # 4. Restore the remaining global writables, settling after EACH write.
    for ref, value in snap.get("global", {}).items():
        if ref == entities.MODE_ENTITY:
            continue
        if write_restore(ref, value, controller_name, result):
            task.sleep(WRITE_SETTLE)

    # 5. Let the pump report back the real values, then verify (HA poll latency).
    #    See OPEN_ISSUES.md #1.
    task.sleep(VERIFY_SETTLE)
    verify_restored(snap, controller_name, result)

    return result


def restore_mode_params(mode, mode_params, controller_name, skip, result):
    """Write a mode's params.

    TODO (safe-order handler): Temperature Control's min/max must be written in
    an order that never creates min > max (the pump rejects it) -- the safe order
    depends on the current values. For now they are written in registry order;
    the dynamic safe-order handler is the next add.
    """
    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        if ref in skip:
            continue
        target_val = mode_params.get(ref)
        pre = entities.get_value(ref, controller_name)
        log.info(f"restore: '{ref}' reads '{pre}' before write, writing '{target_val}'")
        if write_restore(ref, target_val, controller_name, result):
            task.sleep(WRITE_SETTLE)


def write_restore(ref, value, controller_name, result):
    """Write one restore value, recording the outcome. Returns True if a write
    was actually issued (so the caller can settle before the next command)."""
    if not has_value(value):
        result.skipped.append(ref)
        return False
    domain = entities.domain_of(ref)
    if domain not in ("switch", "number", "select"):
        result.skipped.append(ref)
        log.info(f"restore: skipping read-only '{ref}' (domain '{domain}')")
        return False
    if entities.set_value(ref, value, controller_name):
        result.restored.append(ref)
        return True
    result.errors.append({"ref": ref, "reason": "set_value failed (entity missing?)"})
    return False


def verify_restored(snap, controller_name, result):
    """Read every restored writable back and flag any that don't match."""
    for ref in list(result.restored):
        expected = expected_value(snap, ref)
        actual = entities.get_value(ref, controller_name)
        if not values_match(ref, expected, actual):
            result.errors.append({"ref": ref, "reason":
                f"verify: expected '{expected}', read back '{actual}'"})


def expected_value(snap, ref):
    """Look up the snapshot value for a ref (mode selector, global, or mode param)."""
    if ref == entities.MODE_ENTITY:
        return snap.get("mode")
    if ref in snap.get("global", {}):
        return snap["global"][ref]
    return snap.get("mode_params", {}).get(ref)


def values_match(ref, expected, actual):
    """Compare a restored value against its read-back, per domain."""
    if not has_value(actual):
        return False
    if entities.domain_of(ref) == "number":
        try:
            return abs(float(expected) - float(actual)) < 0.01
        except (ValueError, TypeError):
            return False
    return str(expected) == str(actual)
