"""Settled writes to the ALPHA HWR pump via the component's op_id API (issue #92).

Every write here goes through one of the component's programmatic services
(esphome.<controller>_<verb>), each of which carries an `op_id` and emits exactly
one terminal `esphome.alpha_hwr_write_settled` event correlated by that op_id. We
wait for that event and turn its `status` into a plain pass/fail the caller can
read off the returned ApiResult -- no arbitrary settle delays, no read-back
verification. "Settled" means the pump has confirmed the outcome, so the caller
is also free to issue the NEXT write immediately (the component queues writes and
runs them strictly one at a time).

Two layers:
  * await_settle()  -- module-level primitive (mirrors recirc_pump.await_settle):
                       wait for one settle event by op_id, or time out.
  * ApiWriter       -- data-driven wrapper: call(verb, args) dispatches any CAP
                       verb via WRITER_SPEC (which names the service args and their
                       coercions), mints an op_id, calls the service, awaits the
                       settle, logs any non-accepted outcome, returns an ApiResult.

Race safety: we call the service and THEN wait. That is safe even for a locally
rejected command because every settle event -- success or rejection -- originates
in the component on the ESP32 and travels back over the ESPHome API connection,
so it cannot arrive in the same event-loop tick as the (fire-and-return) service
call. HA's single-threaded loop can't process the event until we yield inside
await_settle(). Same reasoning the production recirc_pump.await_settle() relies on.
"""

# The component's terminal settle event (fixed name, not controller-prefixed).
SETTLE_EVENT = "esphome.alpha_hwr_write_settled"

# How long to wait for a settle event, in seconds. The docs say 30s covers
# everything except refresh_single_events, so a real lost command comes back as
# an authoritative 'timeout' rather than a silent client-side give-up.
SETTLE_TIMEOUT = 30

# The one settle status that means "the pump confirmed exactly what we asked
# for". Everything else (clamped / rejected / invalid / timeout / superseded) is
# surfaced on the ApiResult and logged, and the caller decides what it means in
# context (e.g. a clamp on a restore is a problem; a clamp on a deliberate
# out-of-range test is the expected result).
STATUS_ACCEPTED = "accepted"

# Keys in the task.wait_until result that are trigger/event plumbing, not part of
# the pump's settle payload -- dropped before we keep/log the event.
_EVENT_META_KEYS = ("trigger_type", "event_type", "context")


class ApiState:
    """Cross-call counter for op_id uniqueness (the recirc_pump Status pattern).

    Reset on reload, which is fine: a reload also cancels any in-flight settle
    wait, so a reused number can never collide with a still-pending operation.
    """
    op_seq = 0


def next_op_id():
    """Return a process-unique op_id string for one write."""
    ApiState.op_seq += 1
    return f"hwrtest_{ApiState.op_seq}"


def await_settle(op_id, timeout=SETTLE_TIMEOUT):
    """Wait for the settle event matching op_id.

    Returns the flattened task.wait_until result dict (which carries the event's
    fields -- status, command, value, requested_*, etc. -- directly as keys), or
    None if the wait times out. No register-before-send race: the event is
    causally downstream of the service call (it needs the BLE round-trip), so it
    cannot fire before we start waiting.
    """
    result = task.wait_until(
        event_trigger=[SETTLE_EVENT, f"op_id == '{op_id}'"],
        timeout=timeout,
    )
    if result["trigger_type"] == "timeout":
        return None
    return result


def await_settle_entity(command=None, timeout=SETTLE_TIMEOUT):
    """Wait for the next ENTITY-origin settle event -- i.e. the settle for a write
    made through an HA entity (number/switch/select), which carries NO op_id.

    Entity writes route through the same write layer as the op_id services and
    emit the same terminal event, tagged origin="entity" with an empty op_id
    (component issue #92). With no op_id to correlate, this matches "the next
    origin==entity event", which is only valid when we are the SOLE writer AND
    serialize writes (await each before issuing the next). Pass `command`
    (e.g. 'set_setpoint', 'set_mode') to also filter by write command for a second
    layer of confidence.

    Returns the flattened task.wait_until result dict, or None on timeout. Same
    race-safety as await_settle(): the event round-trips from the ESP32, so it
    cannot arrive before this wait registers on HA's single-threaded loop. Best
    for single-value writes (setpoints, mode); coupled run-state entity writes
    (engage/schedule) can fan out internally, so prefer the op_id set_pump_state
    service there.
    """
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
    the full picture for logging or for a caller that wants to treat clamped etc.
    specially. event is the pump's settle payload with plumbing keys removed, so
    it holds the requested_* echoes and settled values side by side.
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


# --- Argument coercions used by WRITER_SPEC --------------------------------
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


# verb -> [(service_arg_name, coerce), ...]. The dispatcher builds the service
# kwargs from this (op_id added automatically). Service arg names match the
# canonical verb EXCEPT set_schedule_enabled, whose service takes a string 'data'
# of "1"/"0" rather than a bool. Arg counts here must match capabilities.CAP
# (guarded by tests/test_writers.py).
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
    """Data-driven wrapper over the component's op_id write services (the API
    backend). Construct once with the install's controller_name (read from
    app_config in __init__, the only place pyscript exposes it) and reuse.
    call(verb, args) dispatches any CAP verb; every call returns an ApiResult, so
    the caller just checks result.ok. restore and the engine both go through call().
    """
    def __init__(self, controller_name, timeout=SETTLE_TIMEOUT):
        self.controller_name = controller_name
        self.timeout = timeout

    def service_name(self, verb):
        """Full esphome service name for a verb on this install."""
        return f"{self.controller_name}_{verb}"

    def call(self, verb, args):
        """Run a CAP verb with positional args (per capabilities.arg_names(verb)),
        returning an ApiResult. Builds the service kwargs from WRITER_SPEC (op_id
        added), calls the service, and awaits the settle. Unknown verb or wrong arg
        count returns an error result without touching the wire."""
        spec = WRITER_SPEC.get(verb)
        if spec is None:
            return self.error_result(verb, f"unknown API verb '{verb}'")
        if len(args) != len(spec):
            return self.error_result(verb, f"{verb} expects {len(spec)} args, got {len(args)}: {args}")

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
        """Await the settle for an ALREADY-ISSUED op_id, then build/log/return the
        ApiResult."""
        result = ApiResult(command, op_id)
        event = await_settle(op_id, self.timeout)
        if event is None:
            result.timed_out = True
            result.status = "timeout"
            result.detail = f"no settle event within {self.timeout}s"
            log.info(f"ApiWriter {command} (op_id={op_id}): timeout after {self.timeout}s")
            return result

        result.event = {k: v for k, v in event.items() if k not in _EVENT_META_KEYS}
        result.status = event.get("status")
        result.detail = event.get("detail")
        result.ok = (result.status == STATUS_ACCEPTED)
        # Report the outcome only, always at INFO. Whether a non-accepted status
        # is a PROBLEM is the caller's judgment -- an expected clamp/reject in a
        # negative test is fine -- so the caller (engine settle-assert, or restore)
        # is what warns. The full settle payload is on result.event for the caller.
        if result.ok:
            log.info(f"ApiWriter {command} (op_id={op_id}): accepted")
        else:
            log.info(f"ApiWriter {command} (op_id={op_id}): {result.status} -- {result.detail}")
        return result

    def error_result(self, command, detail):
        """An ApiResult for a call that never reached the wire (bad verb/args)."""
        result = ApiResult(command, "")
        result.status = "invalid"
        result.detail = detail
        result.ok = False
        log.error(f"ApiWriter {command}: {detail}")
        return result
