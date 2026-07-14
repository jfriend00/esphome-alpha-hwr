"""Suspend and resume the user's pump-controlling automation(s) during a test.

Configured via `suspend_entities` in the app config: a list of full entity_ids
that gate whatever normally drives the pump -- an input_boolean, an automation
entity (works for YAML- or UI-authored automations alike), a switch, etc.

Before a test the entities are captured and turned off; afterward they are
restored to their captured state, so "off stays off, on returns to on". The
generic homeassistant.turn_off / turn_on services are used so any toggleable
domain works with the same code.
"""


def suspend_entities(entity_ids):
    """Capture each entity's state, then turn it off.

    Returns the captured states as {entity_id: state}, to hand to
    resume_entities(). Entities that don't exist are warned and skipped.
    """
    captured = {}
    for entity_id in entity_ids:
        if not state.exist(entity_id):
            log.warning(f"suspend: {entity_id} does not exist -- skipping")
            continue
        captured[entity_id] = str(state.get(entity_id))
        service.call("homeassistant", "turn_off", entity_id=entity_id)
        log.info(f"suspend: {entity_id} was '{captured[entity_id]}', turned off")
    return captured


def resume_entities(captured):
    """Restore each entity to the state captured by suspend_entities()."""
    for entity_id, prev in captured.items():
        if prev == "on":
            service.call("homeassistant", "turn_on", entity_id=entity_id)
        else:
            service.call("homeassistant", "turn_off", entity_id=entity_id)
        log.info(f"resume: {entity_id} restored to '{prev}'")
