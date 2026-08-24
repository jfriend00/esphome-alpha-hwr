"""Host tests for the pure data/logic used by the ENTITY write path:
the mode/setpoint/run-state decomposition maps (entities.py) and the pure paths
of unit conversion (units.py). No pyscript, no HA.

The actual gal/min<->m3/h and F<->C conversions need HA's converter module and
are exercised on-device; here we only check the passthrough / missing-unit paths
(which don't import homeassistant).
"""

import os
import sys

_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "pyscript", "apps", "alpha_hwr_test")
sys.path.insert(0, _APP)
import entities as E       # noqa: E402
import units as U          # noqa: E402
import capabilities as C   # noqa: E402

passed = 0
failed = 0


def ok(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL: {label}")


# --- MODE_MACHINE_TO_DISPLAY is the exact inverse of MODE_DISPLAY_TO_MACHINE ---
ok(len(E.MODE_MACHINE_TO_DISPLAY) == len(E.MODE_DISPLAY_TO_MACHINE),
   "mode maps same length")
for display, machine in E.MODE_DISPLAY_TO_MACHINE.items():
    ok(E.MODE_MACHINE_TO_DISPLAY.get(machine) == display,
       f"mode reverse: {machine} -> {display}")

# --- SETPOINT_REF_BY_MODE: valid machine keys, writable number refs ---
_machines = set(E.MODE_DISPLAY_TO_MACHINE.values())
for machine, ref in E.SETPOINT_REF_BY_MODE.items():
    ok(machine in _machines, f"setpoint mode key valid: {machine}")
    ok(E.domain_of(ref) == "number", f"setpoint ref is a number: {ref}")
    ok(C.is_writable_ref(ref), f"setpoint ref writable: {ref}")

# --- RUN_STATE_ENTITY: the three settable states -> a writable switch write ---
ok(set(E.RUN_STATE_ENTITY.keys()) == {"off", "engaged", "scheduled"},
   "run-state keys are off/engaged/scheduled")
for state, pair in E.RUN_STATE_ENTITY.items():
    ok(len(pair) == 2, f"run-state {state} is (ref, value)")
    ref, value = pair
    ok(C.is_writable_ref(ref), f"run-state {state} ref writable: {ref}")
    ok(value in ("on", "off"), f"run-state {state} value on/off: {value}")

# --- units.to_display pure paths (no HA import) ---
okc, val, _ = U.to_display("number,constant_speed_setpoint", 2000.0, "RPM")
ok(okc and val == 2000.0, "to_display: non-convertible ref passes through")

okc, val, _ = U.to_display("number,temperature_range_min", 40.0, "°C")
ok(okc and val == 40.0, "to_display: display==native passes through")

okc, val, _ = U.to_display("number,temperature_range_min", 40.0, None)
ok(not okc, "to_display: missing display unit -> not ok")

# --- units.to_native pure paths (mirror) ---
okc, val, _ = U.to_native("number,constant_speed_setpoint", 2000.0, "RPM")
ok(okc and val == 2000.0, "to_native: non-convertible ref passes through")

okc, val, _ = U.to_native("number,cycle_flow", 0.9, "m³/h")
ok(okc and val == 0.9, "to_native: unit==native passes through")

okc, val, _ = U.to_native("number,cycle_flow", 0.9, None)
ok(not okc, "to_native: missing unit -> not ok")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
