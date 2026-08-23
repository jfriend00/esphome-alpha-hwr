"""Host tests for capabilities.py -- the capability map, settle-field map, and
accessors. Pure data + pure functions, so this runs under plain CPython.

Run directly (`python test_capabilities.py`) or via run_tests.py.
"""

import os
import sys

_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "pyscript", "apps", "alpha_hwr_test")
sys.path.insert(0, _APP)
import capabilities as C  # noqa: E402

passed = 0
failed = 0


def ok(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL: {label}")


# --- internal consistency ---
for verb in C.CAP:
    entry = C.CAP[verb]
    ok(isinstance(entry, tuple) and len(entry) == 2, f"CAP[{verb}] shape")
    ok(len(entry[0]) >= 1 and all(b in (C.API, C.ENTITY) for b in entry[0]),
       f"CAP[{verb}] backends valid")
    ok(verb in C.SETTLE_FIELDS, f"{verb} has a SETTLE_FIELDS entry")

# every non-service command is deliberately absent from CAP
for cmd in C.NON_SERVICE_COMMANDS:
    ok(cmd not in C.CAP, f"{cmd} not a callable service")

ok(len(C.SETTLE_STATUSES) == 7, "seven settle statuses")

# --- accessors ---
ok(C.is_verb("set_mode"), "is_verb: API verb")
ok(not C.is_verb("number,cycle_flow"), "is_verb: entity ref -> False")

ok(C.backends("set_mode") == (C.API, C.ENTITY), "backends: dual")
ok(C.backends("upload_schedule") == (C.API,), "backends: api-only")
ok(C.backends("nope") == (), "backends: unknown -> ()")

ok(C.arg_names("set_setpoint") == ["mode", "value"], "arg_names: setpoint")
ok(C.arg_names("set_flow_limiter") == ["limiter", "enabled", "limit_gpm"], "arg_names: flow limiter")
ok(C.arg_names("nope") is None, "arg_names: unknown -> None")

ok(C.supports("set_mode", C.ENTITY), "supports: dual on entity")
ok(not C.supports("upload_schedule", C.ENTITY), "supports: api-only not on entity")
ok(C.supports("upload_schedule", C.API), "supports: api-only on api")

ok(C.is_writable_ref("number,cycle_flow"), "writable: number")
ok(C.is_writable_ref("switch,engage_pump"), "writable: switch")
ok(not C.is_writable_ref("sensor,motor_speed"), "writable: sensor -> False")
ok(not C.is_writable_ref("binary_sensor,pump_ready"), "writable: binary_sensor -> False")

ok(C.domain_of_ref("sensor,motor_speed") == "sensor", "domain_of_ref")

# --- valid_settle_field ---
ok(C.valid_settle_field("set_setpoint", "value"), "settle field: command-specific")
ok(C.valid_settle_field("set_setpoint", "status"), "settle field: universal")
ok(C.valid_settle_field("set_setpoint", "requested_value"), "settle field: requested_ echo")
ok(C.valid_settle_field("set_temperature_range", "temp_min"), "settle field: temp_min")
ok(not C.valid_settle_field("set_setpoint", "temp_min"), "settle field: wrong command -> False")
ok(not C.valid_settle_field("set_mode", "bogus"), "settle field: unknown -> False")

# --- known entities are well-formed refs ---
for ref in C.KNOWN_ENTITIES:
    ok("," in ref and len(ref.split(",", 1)) == 2 and ref.split(",", 1)[1] != "",
       f"KNOWN_ENTITIES well-formed: {ref}")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
