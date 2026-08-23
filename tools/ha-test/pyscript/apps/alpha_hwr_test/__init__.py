"""ALPHA HWR HA test harness.

Thin service shell. Entity access lives in entities.py; snapshot/restore in
snapshot.py; automation suspend/resume in suspend.py; the guarded run lifecycle
in run.py. See SETUP.md (under tools/ha-test/) for install and test steps.
"""

import json

from . import entities
from . import snapshot
from . import suspend
from . import run
from . import profiles
from . import settle


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

# Where named profiles (saved snapshots) are stored. Default is inside the app
# dir (gitignored; deploy.bat excludes it via /XD profiles). Override to put it
# outside the app tree (e.g. if your deploy process mirrors/deletes).
PROFILES_DIR = CFG.get("profiles_dir", "/config/pyscript/apps/alpha_hwr_test/profiles")
PROFILE_NAME_ENTITY = "input_text.alpha_hwr_test_profile_name"

# Where the test engine writes its JSON results (last run). MUST live OUTSIDE the
# pyscript tree: writing any file under /config/pyscript trips pyscript's
# file-watcher, which reloads the app MID-RUN (a burst of "Unloaded" messages,
# a reload racing the in-flight run). Anywhere under /config that is not below
# /config/pyscript is fine.
RESULTS_DIR = CFG.get("results_dir", "/config/alpha_hwr_test/results")
RESULTS_PATH = f"{RESULTS_DIR}/last_run.json"


# --- Module state -----------------------------------------------------------
# Class-static state (the recirc_pump.py Status pattern) -- groups the harness's
# cross-call state and avoids the `global` keyword. Persists between service
# calls (until the app reloads).
class State:
    last_snapshot = None          # last captured pump snapshot dict (for restore)
    last_suspend_capture = None   # last suspend capture dict (for resume)


# The settled-write API. Constructed once here (the only place controller_name is
# readable) and threaded into the modules that write to the pump.
API = settle.ApiWriter(CONTROLLER_NAME)


# --- Services ---------------------------------------------------------------
@service
def alpha_hwr_test_ping():
    """yaml
name: ALPHA HWR test ping
description: Proof-of-life for the test harness. Logs a line when called.
"""
    log.info(f"alpha_hwr_test: ping -- controller_name={CONTROLLER_NAME}")


@service
def alpha_hwr_test_api_smoke():
    """yaml
name: ALPHA HWR test API smoke
description: Exercise the settled-write path end to end. Re-asserts the CURRENT pump mode via ApiWriter.call (no net change) and logs the ApiResult. Writes to the pump.
"""
    mode_disp = entities.get_value(entities.MODE_ENTITY, CONTROLLER_NAME)
    machine = entities.MODE_DISPLAY_TO_MACHINE.get(mode_disp)
    if machine is None:
        log.error(f"alpha_hwr_test api_smoke: current mode '{mode_disp}' has no known "
                  f"machine identifier -- cannot run smoke test")
        return
    log.info(f"alpha_hwr_test api_smoke: re-asserting mode '{mode_disp}' ({machine}) "
             f"via ApiWriter.call")
    result = API.call("set_mode", [machine])
    log.info("alpha_hwr_test api_smoke: result =\n"
             + json.dumps(result.to_dict(), indent=2, default=str))


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
    result = run.run_guarded(CONTROLLER_NAME, SUSPEND_ENTITIES, RESULTS_PATH)
    log.info("alpha_hwr_test run: result =\n"
             + json.dumps(result.to_dict(), indent=2, default=str))


def read_profile_name():
    """Read the profile name from the input_text helper ('' if missing)."""
    if not state.exist(PROFILE_NAME_ENTITY):
        return ""
    return state.get(PROFILE_NAME_ENTITY)


@service
def alpha_hwr_test_save_profile():
    """yaml
name: ALPHA HWR test save profile
description: Save current pump settings as a named profile (name from input_text.alpha_hwr_test_profile_name).
"""
    result = profiles.save_profile(PROFILES_DIR, read_profile_name(), CONTROLLER_NAME)
    log.info("alpha_hwr_test save_profile:\n" + json.dumps(result, indent=2, default=str))


@service
def alpha_hwr_test_load_profile():
    """yaml
name: ALPHA HWR test load profile
description: Load a named profile and apply it to the pump (guard + restore). Writes to the pump.
"""
    result = profiles.load_profile(PROFILES_DIR, read_profile_name(), CONTROLLER_NAME)
    log.info("alpha_hwr_test load_profile:\n" + json.dumps(result, indent=2, default=str))


@service
def alpha_hwr_test_list_profiles():
    """yaml
name: ALPHA HWR test list profiles
description: Log the names of saved profiles.
"""
    names = profiles.list_profiles(PROFILES_DIR)
    log.info("alpha_hwr_test profiles: " + json.dumps(names, default=str))
