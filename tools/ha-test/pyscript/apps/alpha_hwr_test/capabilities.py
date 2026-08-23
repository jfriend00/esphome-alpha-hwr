"""Capability map, settle-field map, statuses, and known entities.

Sourced from the component's api_bridge.cpp + write_operation_service.cpp on the
upstream TRUNK (github eman/esphome-alpha-hwr, fetched 2026-08-22). Re-verify
against the trunk when the component changes shape.

Pure data + tiny accessors, no pyscript deps -> importable/checkable under host
CPython (see tests/test_capabilities.py).

Three roles:
  * CAP           -- which write verbs exist, their args, and which backends can
                     run them (drives dispatch + skip-with-reason + "both").
  * SETTLE_FIELDS -- the settled fields each command reports on its write_settled
                     event, so a `settle:` assertion can be validated + resolved.
  * KNOWN_ENTITIES / WRITABLE_DOMAINS -- entity awareness for validation.

Note on units: action inputs and settle values are NATIVE (RPM, m3/h, C, min);
entity reads are in the entity's display unit. Assertions in a `settle:` list
read the fields below; assertions in a test's `expect:` read entities.
"""

# --- Backends ---------------------------------------------------------------
API = "api"
ENTITY = "entity"

# verb -> (backends tuple, positional arg names).  op_id is added by the writer,
# not listed here.  Dual verbs also have an EntityWriter decomposition; api-only
# verbs do not.  Arg names are the HARNESS-level names (the writer maps them to
# the actual service args, e.g. set_schedule_enabled's bool -> data "1"/"0").
# Entity-ref actions ({"number,cycle_flow": 0.9}) are detected by the comma in
# the action key and need no entry here.
CAP = {
    # --- dual: API service + EntityWriter decomposition ---
    "set_mode":              ((API, ENTITY), ["mode"]),
    "set_setpoint":          ((API, ENTITY), ["mode", "value"]),
    "set_temperature_range": ((API, ENTITY), ["min_c", "max_c", "autoadapt"]),
    "set_cycle_times":       ((API, ENTITY), ["on_minutes", "off_minutes", "flow"]),
    "set_pump_enabled":      ((API, ENTITY), ["enabled"]),
    "set_pump_state":        ((API, ENTITY), ["state"]),
    "set_schedule_enabled":  ((API, ENTITY), ["enabled"]),
    # --- api-only: no entity form ---
    "set_flow_limiter":      ((API,), ["limiter", "enabled", "limit_gpm"]),
    "upload_schedule":       ((API,), ["data"]),
    "set_schedule_entry":    ((API,), ["data"]),
    "clear_schedule_entry":  ((API,), ["data"]),
    "refresh_schedule":      ((API,), []),
    "set_single_event":      ((API,), ["data"]),
    "clear_single_event":    ((API,), ["data"]),
    "refresh_single_events": ((API,), []),
    "set_vacation":          ((API,), ["data"]),
    "clear_vacation":        ((API,), []),
}

# NOT registered services (present in write_command_to_string, but no
# register_service): set_remote_mode (reachable only via the remote_mode entity
# switch) and set_clock (internal time sync). Kept here as a note so nobody adds
# them to CAP expecting a callable service.
NON_SERVICE_COMMANDS = ("set_remote_mode", "set_clock")


# --- Settle statuses (write_status_to_string) -------------------------------
# The complete set a write_settled event's `status` can carry. `partial` is
# upload_schedule only. Used to validate a `status ==`/`status in ...` assertion.
SETTLE_STATUSES = ("accepted", "clamped", "rejected", "timeout",
                   "invalid", "superseded", "partial")


# --- Settle fields ----------------------------------------------------------
# On EVERY write_settled event.
SETTLE_UNIVERSAL = ("op_id", "command", "status", "detail", "origin", "node", "seq")

# All schedule/vacation/single-event commands share api_bridge's default branch,
# which emits any subset of these. Listed as a superset -- enough to validate
# that a field COULD appear for such a command.
_SCHEDULE_FIELDS = ["layer", "day", "day_name", "slot", "event_type",
                    "begin", "end", "begin_ts", "end_ts", "enabled",
                    "event_count", "layers_written", "layers_skipped", "schedule_hash"]

# command -> command-specific settled fields (beyond SETTLE_UNIVERSAL and the
# requested_* echoes). From fire_write_settled()'s switch.
SETTLE_FIELDS = {
    "set_pump_enabled":      ["enabled", "mode", "value"],
    "set_mode":              ["mode"],
    "set_setpoint":          ["mode", "value", "enabled"],
    "set_temperature_range": ["temp_min", "temp_max", "autoadapt"],
    "set_cycle_times":       ["on_minutes", "off_minutes", "flow"],
    "set_flow_limiter":      ["limiter", "limiter_enabled", "limit_gpm",
                             "requested_limiter_enabled", "requested_limit_gpm"],
    "set_pump_state":        ["enabled", "schedule_enabled", "state"],
    "upload_schedule":       list(_SCHEDULE_FIELDS),
    "set_schedule_entry":    list(_SCHEDULE_FIELDS),
    "clear_schedule_entry":  list(_SCHEDULE_FIELDS),
    "set_schedule_enabled":  list(_SCHEDULE_FIELDS),
    "refresh_schedule":      list(_SCHEDULE_FIELDS),
    "set_single_event":      list(_SCHEDULE_FIELDS),
    "clear_single_event":    list(_SCHEDULE_FIELDS),
    "refresh_single_events": list(_SCHEDULE_FIELDS),
    "set_vacation":          list(_SCHEDULE_FIELDS),
    "clear_vacation":        list(_SCHEDULE_FIELDS),
}


# --- Entities ---------------------------------------------------------------
# Domains that can be written by a raw entity-ref action ({"<domain,leaf>": v}).
WRITABLE_DOMAINS = ("switch", "number", "select")

# Curated set of refs known to exist on John's build (his snapshot registry +
# the verification/status sensors + the read-only schedule entities). Advisory:
# assertions resolve ANY ref generically, and startup validation checks the refs
# the loaded tests actually use -- this list is for discoverability and to catch
# an obvious typo early. Extend as entities are added; not meant to be exhaustive.
KNOWN_ENTITIES = {
    # writable control
    "select,pump_control_mode",
    "switch,engage_pump",
    "switch,schedule_enabled",
    "switch,remote_mode",
    "switch,temperature_autoadapt",
    "number,constant_speed_setpoint",
    "number,constant_flow_setpoint",
    "number,constant_pressure_setpoint",
    "number,proportional_pressure_setpoint",
    "number,temperature_range_min",
    "number,temperature_range_max",
    "number,cycle_time_on",
    "number,cycle_time_off",
    "number,cycle_flow",
    # read-only: run/verification
    "binary_sensor,pump_motor_active",
    "binary_sensor,pump_ready",
    "sensor,motor_speed",
    "sensor,flow_rate",
    "sensor,pump_run_state",
    "sensor,pump_link_status",
    # read-only: schedule / vacation
    "sensor,schedule_layer_0",
    "sensor,schedule_layer_1",
    "sensor,schedule_layer_2",
    "sensor,schedule_layer_3",
    "sensor,schedule_layer_4",
    "binary_sensor,schedule_stalled",
    "sensor,schedule_hash",
    "sensor,single_events",
    "sensor,vacation",
}


# --- Accessors --------------------------------------------------------------
def is_verb(action_key):
    """True if the action key is an API verb (in CAP), False if an entity ref."""
    return "," not in action_key


def backends(verb):
    """Backends that can run `verb`, or () if unknown."""
    entry = CAP.get(verb)
    if entry is None:
        return ()
    return entry[0]


def arg_names(verb):
    """Positional arg names for `verb` (excluding op_id), or None if unknown."""
    entry = CAP.get(verb)
    if entry is None:
        return None
    return entry[1]


def supports(verb, backend):
    """True if `verb` can run on `backend`."""
    return backend in backends(verb)


def domain_of_ref(ref):
    """Domain part of a 'domain,leaf' ref."""
    return ref.split(",", 1)[0].strip()


def is_writable_ref(ref):
    """True if a raw entity-ref action may write this ref (writable domain)."""
    return domain_of_ref(ref) in WRITABLE_DOMAINS


def valid_settle_field(command, field):
    """True if `field` can appear on `command`'s write_settled event -- a
    universal field, a command-specific field, or a requested_* echo."""
    if field in SETTLE_UNIVERSAL:
        return True
    if field.startswith("requested_"):
        return True
    return field in SETTLE_FIELDS.get(command, [])
