#!/usr/bin/env python3
"""Host test runner for the ALPHA HWR test harness.

Runs every tests/test_*.py under CPython (NOT pyscript) and aggregates the
results. These cover the harness's PURE, pyscript-independent logic -- the
assertion grammar today, more as modules grow. HA-coupled code (the writers, the
engine's state/service I/O, evaluate()'s polling loop) is exercised on-device or
with mocks, not here.

    python run_tests.py

Exits non-zero if any test file fails, so it drops straight into CI.
"""

import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    files = sorted(glob.glob(os.path.join(HERE, "test_*.py")))
    if not files:
        print("run_tests: no test_*.py files found")
        return 1

    failed = []
    for path in files:
        name = os.path.basename(path)
        print(f"=== {name} ===", flush=True)  # flush so the header precedes child output
        rc = subprocess.call([sys.executable, path])
        if rc != 0:
            failed.append(name)
        print()

    total = len(files)
    if failed:
        print(f"{total - len(failed)}/{total} test files passed; FAILED: {', '.join(failed)}")
        return 1
    print(f"{total}/{total} test files passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
