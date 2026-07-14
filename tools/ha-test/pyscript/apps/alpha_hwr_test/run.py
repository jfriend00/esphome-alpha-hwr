"""Guarded test-run lifecycle for the ALPHA HWR test harness.

Wraps a run in the safety envelope every run needs:

  1. GUARD    -- refuse to start unless the pump reports ready.
  2. SUSPEND  -- turn off the user's pump automation(s).
  3. SNAPSHOT -- capture the pump state.
  4. RUN      -- (placeholder) the actual tests go here.
  5. RESTORE  -- put the pump state back.
  6. RESUME   -- re-enable the automation(s), ALWAYS (finally), AFTER restore, so
                 the automation wakes to a correctly-restored pump.

Returns a RunResult. A run always returns a result (even on abort or exception)
and always resumes the automations.
"""

from . import entities
from . import snapshot
from . import suspend

# How long to wait for pump_ready before giving up and aborting the run (the
# component may be mid-restart). See OPEN_ISSUES.md #2.
READY_TIMEOUT = 30.0


class RunResult:
    """Outcome of a guarded run (fixed shape -> dot access)."""
    def __init__(self):
        self.aborted = False
        self.reason = None
        self.snapshot = None    # the captured snapshot dict, or None
        self.restore = None     # a snapshot.RestoreResult, or None
        self.tests = []

    def to_dict(self):
        restore = None
        if self.restore is not None:
            restore = self.restore.to_dict()
        return {
            "aborted": self.aborted,
            "reason": self.reason,
            "snapshot": self.snapshot,
            "restore": restore,
            "tests": self.tests,
        }


def run_guarded(controller_name, suspend_entity_ids):
    """Run the guarded snapshot/restore lifecycle. Returns a RunResult."""
    result = RunResult()

    # 1. GUARD: the component must report ready before we touch anything.
    if not entities.wait_until_ready(controller_name, timeout=READY_TIMEOUT):
        result.aborted = True
        result.reason = f"pump_ready not 'on' within {READY_TIMEOUT}s -- run not started"
        log.error(f"alpha_hwr_test run aborted: {result.reason}")
        return result

    # 2. SUSPEND the user's pump automation(s).
    captured = suspend.suspend_entities(suspend_entity_ids)
    try:
        # 3. SNAPSHOT current pump state.
        snap = snapshot.get_snapshot(controller_name)
        result.snapshot = snap

        # 4. RUN TESTS -- placeholder. The test table/engine plugs in here:
        #    result.tests = engine.run(snap, controller_name, ...)

        # 5. RESTORE the pump state.
        result.restore = snapshot.restore_snapshot(snap, controller_name)
    except Exception as exc:
        result.reason = f"exception during run: {exc}"
        log.error(f"alpha_hwr_test run: {result.reason}")
    finally:
        # 6. RESUME the automation(s) -- ALWAYS, after restore.
        suspend.resume_entities(captured)

    return result
