"""Settled writes to the ALPHA HWR pump via the component's op_id API (issue #92).

Every write here goes through one of the component's programmatic services
(esphome.<controller>_pump_set_*), each of which carries an `op_id` and emits
exactly one terminal `esphome.alpha_hwr_write_settled` event correlated by that
op_id. We wait for that event and turn its `status` into a plain pass/fail the
caller can read off the returned ApiResult -- no arbitrary settle delays, no
read-back verification. "Settled" means the pump has confirmed the outcome, so
the caller is also free to issue the NEXT write immediately (the component
queues writes and runs them strictly one at a time).

Two layers:
  * await_settle()  -- module-level primitive (mirrors recirc_pump.await_settle):
                       wait for one settle event by op_id, or time out.
  * PumpApi         -- one method per service. Each mints an op_id, calls the
                       service, awaits the settle, logs any non-accepted outcome,
                       and returns an ApiResult. This is the SINGLE home for the
                       service names -- when upstream renames them (a release is
                       pending that does), only this file changes.

Race safety: we call the service and THEN wait. That is safe even for a locally
rejected command because every settle event -- success or rejection -- originates
in the component on the ESP32 and travels back over the ESPHome API connection,
so it cannot arrive in the same event-loop tick as the (fire-and-return) service
call. HA's single-threaded loop can't process the event until we yield inside
await_settle(). Same reasoning the production recirc_pump.await_settle() relies on.
"""

import json

# The component's terminal settle event (fixed name, not controller-prefixed).
SETTLE_EVENT = "esphome.alpha_hwr_write_settled"

# How long to wait for a settle event, in seconds. The docs say 30s covers
# everything except refresh_single_events (which this harness does not use), so
# a real lost command comes back as an authoritative 'timeout' rather than a
# silent client-side give-up.
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
    (engage/schedule) can fan out internally, so prefer the op_id pump_set_state
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


class PumpApi:
    """Typed wrapper over the component's op_id write services.

    Construct once with the install's controller_name (read from app_config in
    __init__, the only place pyscript exposes it) and reuse. Every method returns
    an ApiResult; the caller just checks result.ok.
    """
    def __init__(self, controller_name, timeout=SETTLE_TIMEOUT):
        self.controller_name = controller_name
        self.timeout = timeout

    def service_name(self, suffix):
        """Full ESPHome service name for this install (controller prefix + suffix)."""
        return f"{self.controller_name}_{suffix}"

    def settle_for(self, command, op_id):
        """Await the settle event for an ALREADY-ISSUED op_id, then build, log, and
        return the ApiResult. Split from the service call so each method can issue
        its own service.call with explicit named kwargs -- pyscript's AST
        interpreter does not implement call-site **dict unpacking, so a generic
        'call with an args dict' dispatcher is not safe here.
        """
        result = ApiResult(command, op_id)
        event = await_settle(op_id, self.timeout)
        if event is None:
            result.timed_out = True
            result.status = "timeout"
            result.detail = f"no settle event within {self.timeout}s"
            log.warning(f"PumpApi {command} (op_id={op_id}): TIMEOUT after {self.timeout}s")
            return result

        result.event = dict([(k, v) for k, v in event.items() if k not in _EVENT_META_KEYS])
        result.status = event.get("status")
        result.detail = event.get("detail")
        result.ok = (result.status == STATUS_ACCEPTED)
        if result.ok:
            log.info(f"PumpApi {command} (op_id={op_id}): accepted")
        else:
            log.warning(f"PumpApi {command} (op_id={op_id}): status={result.status} "
                        f"detail={result.detail} "
                        f"event={json.dumps(result.event, sort_keys=True, default=str)}")
        return result

    # --- Control services (carry the 'pump_' prefix) -----------------------

    def set_mode(self, mode):
        """Switch control mode. mode is the machine identifier (e.g. 'constant_speed')."""
        op_id = next_op_id()
        log.info(f"PumpApi set_mode (op_id={op_id}): mode={mode}")
        service.call("esphome", self.service_name("pump_set_mode"),
                     mode=mode, op_id=op_id)
        return self.settle_for("set_mode", op_id)

    def set_setpoint(self, mode, value):
        """Set the setpoint for a scalar-setpoint mode (constant speed/flow/pressure,
        proportional pressure). mode is the machine identifier."""
        op_id = next_op_id()
        log.info(f"PumpApi set_setpoint (op_id={op_id}): mode={mode} value={value}")
        service.call("esphome", self.service_name("pump_set_setpoint"),
                     mode=mode, value=float(value), op_id=op_id)
        return self.settle_for("set_setpoint", op_id)

    def set_pump_enabled(self, enabled):
        """Enable (run in configured mode) or disable (stop) the pump."""
        op_id = next_op_id()
        log.info(f"PumpApi set_pump_enabled (op_id={op_id}): enabled={enabled}")
        service.call("esphome", self.service_name("pump_set_enabled"),
                     enabled=bool(enabled), op_id=op_id)
        return self.settle_for("set_pump_enabled", op_id)

    def set_pump_state(self, pump_state):
        """Set the coupled run state: 'off' | 'engaged' | 'scheduled'."""
        op_id = next_op_id()
        log.info(f"PumpApi set_pump_state (op_id={op_id}): state={pump_state}")
        service.call("esphome", self.service_name("pump_set_state"),
                     state=pump_state, op_id=op_id)
        return self.settle_for("set_pump_state", op_id)

    def set_temperature_range(self, min_c, max_c, autoadapt):
        """Set the Temperature Control range and AutoAdapt in one atomic write --
        no min>max intermediate state to order around."""
        op_id = next_op_id()
        log.info(f"PumpApi set_temperature_range (op_id={op_id}): "
                 f"min_c={min_c} max_c={max_c} autoadapt={autoadapt}")
        service.call("esphome", self.service_name("pump_set_temperature_range"),
                     min_c=float(min_c), max_c=float(max_c),
                     autoadapt=bool(autoadapt), op_id=op_id)
        return self.settle_for("set_temperature_range", op_id)

    def set_cycle_times(self, on_minutes, off_minutes, flow):
        """Set Cycle Time on/off minutes and flow in one atomic write. A 0 in any
        field means 'keep the pump's existing value' (component sentinel)."""
        op_id = next_op_id()
        log.info(f"PumpApi set_cycle_times (op_id={op_id}): "
                 f"on={on_minutes} off={off_minutes} flow={flow}")
        service.call("esphome", self.service_name("pump_set_cycle_times"),
                     on_minutes=float(on_minutes), off_minutes=float(off_minutes),
                     flow=float(flow), op_id=op_id)
        return self.settle_for("set_cycle_times", op_id)

    # --- Schedule services (NO 'pump_' prefix; take data='1'/'0') -----------

    def set_schedule_enabled(self, enabled):
        """Enable/disable the pump's internal weekly schedule. Note the different
        service shape: no 'pump_' prefix and a string 'data' arg, not a bool."""
        op_id = next_op_id()
        data = "1" if enabled else "0"
        log.info(f"PumpApi set_schedule_enabled (op_id={op_id}): data={data}")
        service.call("esphome", self.service_name("set_schedule_enabled"),
                     data=data, op_id=op_id)
        return self.settle_for("set_schedule_enabled", op_id)
