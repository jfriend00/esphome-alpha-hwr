"""Snapshot and restore of pump state for the ALPHA HWR test harness.

Captures the CURRENT operating mode, that mode's parameters, and the global
values into a plain dict -- and restores them. Only the active mode's parameters
are captured (other modes' settings are not readable on this component); this is
an agreed simplifying assumption.

Capture is pure reads via the entity helpers (controller_name is passed in from
__init__, where app_config is readable). Each mode parameter is stored as a
{"value": ..., "unit": ...} pair -- the value in the user's DISPLAY unit (what HA
shows) and its unit_of_measurement alongside it (unit is null for params without
one, e.g. a switch). Storing display units keeps a saved profile readable and
comparable to the live entities; conversion to the pump API's native units
happens only at the write boundary (units.py).

Restore writes ONLY through the pump's op_id services (settle.PumpApi): each
write blocks until the component reports it settled, so there are no arbitrary
delays and no read-back verification -- the settle STATUS is the verification.
Two writable groups collapse into single atomic calls (Temperature Control's
min/max/autoadapt; Cycle Time's on/off/flow), and the coupled run state goes back
via one pump_set_state call.

Snapshot format is versioned (top-level "version") so a future format change can
be detected and, if needed, migrated. Note on data shapes: the SNAPSHOT is a
plain dict (dynamic, ref-keyed, and its "global" field can't be an attribute --
reserved word). The RESTORE result is a fixed-shape record (a small class).
"""

from . import entities
from . import settle
from . import units

# Snapshot format version. Bump when the on-disk shape changes; restore checks it
# and (for now) warns on a mismatch. v1 = co-located {"value", "unit"} params.
SNAPSHOT_VERSION = 1

# --- Temperature Control refs (restored as one atomic set_temperature_range) --
TEMP_MODE = "Temperature Control"
TEMP_MIN = "number,temperature_range_min"
TEMP_MAX = "number,temperature_range_max"
TEMP_AUTOADAPT = "switch,temperature_autoadapt"

# --- Cycle Time Control refs (restored as one atomic set_cycle_times) --------
CYCLE_MODE = "Cycle Time Control"
CYCLE_ON = "number,cycle_time_on"
CYCLE_OFF = "number,cycle_time_off"
CYCLE_FLOW = "number,cycle_flow"

# The run states pump_set_state accepts. 'stalled' (a runtime-only illegal state,
# issue #124) is not settable and is normalized to 'scheduled' on restore.
SETTABLE_RUN_STATES = ("off", "engaged", "scheduled")


class RestoreResult:
    """Outcome of a restore -- which refs settled OK, were skipped, or errored."""
    def __init__(self):
        self.restored = []   # refs whose write settled 'accepted'
        self.skipped = []    # refs with no captured value -> nothing written
        self.errors = []     # [{"refs":[...], "command":, "status":, "reason":}]

    def to_dict(self):
        return {
            "restored": self.restored,
            "skipped": self.skipped,
            "errors": self.errors,
        }


def has_value(value):
    """True if a captured value is present (not None/unknown/unavailable)."""
    return value not in (None, "unknown", "unavailable")


def as_float(value):
    """Parse a captured value to float, or None if it isn't numeric."""
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


def param_value(mode_params, ref):
    """The captured display value for a mode param, or None if not captured."""
    entry = mode_params.get(ref)
    if entry is None:
        return None
    return entry.get("value")


def param_unit(mode_params, ref):
    """The captured display unit for a mode param, or None if it has none."""
    entry = mode_params.get(ref)
    if entry is None:
        return None
    return entry.get("unit")


def get_snapshot(controller_name):
    """Capture pump state into a dict. Reads only; changes nothing.

    Structure (format v1):
        {
          "version":     1,
          "mode":        "<active mode>",
          "global":      { "domain,leaf": value, ... },   # excl. the mode selector
          "mode_params": { "domain,leaf": {"value": v, "unit": u}, ... },
        }
    Params are in the entity's DISPLAY units; each carries its unit ("unit" is
    null for params with none) so restore can convert to the API's native units.
    A value of None means that entity did not resolve (see the warning log) --
    handy for spotting a wrong leaf name while bringing the registry up.
    """
    mode = entities.get_value(entities.MODE_ENTITY, controller_name)

    snap = {
        "version": SNAPSHOT_VERSION,
        "mode": mode,
        "global": {},
        "mode_params": {},
    }

    for ref in entities.SNAPSHOT_GLOBAL:
        if ref == entities.MODE_ENTITY:
            continue  # captured separately as snap["mode"]
        snap["global"][ref] = entities.get_value(ref, controller_name)

    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        snap["mode_params"][ref] = {
            "value": entities.get_value(ref, controller_name),
            "unit": entities.get_unit(ref, controller_name),  # None for non-number params
        }

    return snap


def restore_snapshot(snap, controller_name):
    """Restore a snapshot captured by get_snapshot(). Returns a RestoreResult.

    Order: set the mode, then that mode's setpoint(s), then the coupled run state
    LAST (so the pump is never engaged before its setpoint is correct). Every
    write awaits its settle, which is what makes ordering deterministic and lets
    us drop every fixed delay the old delay-based restore relied on. Number values
    are converted from their captured display units to the API's native units
    first (units.to_native); a conversion that can't be done SKIPS that write
    rather than sending a guessed value.
    """
    result = RestoreResult()
    api = settle.ApiWriter(controller_name)

    version = snap.get("version")
    if version != SNAPSHOT_VERSION:
        log.warning(f"restore: snapshot format version {version} != expected "
                    f"{SNAPSHOT_VERSION}; attempting restore anyway")

    mode = snap.get("mode")
    mode_params = snap.get("mode_params", {})
    global_vals = snap.get("global", {})

    if not has_value(mode):
        result.errors.append({"refs": [entities.MODE_ENTITY],
                              "reason": f"snapshot mode was '{mode}'; cannot restore"})
        return result

    machine = entities.MODE_DISPLAY_TO_MACHINE.get(mode)
    if machine is None:
        result.errors.append({"refs": [entities.MODE_ENTITY],
                              "reason": f"mode '{mode}' has no machine identifier; cannot restore"})
        return result

    # 1. Mode. A settled set_mode means the component has fully applied it,
    #    including its post-change setpoint sync, so nothing written below can be
    #    clobbered -- the old fixed MODE_SETTLE wait is gone.
    log.info(f"restore: setting mode '{mode}' ({machine})")
    record_result(result, [entities.MODE_ENTITY], api.call("set_mode", [machine]))

    # 2. This mode's parameters, via the matching atomic service.
    restore_mode_params(api, mode, machine, mode_params, result)

    # 3. Coupled run state (off/engaged/scheduled) LAST. The only other captured
    #    global, switch,remote_mode, is deliberately NOT restored (no v0.15.0
    #    service; captured for future use only -- see entities.SNAPSHOT_GLOBAL).
    restore_run_state(api, global_vals, result)

    return result


def restore_mode_params(api, mode, machine, mode_params, result):
    """Apply the active mode's captured parameters via the matching service."""
    if mode == TEMP_MODE:
        restore_temperature_range(api, mode_params, result)
    elif mode == CYCLE_MODE:
        restore_cycle_times(api, mode_params, result)
    else:
        restore_scalar_setpoint(api, mode, machine, mode_params, result)


def native_number(ref, mode_params):
    """Resolve one captured number ref to its native-unit float. Returns
    (ok, value, detail). ok=False means the value is missing/non-numeric or its
    unit couldn't be converted -- the caller should not write it."""
    raw = param_value(mode_params, ref)
    if not has_value(raw):
        return (False, None, f"'{ref}' not captured")
    fv = as_float(raw)
    if fv is None:
        return (False, None, f"'{ref}' non-numeric ('{raw}')")
    return units.to_native(ref, fv, param_unit(mode_params, ref))


def restore_scalar_setpoint(api, mode, machine, mode_params, result):
    """Modes with a single scalar setpoint (constant speed/flow/pressure,
    proportional pressure): convert to native units, then set_setpoint."""
    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        if not has_value(param_value(mode_params, ref)):
            result.skipped.append(ref)
            log.info(f"restore: '{ref}' not captured; skipping")
            continue
        ok, value, detail = native_number(ref, mode_params)
        if not ok:
            result.errors.append({"refs": [ref], "reason": detail})
            continue
        log.info(f"restore: set_setpoint {machine}={value} ({detail})")
        record_result(result, [ref], api.call("set_setpoint", [machine, value]))


def restore_temperature_range(api, mode_params, result):
    """Temperature Control: min + max + autoadapt go back as ONE atomic
    set_temperature_range call (native °C) -- no min>max intermediate state to
    order around, which retires the old safe-order TODO."""
    refs = [TEMP_MIN, TEMP_MAX, TEMP_AUTOADAPT]
    ok_min, min_c, d_min = native_number(TEMP_MIN, mode_params)
    ok_max, max_c, d_max = native_number(TEMP_MAX, mode_params)
    if not (ok_min and ok_max):
        result.errors.append({"refs": refs,
                              "reason": f"temperature range not restored: {d_min}; {d_max}"})
        return
    aa_raw = param_value(mode_params, TEMP_AUTOADAPT)
    if has_value(aa_raw):
        autoadapt = str(aa_raw).lower() == "on"
    else:
        autoadapt = False
        log.warning("restore: temperature autoadapt not captured; defaulting to off")
    log.info(f"restore: set_temperature_range min={min_c} max={max_c} autoadapt={autoadapt}")
    record_result(result, refs, api.call("set_temperature_range", [min_c, max_c, autoadapt]))


def restore_cycle_times(api, mode_params, result):
    """Cycle Time Control: on + off minutes + flow go back as ONE atomic
    set_cycle_times call. A field we didn't capture is sent as 0 = 'keep the
    pump's existing value' (no real on/off minute or flow is ever 0); captured
    fields are converted to native units first."""
    refs = [CYCLE_ON, CYCLE_OFF, CYCLE_FLOW]
    ok_on, on, d_on = cycle_field(CYCLE_ON, mode_params)
    ok_off, off, d_off = cycle_field(CYCLE_OFF, mode_params)
    ok_flow, flow, d_flow = cycle_field(CYCLE_FLOW, mode_params)
    if not (ok_on and ok_off and ok_flow):
        result.errors.append({"refs": refs,
                              "reason": f"cycle times not restored: {d_on}; {d_off}; {d_flow}"})
        return
    if on == 0.0 and off == 0.0 and flow == 0.0:
        result.skipped.extend(refs)
        log.info("restore: no cycle-time values captured; skipping")
        return
    log.info(f"restore: set_cycle_times on={on} off={off} flow={flow}")
    record_result(result, refs, api.call("set_cycle_times", [on, off, flow]))


def cycle_field(ref, mode_params):
    """A cycle-time field: an uncaptured field is 0 = 'keep existing' (no
    conversion); a captured field is converted to native units. Returns
    (ok, value, detail); ok=False only on a bad/unconvertible captured value."""
    if not has_value(param_value(mode_params, ref)):
        return (True, 0.0, f"'{ref}' not captured (keep)")
    return native_number(ref, mode_params)


def restore_run_state(api, global_vals, result):
    """Restore the coupled run state via pump_set_state (off/engaged/scheduled).
    'stalled' (the illegal 'schedule on but pump stopped' state, #124) is not a
    settable target, so it is normalized to 'scheduled' -- its intended state. A
    snapshot with no captured run state (e.g. a profile saved before this entity
    existed) leaves the pump's run state untouched."""
    raw = global_vals.get(entities.RUN_STATE_REF)
    if not has_value(raw):
        result.skipped.append(entities.RUN_STATE_REF)
        log.warning(f"restore: run state not captured ('{raw}'); leaving pump run state unchanged")
        return
    target = str(raw).lower()
    if target == "stalled":
        log.warning("restore: captured run state was 'stalled'; restoring as 'scheduled'")
        target = "scheduled"
    if target not in SETTABLE_RUN_STATES:
        result.errors.append({"refs": [entities.RUN_STATE_REF],
                              "reason": f"run state '{raw}' is not settable (off/engaged/scheduled)"})
        return
    log.info(f"restore: set_pump_state {target}")
    record_result(result, [entities.RUN_STATE_REF], api.call("set_pump_state", [target]))


def record_result(result, refs, api_result):
    """Fold one settled-write outcome into the RestoreResult. refs is the list of
    registry refs the write covers (one, or several for a grouped atomic call).
    Only 'accepted' counts as restored; anything else (clamped/rejected/invalid/
    timeout/superseded) is recorded as an error with the settle status."""
    if api_result.ok:
        result.restored.extend(refs)
        return
    result.errors.append({
        "refs": refs,
        "command": api_result.command,
        "status": api_result.status,
        "reason": api_result.detail or f"settle status '{api_result.status}'",
    })
