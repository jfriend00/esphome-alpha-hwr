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
# #1: PROVISIONAL. The component caches writes, so an immediate read-back is our
#     own value, not the pump's. We wait once for the pump to report the real
#     value before verifying. Real timing is TBD from the component author.
VERIFY_SETTLE = 15.0

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
    write_restore(entities.MODE_ENTITY, mode, controller_name, result)

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

    # 3. Restore this mode's params (skipping any that timed out at readiness).
    restore_mode_params(mode, mode_params, controller_name, not_ready, result)

    # 4. Restore the remaining global writables (mode selector already done).
    for ref, value in snap.get("global", {}).items():
        if ref == entities.MODE_ENTITY:
            continue
        write_restore(ref, value, controller_name, result)

    # 5. Let the pump report back the real values, then verify.
    #    VERIFY_SETTLE is provisional -- see OPEN_ISSUES.md #1.
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
        write_restore(ref, mode_params.get(ref), controller_name, result)


def write_restore(ref, value, controller_name, result):
    """Write one restore value, recording the outcome in the RestoreResult."""
    if not has_value(value):
        result.skipped.append(ref)
        return
    domain = entities.domain_of(ref)
    if domain not in ("switch", "number", "select"):
        result.skipped.append(ref)
        log.info(f"restore: skipping read-only '{ref}' (domain '{domain}')")
        return
    if entities.set_value(ref, value, controller_name):
        result.restored.append(ref)
    else:
        result.errors.append({"ref": ref, "reason": "set_value failed (entity missing?)"})


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
