"""Snapshot and restore of pump state for the ALPHA HWR test harness.

Captures the CURRENT operating mode, that mode's writable parameters, and the
global writable settings into a plain dict -- and restores them. Only the
active mode's parameters are captured (other modes' settings are not readable
on this component); this is an agreed simplifying assumption.

controller_name is passed in (from __init__, where app_config is readable) and
threaded down to the entity helpers.
"""

from . import entities


def get_snapshot(controller_name):
    """Capture pump state into a dict. Reads only; changes nothing.

    Structure:
        {
          "mode":        "<active mode>",
          "global":      { "domain,leaf": value, ... },   # excl. the mode selector
          "mode_params": { "domain,leaf": value, ... },   # active mode's params
        }
    A value of None means that entity did not resolve (see the warning log) --
    handy for spotting a wrong leaf name while bringing the registry up.
    """
    mode = entities.get_value(entities.MODE_ENTITY, controller_name)

    snap = {
        "mode": mode,
        "global": {},
        "mode_params": {},
    }

    for ref in entities.SNAPSHOT_GLOBAL:
        if ref == entities.MODE_ENTITY:
            continue  # captured separately as snap["mode"]
        snap["global"][ref] = entities.get_value(ref, controller_name)

    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        snap["mode_params"][ref] = entities.get_value(ref, controller_name)

    return snap


def restore_snapshot(snap, controller_name):
    """Restore a snapshot captured by get_snapshot().

    Order: set the mode first (so mode-specific params land in the right mode),
    then that mode's params in registry order, then the remaining globals.
    Read-only and missing/unknown values are skipped.
    """
    mode = snap.get("mode")

    # 1. Return to the original mode first.
    if mode not in (None, "unknown", "unavailable"):
        entities.set_value(entities.MODE_ENTITY, mode, controller_name)

    # 2. Restore that mode's parameters, in registry order (order can matter).
    mode_params = snap.get("mode_params", {})
    for ref in entities.SNAPSHOT_PER_MODE.get(mode, []):
        restore_one(ref, mode_params.get(ref), controller_name)

    # 3. Restore the remaining global writables (mode selector already done).
    for ref, value in snap.get("global", {}).items():
        if ref == entities.MODE_ENTITY:
            continue
        restore_one(ref, value, controller_name)


def restore_one(ref, value, controller_name):
    """Restore a single ref, skipping read-only / missing values."""
    if value in (None, "unknown", "unavailable"):
        log.warning(f"snapshot restore: skipping '{ref}' (captured value was {value})")
        return
    domain = entities.domain_of(ref)
    if domain not in ("switch", "number", "select"):
        log.info(f"snapshot restore: skipping read-only '{ref}' (domain '{domain}')")
        return
    entities.set_value(ref, value, controller_name)
