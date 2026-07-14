"""ALPHA HWR HA test harness.

Thin service shell. Entity access lives in entities.py; snapshot/restore in
snapshot.py; automation suspend/resume in suspend.py; the guarded run lifecycle
in run.py. See SETUP.md (under tools/ha-test/) for install and test steps.
"""

import json

from . import snapshot
from . import suspend
from . import run


# --- App configuration ------------------------------------------------------
# Delivered via the `pyscript: apps: alpha_hwr_test:` block in the HA config.
# app_config is only readable HERE, in the app's main file -- NOT in sibling
# modules (confirmed: they get NameError on pyscript.app_config). So we read and
# validate config here and thread it into the modules that need it.
CFG = pyscript.app_config or {}
CONTROLLER_NAME = CFG.get("controller_name", "")
if not CONTROLLER_NAME:
    log.error("alpha_hwr_test: 'controller_name' is not set in the app config "
              "(pyscript: apps: alpha_hwr_test:) -- entity resolution will fail")

# Entities that gate the user's pump automation: turned off during a test run
# and restored to their captured state afterward. Absent/empty = suspend none.
SUSPEND_ENTITIES = CFG.get("suspend_entities", []) or []


# --- Module state -----------------------------------------------------------
# Class-static state (the recirc_pump.py Status pattern) -- groups the harness's
# cross-call state and avoids the `global` keyword. Persists between service
# calls (until the app reloads).
class State:
    last_snapshot = None          # last captured pump snapshot dict (for restore)
    last_suspend_capture = None   # last suspend capture dict (for resume)


# --- Services ---------------------------------------------------------------
@service
def alpha_hwr_test_ping():
    """yaml
name: ALPHA HWR test ping
description: Proof-of-life for the test harness. Logs a line when called.
"""
    log.info(f"alpha_hwr_test: ping -- controller_name={CONTROLLER_NAME}")


@service
def alpha_hwr_test_snapshot():
    """yaml
name: ALPHA HWR test snapshot
description: Capture a pump-state snapshot, store it, and log it. Reads only; changes nothing.
"""
    State.last_snapshot = snapshot.get_snapshot(CONTROLLER_NAME)
    log.info("alpha_hwr_test snapshot (stored):\n"
             + json.dumps(State.last_snapshot, indent=2, default=str))


@service
def alpha_hwr_test_restore():
    """yaml
name: ALPHA HWR test restore
description: Restore the last captured snapshot and log the result. Writes to the pump.
"""
    if State.last_snapshot is None:
        log.error("alpha_hwr_test restore: no snapshot stored yet -- run the "
                  "alpha_hwr_test_snapshot service first")
        return
    result = snapshot.restore_snapshot(State.last_snapshot, CONTROLLER_NAME)
    log.info("alpha_hwr_test restore: result =\n"
             + json.dumps(result.to_dict(), indent=2, default=str))


@service
def alpha_hwr_test_suspend():
    """yaml
name: ALPHA HWR test suspend automations
description: Capture and turn off the configured suspend_entities; store for resume.
"""
    State.last_suspend_capture = suspend.suspend_entities(SUSPEND_ENTITIES)
    log.info("alpha_hwr_test suspend: captured =\n"
             + json.dumps(State.last_suspend_capture, indent=2, default=str))


@service
def alpha_hwr_test_resume():
    """yaml
name: ALPHA HWR test resume automations
description: Restore the suspend_entities to the state captured by the suspend service.
"""
    if State.last_suspend_capture is None:
        log.error("alpha_hwr_test resume: nothing suspended yet -- run the "
                  "alpha_hwr_test_suspend service first")
        return
    suspend.resume_entities(State.last_suspend_capture)
    log.info("alpha_hwr_test resume: restored suspend_entities")


@service
def alpha_hwr_test_run():
    """yaml
name: ALPHA HWR test run (guarded cycle)
description: Guarded run -- ready-gate, suspend automations, snapshot, restore, resume. Writes to the pump.
"""
    result = run.run_guarded(CONTROLLER_NAME, SUSPEND_ENTITIES)
    log.info("alpha_hwr_test run: result =\n"
             + json.dumps(result.to_dict(), indent=2, default=str))
