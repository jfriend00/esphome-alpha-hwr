"""ALPHA HWR HA test harness.

Thin service shell. Entity access lives in entities.py; snapshot/restore in
snapshot.py. See SETUP.md (under tools/ha-test/) for install and test steps.
"""

import json

# Import EVERY sibling module directly here, even ones __init__ doesn't call
# itself (entities is used by snapshot, not by __init__). A direct import from
# __init__ is what gives a sibling its own reloadable pyscript context
# (apps.alpha_hwr_test.<name>). A module that is only imported transitively (by
# a sibling) never becomes a reload target, so edits to it silently never load.
from . import snapshot


# --- App configuration ------------------------------------------------------
# Delivered via the `pyscript: apps: alpha_hwr_test:` block in the HA config.
# app_config is only readable HERE, in the app's main file -- NOT in sibling
# modules (confirmed: they get NameError on pyscript.app_config). So we read and
# validate controller_name here and thread it into snapshot/restore.
CFG = pyscript.app_config or {}
CONTROLLER_NAME = CFG.get("controller_name", "")
if not CONTROLLER_NAME:
    log.error("alpha_hwr_test: 'controller_name' is not set in the app config "
              "(pyscript: apps: alpha_hwr_test:) -- entity resolution will fail")


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
description: Capture a pump-state snapshot and log it. Reads only; changes nothing.
"""
    snap = snapshot.get_snapshot(CONTROLLER_NAME)
    log.info("alpha_hwr_test snapshot:\n" + json.dumps(snap, indent=2, default=str))
