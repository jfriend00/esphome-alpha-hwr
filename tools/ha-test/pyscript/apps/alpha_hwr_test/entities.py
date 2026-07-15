"""Entity registry and access helpers for the ALPHA HWR test harness.

Holds the snapshot registry (which entity refs to save/restore) plus the
helpers that turn a "domain,leaf" ref into a real entity_id and read/write its
value.

A ref is the compact string "domain,leaf", e.g. "select,pump_control_mode".
The leaf is the entity_id with the domain and the install's controller-name
prefix removed. resolve() recombines it with controller_name, which the caller
passes in -- pyscript's app_config is only readable in the app's __init__, so
the name is read+validated there and threaded through:
    "select,pump_control_mode"  ->  select.<controller_name>_pump_control_mode
"""

import time  # for monotonic timeout math (dt_now is NOT available in a sibling module)


# --- Snapshot registry ------------------------------------------------------
# Only WRITABLE entities are meant to live here (snapshot saves/restores them).
# Read-only sensors used for later verification belong elsewhere.

SNAPSHOT_GLOBAL = {
    "select,pump_control_mode",
    "switch,remote_mode",
    "switch,schedule_enabled",
    "switch,pump_enabled",
}

SNAPSHOT_PER_MODE = {
    "Constant Speed": ["number,constant_speed_setpoint"],
    "Cycle Time Control": ["number,cycle_time_off","number,cycle_time_on"],
    "Proportional Pressure": ["number,proportional_pressure_setpoint"],
    "Temperature Control": [
        "switch,temperature_autoadapt",
        "number,temperature_range_max",      # min/max restored via safe-order handler
        "number,temperature_range_min"],
    "Constant Flow": ["number,constant_flow_setpoint"],
    "Constant Pressure": ["number,constant_pressure_setpoint"]
}

# Logical name of the mode selector (used to read/set the active pump mode).
MODE_ENTITY = "select,pump_control_mode"

# Read-only pump-ready gate (v0.10.1+): "on" means OK to read/write and reads
# are valid; ANY other value (off / unavailable / unknown) means not OK.
PUMP_READY = "binary_sensor,pump_ready"


# --- Resolution + access ----------------------------------------------------
# controller_name is threaded in from __init__ (the only place pyscript exposes
# app_config). Making it a required argument means it can never be silently
# unset -- there is no module global to forget to initialize.

def domain_of(ref):
    """Return the domain part of a "domain,leaf" ref."""
    return ref.split(",", 1)[0].strip()


def resolve(ref, controller_name):
    """Turn a "domain,leaf" ref into a full entity_id."""
    domain, leaf = ref.split(",", 1)
    return f"{domain.strip()}.{controller_name}_{leaf.strip()}"


def get_value(ref, controller_name):
    """Read the current state of a ref. Returns None if the entity is missing."""
    entity_id = resolve(ref, controller_name)
    if not state.exist(entity_id):
        log.warning(f"entities.get_value: {entity_id} does not exist (from '{ref}')")
        return None
    return str(state.get(entity_id))


def set_value(ref, value, controller_name):
    """Write a value to a ref, dispatching by domain.

    Only writable domains (switch, number, select) are supported; anything else
    (e.g. read-only sensors) is skipped with a warning. Returns True if a write
    was issued.
    """
    entity_id = resolve(ref, controller_name)
    domain = domain_of(ref)
    if not state.exist(entity_id):
        log.warning(f"entities.set_value: {entity_id} does not exist (from '{ref}')")
        return False
    if domain == "switch":
        svc = "turn_on" if str(value).lower() == "on" else "turn_off"
        service.call("switch", svc, entity_id=entity_id)
    elif domain == "number":
        service.call("number", "set_value", entity_id=entity_id, value=float(value))
    elif domain == "select":
        service.call("select", "select_option", entity_id=entity_id, option=value)
    else:
        log.warning(f"entities.set_value: cannot set read-only/unsupported domain "
                    f"'{domain}' for {entity_id}")
        return False
    return True


# --- Readiness / waits ------------------------------------------------------

def is_valid_float(ref, controller_name):
    """True if the ref's entity currently reads a parseable float value."""
    entity_id = resolve(ref, controller_name)
    if not state.exist(entity_id):
        return False
    value = state.get(entity_id)
    if value in (None, "unknown", "unavailable"):
        return False
    try:
        float(value)
        return True
    except (ValueError, TypeError):
        return False


def wait_until_valid_floats(refs, controller_name, timeout=15.0, poll=0.25):
    """Poll until every ref reads a valid float, or until timeout.

    Returns the list of refs still NOT valid at timeout (empty list = all became
    valid). Uses task.sleep, which yields cooperatively, so it does not block
    Home Assistant -- the caller just takes up to `timeout` seconds.
    """
    deadline = time.monotonic() + timeout
    pending = list(refs)
    while pending:
        pending = [r for r in pending if not is_valid_float(r, controller_name)]
        if not pending:
            return []
        if time.monotonic() >= deadline:
            return pending
        task.sleep(poll)
    return []


def is_ready(controller_name):
    """True only if the pump-ready gate reads exactly 'on'."""
    entity_id = resolve(PUMP_READY, controller_name)
    if not state.exist(entity_id):
        return False
    return state.get(entity_id) == "on"


def wait_until_ready(controller_name, timeout=30.0, poll=0.25):
    """Poll until pump_ready is 'on', or timeout. Returns True if it became ready.

    Handles the already-ready case naturally (the first check returns at once).
    """
    deadline = time.monotonic() + timeout
    while True:
        if is_ready(controller_name):
            return True
        if time.monotonic() >= deadline:
            log.warning(f"wait_until_ready: pump_ready not 'on' within {timeout}s")
            return False
        task.sleep(poll)
