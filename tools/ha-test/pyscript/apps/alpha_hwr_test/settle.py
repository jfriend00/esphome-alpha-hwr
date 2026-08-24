"""Settled writes to the ALPHA HWR pump (issue #92) -- two backends, one result.

Every write is confirmed by the component's terminal `esphome.alpha_hwr_write_settled`
event (fixed name), whose `status` becomes a plain pass/fail on the returned
ApiResult -- no arbitrary delays, no read-back verification.

  * await_settle(op_id)        -- wait for one op_id-correlated settle, or time out.
  * await_settle_entity()      -- wait for the next origin=entity settle (entity
                                  writes carry no op_id; valid under sole-writer +
                                  serialized writes).
  * ApiWriter                  -- API backend: call(verb,args) -> op_id service.
  * EntityWriter               -- ENTITY backend: call(verb-or-ref,args) -> HA
                                  entity write(s) (number/switch/select), the same
                                  path a UI user drives. Same ApiResult shape.

Race safety: call the service/entity, THEN wait. Every settle event originates in
the component on the ESP32 and returns over the API connection, so it cannot
arrive in the same event-loop tick as the fire-and-return call; HA's
single-threaded loop can't process it until we yield inside the wait.
"""

# EntityWriter needs the entity registry + unit conversion. Dual-import so settle
# loads both as a pyscript-app sibling (`from .`) and standalone in host tests.
try:
    from . import entities
    from . import units
except ImportError:  # host-test context: app dir on sys.path
    import entities
    import units

# The component's terminal settle event (fixed name, not controller-prefixed).
SETTLE_EVENT = "esphome.alpha_hwr_write_settled"

# How long to wait for a settle event, in seconds. The docs say 30s covers
# everything except refresh_single_events, so a real lost command comes back as
# an authoritative 'timeout' rather than a silent client-side give-up.
SETTLE_TIMEOUT = 30

# The one settle status that means "the pump confirmed exactly what we asked for".
STATUS_ACCEPTED = "accepted"

# Keys in the task.wait_until result that are trigger/event plumbing, not part of
# the pump's settle payload -- dropped before we keep/log the event.
_EVENT_META_KEYS = ("trigger_type", "event_type", "context")


class ApiState:
    """Cross-call counter for op_id uniqueness (the recirc_pump Status pattern).
    Reset on reload, which is fine: a reload also cancels any in-flight wait."""
    op_seq = 0


def next_op_id():
    """Return a process-unique op_id string for one write."""
    ApiState.op_seq += 1
    return f"hwrtest_{ApiState.op_seq}"


def await_settle(op_id, timeout=SETTLE_TIMEOUT):
    """Wait for the settle event matching op_id. Returns the flattened
    task.wait_until result dict (event fields as keys), or None on timeout."""
    result = task.wait_until(
        event_trigger=[SETTLE_EVENT, f"op_id == '{op_id}'"],
        timeout=timeout,
    )
    if result["trigger_type"] == "timeout":
        return None
    return result


def await_settle_entity(command=None, timeout=SETTLE_TIMEOUT):
    """Wait for the next ENTITY-origin settle event (an entity write, which carries
    NO op_id). Correlates by 'the next origin==entity event', valid only when we are
    the SOLE writer AND serialize writes. Optionally narrow by command. Returns the
    flattened result dict, or None on timeout."""
    cond = "origin == 'entity'"
    if command:
        cond = f"origin == 'entity' and command == '{command}'"
    result = task.wait_until(
        event_trigger=[SETTLE_EVENT, cond],
        timeout=timeout,
    )
    if result["trigger_type"] == "timeout":
        return None
    return result


class ApiResult:
    """Outcome of one settled write (fixed shape -> dot access).

    ok is the simple success flag (status == accepted). status/detail/event carry
    the full picture. event is the settle payload with plumbing keys removed, so it
    holds the requested_* echoes and settled values side by side.
    """
    def __init__(self, command, op_id):
        self.command = command
        self.op_id = op_id
        self.status = None       # settle status string, or "timeout"
        self.ok = False          # status == STATUS_ACCEPTED
        self.detail = None       # component-supplied reason, when present
        self.timed_out = False   # no settle event within the budget
        self.event = None        # settle payload dict (meta keys stripped)

    def to_dict(self):
        return {
            "command": self.command,
            "op_id": self.op_id,
            "status": self.status,
            "ok": self.ok,
            "detail": self.detail,
            "timed_out": self.timed_out,
            "event": self.event,
        }


def result_from_event(command, op_id, event):
    """Build an ApiResult from a settle event dict (or None -> timeout). Shared by
    both writers; does NOT log (the caller logs at the right granularity)."""
    result = ApiResult(command, op_id)
    if event is None:
        result.timed_out = True
        result.status = "timeout"
        result.detail = "no settle event within budget"
        return result
    result.event = {k: v for k, v in event.items() if k not in _EVENT_META_KEYS}
    result.status = event.get("status")
    result.detail = event.get("detail")
    result.ok = (result.status == STATUS_ACCEPTED)
    return result


def error_result(command, detail):
    """An ApiResult for a call that never reached the wire (bad verb/args/entity)."""
    result = ApiResult(command, "")
    result.status = "invalid"
    result.detail = detail
    result.ok = False
    log.error(f"writer {command}: {detail}")
    return result


# --- Argument coercions used by WRITER_SPEC (API backend) -------------------
def to_float(x):
    return float(x)


def to_bool(x):
    return bool(x)


def to_str(x):
    return str(x)


def to_bit(x):
    """bool -> '1'/'0' for set_schedule_enabled's string 'data' arg."""
    if x:
        return "1"
    return "0"


# verb -> [(service_arg_name, coerce), ...]. The API dispatcher builds the service
# kwargs from this (op_id added). Service arg names match the canonical verb EXCEPT
# set_schedule_enabled (string 'data' of "1"/"0"). Arg counts must match
# capabilities.CAP (guarded by tests/test_writers.py).
WRITER_SPEC = {
    "set_mode":              [("mode", to_str)],
    "set_setpoint":          [("mode", to_str), ("value", to_float)],
    "set_temperature_range": [("min_c", to_float), ("max_c", to_float), ("autoadapt", to_bool)],
    "set_cycle_times":       [("on_minutes", to_float), ("off_minutes", to_float), ("flow", to_float)],
    "set_pump_enabled":      [("enabled", to_bool)],
    "set_pump_state":        [("state", to_str)],
    "set_schedule_enabled":  [("data", to_bit)],
    "set_flow_limiter":      [("limiter", to_str), ("enabled", to_bool), ("limit_gpm", to_float)],
    "upload_schedule":       [("data", to_str)],
    "set_schedule_entry":    [("data", to_str)],
    "clear_schedule_entry":  [("data", to_str)],
    "set_single_event":      [("data", to_str)],
    "clear_single_event":    [("data", to_str)],
    "set_vacation":          [("data", to_str)],
    "refresh_schedule":      [],
    "refresh_single_events": [],
    "clear_vacation":        [],
}


class ApiWriter:
    """API backend -- op_id write services. call(verb, args) dispatches any CAP
    verb via WRITER_SPEC and returns an ApiResult (caller checks result.ok)."""
    def __init__(self, controller_name, timeout=SETTLE_TIMEOUT):
        self.controller_name = controller_name
        self.timeout = timeout

    def service_name(self, verb):
        return f"{self.controller_name}_{verb}"

    def call(self, verb, args):
        """Run a CAP verb with positional args (per capabilities.arg_names(verb))."""
        spec = WRITER_SPEC.get(verb)
        if spec is None:
            return error_result(verb, f"unknown API verb '{verb}'")
        if len(args) != len(spec):
            return error_result(verb, f"{verb} expects {len(spec)} args, got {len(args)}: {args}")

        op_id = next_op_id()
        kwargs = {"op_id": op_id}
        idx = 0
        for arg_name, coerce in spec:
            kwargs[arg_name] = coerce(args[idx])
            idx += 1

        log.info(f"ApiWriter {verb} (op_id={op_id}): {kwargs}")
        service.call("esphome", self.service_name(verb), **kwargs)
        return self.settle_for(verb, op_id)

    def settle_for(self, command, op_id):
        """Await the settle for an ALREADY-ISSUED op_id, then build/log/return."""
        event = await_settle(op_id, self.timeout)
        result = result_from_event(command, op_id, event)
        _log_outcome("ApiWriter", command, f"op_id={op_id}", result, self.timeout)
        return result


class EntityWriter:
    """ENTITY backend -- drives the pump through the SAME HA entities a UI user
    touches (number/switch/select), confirming each via an origin=entity settle.
    call(key, args) accepts an API verb (decomposed to entity writes) or a raw
    'domain,leaf' ref (written directly). Same ApiResult shape as ApiWriter.

    v1 covers the single-entity verbs (set_mode/set_setpoint/set_pump_enabled/
    set_pump_state/set_schedule_enabled) plus raw entity-ref writes. The multi-
    entity atomic verbs (set_temperature_range/set_cycle_times) and the API-only
    verbs have no entity path here yet -> error_result.
    """
    def __init__(self, controller_name, timeout=SETTLE_TIMEOUT):
        self.controller_name = controller_name
        self.timeout = timeout

    def call(self, key, args):
        if "," in key:                      # raw entity-ref write
            return self.write_ref_action(key, args[0])
        if key == "set_mode":
            return self.write_mode(args[0])
        if key == "set_setpoint":
            return self.write_setpoint(args[0], args[1])
        if key == "set_pump_enabled":
            return self.write_pump_enabled(args[0])
        if key == "set_schedule_enabled":
            return self.write_schedule_enabled(args[0])
        if key == "set_pump_state":
            return self.write_pump_state(args[0])
        return error_result(key, f"'{key}' has no entity-backend path "
                                 f"(API-only, or multi-entity not yet implemented)")

    # --- one confirmed entity write ---
    def write_settled(self, ref, value, command):
        """Write one entity (via entities.set_value) and await its origin=entity
        settle. Returns an ApiResult labeled `command`."""
        if not entities.set_value(ref, value, self.controller_name):
            return error_result(command, f"entity write failed for '{ref}' (missing/read-only?)")
        event = await_settle_entity(timeout=self.timeout)
        result = result_from_event(command, "", event)
        _log_outcome("EntityWriter", command, f"{ref}={value}", result, self.timeout)
        return result

    def display_for(self, ref, native_value):
        """Convert a NATIVE numeric value to the entity's current display unit
        (HA converts it back to native on write). Returns (ok, value)."""
        unit = entities.get_unit(ref, self.controller_name)
        ok, value, detail = units.to_display(ref, float(native_value), unit)
        if not ok:
            log.warning(f"EntityWriter: {detail}")
            return (False, None)
        return (True, value)

    # --- semantic verbs -> entity writes ---
    def write_mode(self, machine):
        display = entities.MODE_MACHINE_TO_DISPLAY.get(machine)
        if display is None:
            return error_result("set_mode", f"unknown mode '{machine}'")
        return self.write_settled(entities.MODE_ENTITY, display, "set_mode")

    def write_setpoint(self, machine, native_value):
        ref = entities.SETPOINT_REF_BY_MODE.get(machine)
        if ref is None:
            return error_result("set_setpoint", f"mode '{machine}' has no scalar setpoint entity")
        ok, value = self.display_for(ref, native_value)
        if not ok:
            return error_result("set_setpoint", f"could not convert {native_value} for {ref}")
        return self.write_settled(ref, value, "set_setpoint")

    def write_pump_enabled(self, enabled):
        value = "off"
        if enabled:
            value = "on"
        return self.write_settled("switch,engage_pump", value, "set_pump_enabled")

    def write_schedule_enabled(self, enabled):
        value = "off"
        if enabled:
            value = "on"
        return self.write_settled("switch,schedule_enabled", value, "set_schedule_enabled")

    def write_pump_state(self, state):
        mapping = entities.RUN_STATE_ENTITY.get(state)
        if mapping is None:
            return error_result("set_pump_state", f"unknown run state '{state}'")
        ref, value = mapping
        return self.write_settled(ref, value, "set_pump_state")

    # --- raw entity-ref write ---
    def write_ref_action(self, ref, value):
        if entities.domain_of(ref) == "number":
            ok, disp = self.display_for(ref, value)
            if not ok:
                return error_result(ref, f"could not convert {value} for {ref}")
            return self.write_settled(ref, disp, ref)
        return self.write_settled(ref, value, ref)


def _log_outcome(who, command, ctx, result, timeout):
    """Shared INFO logging for a settle outcome (severity judgment is the caller's,
    e.g. the engine warns on a failed settle assertion)."""
    if result.timed_out:
        log.info(f"{who} {command} ({ctx}): timeout after {timeout}s")
    elif result.ok:
        log.info(f"{who} {command} ({ctx}): accepted")
    else:
        log.info(f"{who} {command} ({ctx}): {result.status} -- {result.detail}")
