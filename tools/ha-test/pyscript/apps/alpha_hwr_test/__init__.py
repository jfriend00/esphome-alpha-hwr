"""ALPHA HWR HA test harness -- skeleton (proof of life).

Minimal pyscript app that exposes a single service which logs a line when
called. Its only job right now is to prove the plumbing end to end:

  * the app loads from pyscript/apps/alpha_hwr_test/,
  * its configuration block is delivered (controller_name echoed below),
  * the service registers, and
  * it can be invoked from both the HA UI and the REST API.

No pump interaction yet -- this is the harness bootstrap. See SETUP.md
(alongside this app, under tools/ha-test/) for install and test steps.
"""

# --- App configuration ------------------------------------------------------
# Delivered via the `pyscript: apps: alpha_hwr_test:` block in the HA config.
# An app will not load at all without such a block (even an empty one), so this
# is always present once the app is running. `.app_config` is None when the
# block is empty; fall back to an empty dict so `.get(...)` is always safe.
CFG = pyscript.app_config or {}
CONTROLLER_NAME = CFG.get("controller_name", "<unset>")


# --- Services ---------------------------------------------------------------
@service
def alpha_hwr_test_ping():
    """yaml
name: ALPHA HWR test ping
description: Proof-of-life for the test harness. Logs a line when called.
"""
    log.info(
        f"alpha_hwr_test: ping -- harness skeleton alive; "
        f"controller_name={CONTROLLER_NAME}"
    )
