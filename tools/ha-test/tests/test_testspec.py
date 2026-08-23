"""Host tests for testspec.py -- parse_action, expand_conditions, validate.

testspec imports capabilities + assertions via a dual-import, so with the app dir
on sys.path it loads standalone under CPython.
"""

import os
import sys

_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "pyscript", "apps", "alpha_hwr_test")
sys.path.insert(0, _APP)
import testspec as T  # noqa: E402

passed = 0
failed = 0


def ok(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL: {label}")


def has(problems, substr):
    for p in problems:
        if substr in p["error"]:
            return True
    return False


# --- parse_action ---
verb, args, settle, err = T.parse_action({"set_setpoint": ["constant_speed", 2000]})
ok(verb == "set_setpoint" and args == ["constant_speed", 2000] and settle == [] and err is None,
   "parse_action list args")

verb, args, settle, err = T.parse_action({"set_mode": "constant_speed"})
ok(verb == "set_mode" and args == ["constant_speed"] and err is None, "parse_action scalar arg")

verb, args, settle, err = T.parse_action(
    {"set_setpoint": ["constant_speed", 9000], "settle": ["status == clamped"]})
ok(settle == ["status == clamped"], "parse_action keeps settle list")

verb, args, settle, err = T.parse_action({"number,cycle_flow": 0.9})
ok(verb == "number,cycle_flow" and args == [0.9] and err is None, "parse_action entity ref")

verb, args, settle, err = T.parse_action({"a": 1, "b": 2})
ok(verb is None and err is not None, "parse_action rejects two command keys")

# --- expand_conditions ---
flat, errs = T.expand_conditions(
    ["@pump_running", "x,y == on"],
    {"pump_running": ["a,b == on", "c,d > 0"]})
ok(flat == ["a,b == on", "c,d > 0", "x,y == on"] and errs == [], "expand: named + inline")

flat, errs = T.expand_conditions(["@nope"], {})
ok(flat == [] and "unknown condition '@nope'" in errs, "expand: unknown ref reported")

flat, errs = T.expand_conditions(["@a"], {"a": ["@b"], "b": ["x,y == 1"]})
ok(flat == ["x,y == 1"] and errs == [], "expand: nested")

flat, errs = T.expand_conditions(["@a"], {"a": ["@b"], "b": ["@a"]})
ok(len(errs) > 0, "expand: cycle reported (not infinite)")

# --- validate: clean table ---
clean = [{"name": "t",
          "actions": [{"set_setpoint": ["constant_speed", 2000]}],
          "expect": ["sensor,motor_speed > 0 within 5s"]}]
ok(T.validate(clean, {}) == [], "validate: clean table has no problems")

# --- validate: each failure mode ---
ok(has(T.validate([{"name": "t", "actions": [{"set_bogus": [1]}]}], {}), "unknown API verb"),
   "validate: unknown verb")

ok(has(T.validate([{"name": "t", "actions": [{"set_setpoint": ["constant_speed"]}]}], {}), "expects 2 args"),
   "validate: wrong arg count")

ok(has(T.validate([{"name": "t", "actions": [{"sensor,motor_speed": 5}]}], {}), "not a writable domain"),
   "validate: entity-ref non-writable")

ok(T.validate([{"name": "t", "actions": [{"number,cycle_flow": 0.9}]}], {}) == [],
   "validate: writable entity-ref ok")

ok(has(T.validate([{"name": "t", "actions": [{"set_mode": "constant_speed",
                                              "settle": ["sensor,x == on"]}]}], {}), "entity ref"),
   "validate: settle assertion that is an entity ref")

ok(has(T.validate([{"name": "t", "actions": [{"set_setpoint": ["constant_speed", 2000],
                                              "settle": ["temp_min == 40"]}]}], {}), "not a settled field"),
   "validate: invalid settle field for command")

ok(T.validate([{"name": "t", "actions": [{"set_setpoint": ["constant_speed", 2000],
                                          "settle": ["value ~= 1650"]}]}], {}) == [],
   "validate: valid settle field")

ok(has(T.validate([{"name": "t", "actions": [{"set_mode": "constant_speed",
                                              "settle": ["value foo bar"]}]}], {}), "unparseable settle"),
   "validate: unparseable settle assertion")

ok(has(T.validate([{"name": "t", "expect": ["@nope"]}], {}), "unknown condition"),
   "validate: unknown @condition")

ok(has(T.validate([{"name": "t", "backend": "bogus", "actions": []}], {}), "invalid backend"),
   "validate: bad backend")

ok(has(T.validate([{"name": "t", "expect": ["status == accepted"]}], {}), "entity ref"),
   "validate: expect must be an entity ref")

ok(has(T.validate([{"actions": []}], {}), "no 'name'"),
   "validate: missing name")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
