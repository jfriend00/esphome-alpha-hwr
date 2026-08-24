"""Test engine: run a validated test table and collect results.

Runtime (pyscript) counterpart to the pure testspec.py. For each test it
dispatches actions through a Writer (ApiWriter for v1), checks each action's
settle result, then evaluates the test's entity expectations. It collects every
test's result rather than stopping the SUITE at the first failure; within a test,
a failed action stops that test (its premise is broken) but the suite continues.

v1 runs the API backend only. A test that declares an entity/both backend, or
uses a raw entity-ref action, is SKIPPED with a reason until EntityWriter exists.

Results are a plain dict (dynamic, heavily serialized) -- logged, and optionally
written to a JSON file (the durable sink).
"""

import json

from . import assertions
from . import capabilities
from . import entities
from . import settle
from . import testspec

# Backends the engine can execute today. testspec.VALID_BACKENDS is wider (what a
# table may DECLARE); a valid-but-unrunnable backend is skipped, not failed.
SUPPORTED_BACKENDS = ("api", "entity")

# Settle expectation applied when an action declares none: the write must succeed.
DEFAULT_SETTLE = ["status == accepted"]


def run_tests(tests, conditions, controller_name, results_path=None):
    """Validate, then run the whole table. Returns a result dict. On any
    validation problem it returns early WITHOUT running -- fail loud, touch no
    pump."""
    problems = testspec.validate(tests, conditions)
    if problems:
        log.error("alpha_hwr_test engine: table invalid, not running:\n"
                  + json.dumps(problems, indent=2, default=str))
        result = {"aborted": True, "problems": problems,
                  "passed": 0, "failed": 0, "skipped": 0,
                  "total": len(tests), "tests": []}
        emit_results(result, results_path)
        return result

    api_writer = settle.ApiWriter(controller_name)
    entity_writer = settle.EntityWriter(controller_name)

    def reader(ref):
        return entities.read_value(ref, controller_name)

    test_results = []
    n_pass = 0
    n_fail = 0
    n_skip = 0
    for test in tests:
        tr = run_one_test(test, conditions, api_writer, entity_writer, reader)
        test_results.append(tr)
        if tr["skipped"]:
            n_skip += 1
        elif tr["passed"]:
            n_pass += 1
        else:
            n_fail += 1

    result = {"aborted": False, "problems": [],
              "passed": n_pass, "failed": n_fail, "skipped": n_skip,
              "total": len(tests), "tests": test_results}
    log.info(f"alpha_hwr_test engine: {n_pass} passed, {n_fail} failed, "
             f"{n_skip} skipped of {len(tests)}")
    emit_results(result, results_path)
    return result


def run_one_test(test, conditions, api_writer, entity_writer, reader):
    name = test.get("name", "<unnamed>")
    backend = test.get("backend", testspec.DEFAULT_BACKEND)
    tr = {"name": name, "backend": backend, "passed": False, "skipped": False,
          "reason": None, "actions": [], "expects": []}

    # "both" is not runnable yet (would run the suite twice); api/entity are.
    if backend not in SUPPORTED_BACKENDS:
        tr["skipped"] = True
        tr["reason"] = f"backend '{backend}' not runnable yet (supported: {SUPPORTED_BACKENDS})"
        log.warning(f"alpha_hwr_test '{name}': SKIPPED -- {tr['reason']}")
        return tr

    # The test's backend writer for API verbs; entity-ref actions always use the
    # entity writer (they ARE entity writes).
    backend_writer = api_writer
    if backend == "entity":
        backend_writer = entity_writer

    log.info(f"alpha_hwr_test '{name}' [{backend}]: running ({len(test.get('actions', []))} actions)")

    # Actions, in order. Stop on the first failed action -- the test's premise is
    # broken -- but the suite keeps going to the next test.
    all_actions_ok = True
    for action in test.get("actions", []):
        ar = run_action(action, backend_writer, entity_writer)
        tr["actions"].append(ar)
        if not ar["ok"]:
            all_actions_ok = False
            break

    if not all_actions_ok:
        tr["passed"] = False
        return tr

    # Entity expectations (with within-timeout polling).
    expanded, _ = testspec.expand_conditions(test.get("expect", []), conditions)
    expects_ok = True
    for cstr in expanded:
        a = assertions.parse_assertion(cstr)
        # NOTE: do NOT chain .to_dict() onto the evaluate() call. pyscript binds
        # self=None when a user-class method is called directly on a user
        # function's return value, so assign first, then call the method.
        er_result = assertions.evaluate(a, reader)
        er = er_result.to_dict()
        tr["expects"].append(er)
        if not er["passed"]:
            expects_ok = False

    tr["passed"] = expects_ok
    return tr


def run_action(action, backend_writer, entity_writer):
    verb, args, settle_list, perr = testspec.parse_action(action)
    ar = {"action": action, "ok": True, "reason": None, "settle": [], "result": None}
    if perr:
        ar["ok"] = False
        ar["reason"] = perr
        log.warning(f"alpha_hwr_test action: {perr}")
        return ar

    # An API verb goes to the test's backend writer; a raw entity-ref action is
    # inherently an entity write, so it always uses the entity writer.
    writer = backend_writer
    if not capabilities.is_verb(verb):
        writer = entity_writer
    api_result = writer.call(verb, args)
    ar["result"] = api_result.to_dict()

    checks = settle_list
    if not checks:
        checks = DEFAULT_SETTLE
    for sstr in checks:
        a = assertions.parse_assertion(sstr)
        # Assign first, then .to_dict() -- chaining a user-class method onto a
        # user function's return trips pyscript (self=None). See engine.py above.
        cr_result = assertions.check(a, settle_value(api_result, a.left))
        cr = cr_result.to_dict()
        ar["settle"].append(cr)
        if not cr["passed"]:
            ar["ok"] = False

    if ar["ok"]:
        log.info(f"alpha_hwr_test action {verb}: OK (settle status={api_result.status})")
    else:
        log.warning(f"alpha_hwr_test action {verb}: FAIL -- a settle assertion did not match "
                    f"(settle status={api_result.status})")
    return ar


def settle_value(api_result, field):
    """Value of a settle field for an assertion. status/detail come from the
    ApiResult (present even on timeout, where there is no event); every other
    field comes from the settle event payload."""
    if field == "status":
        return api_result.status
    if field == "detail":
        return api_result.detail
    if api_result.event is None:
        return None
    return api_result.event.get(field)


def emit_results(result, results_path):
    """Always log the results; write them to a JSON file too when a path is given."""
    log.info("alpha_hwr_test engine results:\n" + json.dumps(result, indent=2, default=str))
    if not results_path:
        return
    try:
        write_results_file(results_path, result)
        log.info(f"alpha_hwr_test engine: results written to {results_path}")
    except Exception as exc:
        log.warning(f"alpha_hwr_test engine: could not write results to {results_path}: {exc}")


@pyscript_executor
def write_results_file(path, data):
    import os
    import json
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2, default=str)
