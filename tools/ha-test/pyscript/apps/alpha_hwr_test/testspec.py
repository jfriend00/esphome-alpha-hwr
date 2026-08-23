"""Pure spec layer for the test engine: parse actions, expand named conditions,
and validate a test table before anything runs.

No pyscript deps -- imports only the other pure modules (capabilities,
assertions), via a dual-import so this file loads both as a pyscript-app sibling
(`from . import ...`) and standalone in a host test (`import ...` with the app
dir on sys.path). That keeps validate()/expand_conditions()/parse_action() unit-
testable off-device (see tests/test_testspec.py); execution lives in engine.py.

Test-table shape (see tests_table.py):
    TESTS = [
      {
        "name": "...",
        "backend": "api" | "entity" | "both",     # optional, default "api"
        "actions": [
           {"set_setpoint": ["constant_speed", 2000]},          # API verb + args
           {"set_setpoint": ["constant_speed", 9000],
            "settle": ["status == clamped", "value ~= 1650"]},  # + settle asserts
           {"number,temperature_range_min": 42},                # raw entity write
        ],
        "expect": [ "@pump_running", "sensor,motor_speed in 1800..2200 within 15s" ],
      },
    ]
    CONDITIONS = { "pump_running": ["binary_sensor,pump_motor_active == on within 10s", ...] }
"""

try:
    from . import capabilities
    from . import assertions
except ImportError:  # host-test context: app dir on sys.path
    import capabilities
    import assertions

# Backends a test may DECLARE (validation). What the engine can actually RUN is a
# separate, possibly smaller set (engine.SUPPORTED_BACKENDS) -- an unrunnable but
# valid backend is skipped at run time, not rejected here.
VALID_BACKENDS = ("api", "entity", "both")
DEFAULT_BACKEND = "api"

# Max named-condition nesting depth before we call it a cycle.
MAX_EXPAND_DEPTH = 20


def parse_action(action):
    """Split an action dict into (key, args, settle_list, error).

    `key` is the single command key -- an API verb or a 'domain,leaf' entity ref.
    `args` is a list (a scalar value becomes a one-element list). `settle_list` is
    the optional settle-assertion strings. `error` is a message string or None.
    """
    settle_list = action.get("settle", [])
    keys = [k for k in action if k != "settle"]
    if len(keys) != 1:
        return (None, None, settle_list,
                f"action must have exactly one command key (plus optional 'settle'), got {keys}")
    key = keys[0]
    raw = action[key]
    if isinstance(raw, list):
        args = list(raw)
    else:
        args = [raw]
    return (key, args, settle_list, None)


def expand_conditions(entries, conditions, depth=0):
    """Expand @named refs in an expect/condition list to a flat list of assertion
    strings. Returns (flat, errors). Nested @refs expand recursively; a cycle or
    runaway depth is reported rather than looping forever."""
    conditions = conditions or {}
    flat = []
    errors = []
    if depth > MAX_EXPAND_DEPTH:
        return ([], [f"condition nesting too deep (>{MAX_EXPAND_DEPTH}); cycle?"])
    for entry in entries:
        if isinstance(entry, str) and entry.startswith("@"):
            name = entry[1:]
            if name not in conditions:
                errors.append(f"unknown condition '@{name}'")
                continue
            sub_flat, sub_err = expand_conditions(conditions[name], conditions, depth + 1)
            flat.extend(sub_flat)
            errors.extend(sub_err)
        else:
            flat.append(entry)
    return (flat, errors)


def validate(tests, conditions):
    """Check a whole test table before running. Returns a list of problem dicts
    [{"test": name, "error": msg}]; empty means clean. Catches unknown verbs, bad
    arg counts, non-writable entity-ref writes, unparseable/misplaced assertions,
    invalid settle fields, unknown @conditions, and bad backends -- so a malformed
    table fails loudly up front instead of mid-run."""
    problems = []

    def add(name, msg):
        problems.append({"test": name, "error": msg})

    # Named conditions must parse (and be entity assertions, since they land in
    # expect lists).
    conditions = conditions or {}
    for cname in conditions:
        for cstr in conditions[cname]:
            a = try_parse(cstr)
            if a is None:
                add(f"@{cname}", f"unparseable condition: {cstr}")
            elif not a.is_ref:
                add(f"@{cname}", f"condition must be an entity ref (needs a comma): {cstr}")

    for test in tests:
        name = test.get("name", "<unnamed>")
        if "name" not in test:
            add(name, "test has no 'name'")
        backend = test.get("backend", DEFAULT_BACKEND)
        if backend not in VALID_BACKENDS:
            add(name, f"invalid backend '{backend}' (one of {VALID_BACKENDS})")

        for action in test.get("actions", []):
            validate_action(name, action, add)

        expanded, exp_err = expand_conditions(test.get("expect", []), conditions)
        for e in exp_err:
            add(name, e)
        for cstr in expanded:
            a = try_parse(cstr)
            if a is None:
                add(name, f"unparseable expect: {cstr}")
            elif not a.is_ref:
                add(name, f"expect must be an entity ref (needs a comma): {cstr}")

    return problems


def validate_action(name, action, add):
    verb, args, settle_list, perr = parse_action(action)
    if perr:
        add(name, perr)
        return

    if capabilities.is_verb(verb):
        names = capabilities.arg_names(verb)
        if names is None:
            add(name, f"unknown API verb '{verb}'")
        elif len(args) != len(names):
            add(name, f"'{verb}' expects {len(names)} args {names}, got {len(args)}: {args}")
    else:
        # entity-ref action: writable domain, single value
        if not capabilities.is_writable_ref(verb):
            add(name, f"entity-ref action '{verb}' is not a writable domain "
                      f"{capabilities.WRITABLE_DOMAINS}")
        if len(args) != 1:
            add(name, f"entity-ref action '{verb}' takes one value, got {args}")

    # settle assertions: must parse, must NOT be entity refs, and (for API verbs)
    # must name a field the command actually reports.
    for sstr in settle_list:
        a = try_parse(sstr)
        if a is None:
            add(name, f"unparseable settle assertion: {sstr}")
            continue
        if a.is_ref:
            add(name, f"settle assertion looks like an entity ref (has a comma): {sstr}")
            continue
        if capabilities.is_verb(verb) and not capabilities.valid_settle_field(verb, a.left):
            add(name, f"'{a.left}' is not a settled field of '{verb}': {sstr}")


def try_parse(text):
    try:
        return assertions.parse_assertion(text)
    except ValueError:
        return None
