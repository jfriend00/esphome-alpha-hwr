"""Host tests for assertions.py -- the assertion grammar and comparison.

Pure CPython, no pyscript: assertions.py splits into a pure layer
(parse_assertion / compare / check) and one pyscript-only function (evaluate,
which calls task.sleep). We import the module and exercise the pure layer; the
free `task` name is never resolved because we don't call evaluate().

Run directly (`python test_assertions.py`) or via run_tests.py. Exits non-zero
if any case fails.
"""

import os
import sys

# Import assertions.py from the app dir, resolved relative to THIS file so the
# suite runs from any checkout location (no hard-coded absolute path).
_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "pyscript", "apps", "alpha_hwr_test")
sys.path.insert(0, _APP)
import assertions as A  # noqa: E402

passed = 0
failed = 0


def ok(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"  FAIL: {label}")


def comp(text, actual):
    return A.compare(A.parse_assertion(text), actual)


def raises(text):
    try:
        A.parse_assertion(text)
        return False
    except ValueError:
        return True


# --- parse structure ---
a = A.parse_assertion("sensor,motor_speed in 1800..2200 within 15s")
ok(a.left == "sensor,motor_speed" and a.is_ref and a.op == "in"
   and a.spec == ("range", 1800.0, 2200.0) and a.timeout_s == 15.0, "parse range+within")

a = A.parse_assertion("status == clamped")
ok(a.left == "status" and not a.is_ref and a.op == "==" and a.spec == ("scalar", "clamped")
   and a.timeout_s is None, "parse settle field, no timeout")

a = A.parse_assertion("status in accepted,clamped")
ok(a.op == "in" and a.spec == ("set", ["accepted", "clamped"]), "parse set")

a = A.parse_assertion("motor_speed ~= 2000 +/- 5%")
ok(a.spec == ("approx", 2000.0, 5.0, True), "parse approx percent")

a = A.parse_assertion("motor_speed ~= 2000 +/- 50")
ok(a.spec == ("approx", 2000.0, 50.0, False), "parse approx absolute")

a = A.parse_assertion("select,pump_control_mode == 'Constant Speed'")
ok(a.spec == ("scalar", "Constant Speed"), "parse quoted value with space")

a = A.parse_assertion("event_count === 3")
ok(a.op == "===" and a.spec == ("scalar", "3"), "parse exact")

# --- compare: == default tolerance / strings ---
ok(comp("x == 40", "40"), "== exact number")
ok(comp("x == 2.0", "1.9999999"), "== absorbs float noise (1e-3)")
ok(not comp("x == 2.0", "1.998"), "== rejects beyond default tol")
ok(comp("x == on", "on"), "== string match")
ok(not comp("x == on", "off"), "== string mismatch")

# --- compare: === exact ---
ok(comp("x === 3", "3"), "=== exact match")
ok(not comp("x === 3", "3.0000001"), "=== rejects tiny diff")

# --- compare: ~= absolute / percent ---
ok(comp("x ~= 2000 +/- 50", "1950"), "~= abs low edge")
ok(not comp("x ~= 2000 +/- 50", "1949"), "~= abs just outside")
ok(comp("x ~= 2000 +/- 5%", "1900"), "~= pct low edge (100)")
ok(not comp("x ~= 2000 +/- 5%", "1899"), "~= pct just outside")

# --- compare: in range / set ---
ok(comp("x in 1800..2200", "1800"), "in range lo inclusive")
ok(comp("x in 1800..2200", "2200"), "in range hi inclusive")
ok(not comp("x in 1800..2200", "2201"), "in range outside")
ok(comp("status in accepted,clamped", "clamped"), "in set string member")
ok(not comp("status in accepted,clamped", "rejected"), "in set string non-member")
ok(comp("n in 1,2,3", "3"), "in set numeric member")
ok(not comp("n in 1,2,3", "4"), "in set numeric non-member")

# --- compare: numeric ordering ---
ok(comp("x > 0", "5"), "> true")
ok(not comp("x > 0", "0"), "> false at boundary")
ok(comp("x <= 10", "10"), "<= inclusive")

# --- compare: bad / missing values ---
ok(comp("x == unavailable", "unavailable"), "== unavailable matches bad state")
ok(not comp("x in 1..2", "unavailable"), "range not satisfied by unavailable")
ok(not comp("x == on", None), "missing entity fails ==")
ok(not comp("x != on", None), "missing entity: != stays unsatisfied")
ok(comp("x != off", "on"), "!= real value differs -> pass")
ok(not comp("x != on", "on"), "!= equal -> fail")

# --- errors ---
ok(raises("sensor,x foo 3"), "unknown op raises")
ok(raises("sensor,x =="), "missing value raises")
ok(raises("sensor,x in 3"), "in without '..' or ',' raises")
ok(raises("sensor,x > abc"), "non-numeric > raises")
ok(raises("sensor,x ~= 2000 +/- abc"), "bad tolerance raises")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
