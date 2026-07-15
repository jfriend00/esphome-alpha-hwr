"""Save/load named "profiles" of pump settings for the ALPHA HWR test harness.

A profile is just a snapshot (the get_snapshot dict) written to a JSON file:
  * save  = get_snapshot() -> write <profiles_dir>/<name>.json
  * load  = read the file -> restore_snapshot()  (guarded by pump_ready)

Useful for component development: park a working mode + settings under a name and
recall it in a couple of clicks, separate from your day-to-day pump config.

File I/O uses @pyscript_executor so it runs as real CPython in a worker thread
(non-blocking, and `with open(...)` works).
"""

from . import entities
from . import snapshot

# Guard timeout for load (component may be mid-restart). See OPEN_ISSUES.md #2.
GUARD_TIMEOUT = 30.0

# Profile names are restricted to this safe set so a name can never escape the
# profiles directory (no '/', no '..'). Defense-in-depth, not a hard security
# boundary -- the name comes from the user's own input_text -- but it stops a
# sloppy/typo'd name from clobbering a file outside profiles_dir. The input_text
# helper mirrors this as a UI `pattern` (client-side); this is the real guard,
# since a service/API call bypasses the UI.
NAME_OK = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
MAX_NAME_LEN = 64

# --- Pump-model gate (NOT IMPLEMENTED -- see OPEN_ISSUES.md #5) --------------
# eman targets multiple pump models; a profile saved on one model may be wrong
# (or unsafe) on another. A future gate should refuse a cross-model load. Likely
# comparison: the diagnostic sensors Product Name + Product Version (model
# identity). Serial Number is per-unit (too strict -- blocks sharing a profile
# between two identical pumps). Software Version (firmware) is probably a soft
# WARN, not a hard block; Hardware Version is finer than needed. When decided,
# save those identity fields into the profile JSON as metadata and compare on
# load. Not implemented -- John + eman have the same pump, no others to test.


def safe_name(name):
    """Reduce a profile name to a safe filename stem, or '' if nothing usable."""
    if name is None:
        return ""
    cleaned = "".join([c for c in str(name).strip() if c in NAME_OK])
    return cleaned[:MAX_NAME_LEN]


def profile_path(profiles_dir, name):
    """Full path for a profile file (name already sanitized -> stays in dir)."""
    base = profiles_dir.rstrip("/")
    return f"{base}/{name}.json"


@pyscript_executor
def write_json_file(path, data):
    import os
    import json
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)


@pyscript_executor
def read_json_file(path):
    import json
    with open(path, "r") as f:
        return json.load(f)


@pyscript_executor
def file_exists(path):
    import os
    return os.path.isfile(path)


@pyscript_executor
def list_json_stems(dir_path):
    import os
    if not os.path.isdir(dir_path):
        return []
    stems = []
    for fname in sorted(os.listdir(dir_path)):
        if fname.endswith(".json"):
            stems.append(fname[:-5])
    return stems


def save_profile(profiles_dir, name, controller_name):
    """Capture current pump settings and write them as profile <name>."""
    stem = safe_name(name)
    if not stem:
        return {"ok": False, "reason": f"invalid profile name: '{name}'"}
    snap = snapshot.get_snapshot(controller_name)
    path = profile_path(profiles_dir, stem)
    try:
        write_json_file(path, snap)
    except Exception as exc:
        return {"ok": False, "reason": f"write failed: {exc}"}
    log.info(f"profiles: saved '{stem}' -> {path}")
    return {"ok": True, "name": stem, "path": path, "snapshot": snap}


def load_profile(profiles_dir, name, controller_name):
    """Read profile <name> and apply it via restore_snapshot (guarded)."""
    stem = safe_name(name)
    if not stem:
        return {"ok": False, "reason": f"invalid profile name: '{name}'"}
    path = profile_path(profiles_dir, stem)
    if not file_exists(path):
        return {"ok": False, "reason": f"profile not found: '{stem}'"}

    # GUARD: the component must be ready before we write settings to it.
    if not entities.wait_until_ready(controller_name, timeout=GUARD_TIMEOUT):
        return {"ok": False, "reason": f"pump not ready within {GUARD_TIMEOUT}s -- not applied"}

    try:
        snap = read_json_file(path)
    except Exception as exc:
        return {"ok": False, "reason": f"read failed: {exc}"}

    result = snapshot.restore_snapshot(snap, controller_name)
    log.info(f"profiles: loaded '{stem}' from {path}")
    return {"ok": True, "name": stem, "restore": result.to_dict()}


def list_profiles(profiles_dir):
    """Return the list of saved profile names (file stems)."""
    return list_json_stems(profiles_dir)
