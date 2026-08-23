# ALPHA HWR test-harness -- host tests

Plain-CPython unit tests for the harness's own **pure, pyscript-independent**
logic. No pyscript, no Home Assistant, no pump, no framework -- each file is a
standalone script that imports a harness module, asserts, prints
`N passed, M failed`, and exits non-zero on any failure.

## Run

```bash
cd tools/ha-test/tests
python run_tests.py
```

`run_tests.py` discovers every `test_*.py` here, runs each under the current
Python, and aggregates. Run one file directly while iterating:

```bash
python test_assertions.py
```

## What is (and isn't) covered here

These tests exercise the logic that has no pyscript dependency -- the assertion
grammar and comparison (`assertions.py`: `parse_assertion` / `compare` /
`check`) today, and the pure cores of later modules (the capability-map backend
resolution, `units.to_native`'s converter selection, named-condition expansion)
as they land.

The HA-coupled parts are **not** host-testable and are exercised on-device (or
later with mocks): the writers (`state`/`service` calls), the engine's I/O, and
`assertions.evaluate()`'s polling loop (`task.sleep`). Same line eman's
component suite draws -- pure logic on the host, hardware/HA-coupled code where
it actually runs.

## Files

- `run_tests.py` -- discovers and runs every `test_*.py`; exits non-zero on failure.
- `test_assertions.py` -- the assertion grammar + comparison battery.

These live outside `pyscript/apps/alpha_hwr_test/`, so `deploy.bat` never copies
them to the Home Assistant config -- they're a dev/CI artifact only.
