"""Host tests for the pure parts of settle.py.

settle.py imports only `json` at module level and touches pyscript globals
(log/service/task) only inside method bodies, so it imports and instantiates fine
under CPython. We exercise service_name() and the WRITER_SPEC table (the arg
coercions and their agreement with capabilities.CAP) -- not call(), which hits
the wire and uses log.
"""

import os
import sys

_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "pyscript", "apps", "alpha_hwr_test")
sys.path.insert(0, _APP)
import settle as S       # noqa: E402
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


# --- service_name: canonical verb, controller-prefixed (no per-build remap) ---
w = S.ApiWriter("recirc_controller")
ok(w.service_name("set_setpoint") == "recirc_controller_set_setpoint", "service_name set_setpoint")
ok(w.service_name("set_flow_limiter") == "recirc_controller_set_flow_limiter", "service_name set_flow_limiter")
ok(w.controller_name == "recirc_controller", "controller_name stored")
ok(w.timeout == S.SETTLE_TIMEOUT, "default timeout")

# --- WRITER_SPEC <-> CAP agreement (guards drift between the two tables) ---
ok(set(S.WRITER_SPEC.keys()) == set(C.CAP.keys()),
   "WRITER_SPEC covers exactly the CAP verbs")

for verb in C.CAP:
    spec = S.WRITER_SPEC.get(verb, [])
    cap_args = C.arg_names(verb)
    ok(len(spec) == len(cap_args),
       f"{verb}: WRITER_SPEC arg count {len(spec)} == CAP {len(cap_args)}")
    # every spec entry is (name, callable-coerce)
    for pair in spec:
        ok(len(pair) == 2 and callable(pair[1]), f"{verb}: spec entry shape {pair}")

# --- coercions behave ---
ok(S.to_float("2000") == 2000.0, "to_float")
ok(S.to_bool(1) is True and S.to_bool(0) is False, "to_bool")
ok(S.to_str(5) == "5", "to_str")
ok(S.to_bit(True) == "1" and S.to_bit(False) == "0", "to_bit (schedule_enabled data)")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
