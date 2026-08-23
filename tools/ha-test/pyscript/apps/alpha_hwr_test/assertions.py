"""Assertion parsing + evaluation for the ALPHA HWR test harness.

An assertion is a compact string checked against a value from one of two sources:
  * an ENTITY read -- the left token is a "domain,leaf" ref (has a comma). Polled
    with a within-timeout, since real sensors settle over time.
  * a SETTLE field -- the left token is a bare field name from the write_settled
    event (no comma, e.g. status / value / temp_min). Checked once; the settle
    result is already in hand.

Grammar (single-parse, whole-token ops -> deterministic):

    <left> <op> <value> [within <N>[s]]

  op    := === | == | != | >= | <= | > | < | in | ~=
  value := (by op)
    == != ===   scalar: number, bare token (on/off/unavailable), or 'quoted'
    > >= < <=   number
    in          range  "lo..hi"   OR  set  "a,b,c"   (membership; strings or nums)
    ~=          "target +/- tol"  or  "target +/- tol%"  (bare "target" = default tol)

Comparison:
    ==  / !=    numbers within DEFAULT_TOL, strings exact
    ===         exact (numbers: zero tolerance)
    ~=          explicit tolerance, absolute or percent
    in          numeric range, or explicit set
    > >= < <=   numeric

parse_assertion(), compare() and check() are pure Python (no pyscript globals),
so they are unit-testable off-device. Only evaluate() -- the polling loop -- uses
task.sleep and must run inside pyscript.
"""

import time

# Absolute tolerance applied to numeric `==` / `!=`. Sized to swallow unit-
# conversion and float round-off noise (e.g. 2.0 read back as 1.9999999), NOT
# physical variance -- use ~= or in for that. A single knob; make it config-
# injected later if a test ever needs a different floor.
DEFAULT_TOL = 1e-3

# States meaning "no real value yet". Treated as not-satisfied (keep polling) for
# every op except an explicit "== <bad token>".
BAD = (None, "unknown", "unavailable")

# Longest first: the parser matches the op as a whole whitespace token, but the
# ordering documents that === must not be read as == plus a stray =.
OPS = ("===", "==", "!=", ">=", "<=", ">", "<", "in", "~=")


def as_float(value):
    """Parse to float, or None if not numeric."""
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


def unquote(s):
    """Strip a matching pair of surrounding quotes, if present."""
    if len(s) >= 2 and s[0] == s[-1] and s[0] in ("'", '"'):
        return s[1:-1]
    return s


class Assertion:
    """Parsed assertion (fixed shape -> dot access)."""
    def __init__(self, left, is_ref, op, spec, timeout_s, raw):
        self.left = left            # entity ref ("domain,leaf") or settle field
        self.is_ref = is_ref        # True if left is an entity ref (has a comma)
        self.op = op
        self.spec = spec            # ("scalar",v) | ("num",n) | ("range",lo,hi)
                                    #   | ("set",[..]) | ("approx",tgt,tol,is_pct)
        self.timeout_s = timeout_s  # float, or None for check-once
        self.raw = raw


def parse_assertion(text):
    """Parse one assertion string into an Assertion. Raises ValueError on a
    malformed line so the loader can report which row failed."""
    raw = text
    text = text.strip()

    # Peel an optional "within <N>[s]" tail off the end.
    timeout_s = None
    idx = text.rfind(" within ")
    if idx != -1:
        tail = text[idx + 8:].strip()
        if tail.endswith("s"):
            tail = tail[:-1]
        t = as_float(tail)
        if t is None:
            raise ValueError(f"bad 'within' value in: {raw}")
        timeout_s = t
        text = text[:idx].strip()

    # <left> <op> <value>  (value keeps any internal spaces)
    parts = text.split(None, 2)
    if len(parts) < 3:
        raise ValueError(f"expected '<left> <op> <value>' in: {raw}")
    left = parts[0]
    op = parts[1]
    value = parts[2].strip()
    if op not in OPS:
        raise ValueError(f"unknown operator '{op}' in: {raw}")

    is_ref = ("," in left)
    spec = parse_value(op, value, raw)
    return Assertion(left, is_ref, op, spec, timeout_s, raw)


def parse_value(op, value, raw):
    if op in ("==", "!=", "==="):
        return ("scalar", unquote(value))
    if op in (">", ">=", "<", "<="):
        n = as_float(value)
        if n is None:
            raise ValueError(f"'{op}' needs a number in: {raw}")
        return ("num", n)
    if op == "in":
        if ".." in value:
            lo_s, hi_s = value.split("..", 1)
            lo = as_float(lo_s)
            hi = as_float(hi_s)
            if lo is None or hi is None:
                raise ValueError(f"bad range in: {raw}")
            return ("range", lo, hi)
        if "," in value:
            items = [unquote(p.strip()) for p in value.split(",")]
            return ("set", items)
        raise ValueError(f"'in' needs 'lo..hi' or a comma set in: {raw}")
    if op == "~=":
        if "+/-" in value:
            tgt_s, tol_s = value.split("+/-", 1)
            tgt_s = tgt_s.strip()
            tol_s = tol_s.strip()
            is_pct = tol_s.endswith("%")
            if is_pct:
                tol_s = tol_s[:-1].strip()
            tgt = as_float(tgt_s)
            tol = as_float(tol_s)
            if tgt is None or tol is None:
                raise ValueError(f"bad '~=' tolerance in: {raw}")
            return ("approx", tgt, tol, is_pct)
        tgt = as_float(value)
        if tgt is None:
            raise ValueError(f"bad '~=' target in: {raw}")
        return ("approx", tgt, DEFAULT_TOL, False)
    # OPS membership was checked in parse_assertion, so this is unreachable.
    raise ValueError(f"unhandled operator '{op}' in: {raw}")


def compare(assertion, actual):
    """True if `actual` (a state/field string, or None) satisfies the assertion."""
    op = assertion.op
    spec = assertion.spec

    if op in ("==", "!=", "==="):
        return compare_eq(op, spec[1], actual)

    a = as_float(actual)
    if a is None:
        # A string actual can still satisfy an `in` SET (e.g. status membership).
        if op == "in" and spec[0] == "set":
            return set_contains(spec[1], actual)
        return False

    if op == ">":
        return a > spec[1]
    if op == ">=":
        return a >= spec[1]
    if op == "<":
        return a < spec[1]
    if op == "<=":
        return a <= spec[1]
    if op == "~=":
        target = spec[1]
        tol = spec[2]
        if spec[3]:  # percent
            tol = abs(target) * tol / 100.0
        return abs(a - target) <= tol
    if op == "in":
        if spec[0] == "range":
            return spec[1] <= a <= spec[2]
        if spec[0] == "set":
            return set_contains(spec[1], actual)
    return False


def set_contains(items, actual):
    """True if `actual` matches any set item (per == semantics)."""
    for item in items:
        if compare_eq("==", item, actual):
            return True
    return False


def compare_eq(op, expected, actual):
    """== / != / === comparison. Numbers use DEFAULT_TOL (zero for ===); strings
    compare exactly. A bad/missing actual matches only an explicit '== <bad>'."""
    if actual in BAD:
        if op == "==":
            return str(actual) == expected
        return False
    an = as_float(actual)
    en = as_float(expected)
    if an is not None and en is not None:
        if op == "===":
            match = (an == en)
        else:
            match = abs(an - en) <= DEFAULT_TOL
    else:
        match = (str(actual) == expected)
    if op == "!=":
        return not match
    return match


class AssertionResult:
    """Outcome of evaluating one assertion (fixed shape -> dot access)."""
    def __init__(self, assertion):
        self.assertion = assertion.raw
        self.left = assertion.left
        self.actual = None
        self.passed = False
        self.elapsed = 0.0
        self.timed_out = False

    def to_dict(self):
        return {
            "assert": self.assertion,
            "actual": self.actual,
            "passed": self.passed,
            "elapsed": round(self.elapsed, 2),
            "timed_out": self.timed_out,
        }


def check(assertion, actual):
    """Evaluate once against an already-known value (settle-field path)."""
    result = AssertionResult(assertion)
    result.actual = actual
    result.passed = compare(assertion, actual)
    return result


def evaluate(assertion, reader, poll=0.25):
    """Poll reader(left) until the assertion passes or its within-timeout elapses
    (entity-read path). With no timeout, checks once. reader is a callable taking
    the assertion's left token and returning the current value string, or None.
    Uses task.sleep, so call from within pyscript."""
    deadline = None
    if assertion.timeout_s is not None:
        deadline = time.monotonic() + assertion.timeout_s
    start = time.monotonic()

    while True:
        actual = reader(assertion.left)
        if compare(assertion, actual):
            result = AssertionResult(assertion)
            result.actual = actual
            result.passed = True
            result.elapsed = time.monotonic() - start
            return result
        if deadline is None or time.monotonic() >= deadline:
            result = AssertionResult(assertion)
            result.actual = actual
            result.passed = False
            result.elapsed = time.monotonic() - start
            result.timed_out = (deadline is not None)
            return result
        task.sleep(poll)
