"""Test table for the ALPHA HWR harness -- SEED examples; tune values to your pump.

Tests run inside the guarded cycle (snapshot -> tests -> restore), so they write
to the pump and the pump is returned to its prior state afterward. Values below
reflect John's pump (constant-speed hardware minimum ~1650 RPM); adjust for
another unit.

Schema and grammar: see testspec.py and assertions.py. In short:
  * actions   -- API verbs ({"set_setpoint": ["constant_speed", 2000]}) with an
                 optional "settle" list of settle-field assertions (default
                 expectation: "status == accepted").
  * expect    -- entity assertions ("<domain,leaf> <op> <value> [within Ns]"),
                 or "@name" references into CONDITIONS.
  * backend   -- "api" (default) for now; "entity"/"both" are skipped until the
                 entity backend exists.
"""

CONDITIONS = {
    # The pump is actually running: the motor reports active AND a nonzero speed.
    "pump_running": [
        "binary_sensor,pump_motor_active == on within 15s",
        "sensor,motor_speed > 0 within 15s",
    ],
}

TESTS = [
    {
        "name": "constant_speed_2000_runs",
        "actions": [
            {"set_mode": "constant_speed"},
            {"set_setpoint": ["constant_speed", 2000]},
            {"set_pump_state": "engaged"},
        ],
        "expect": [
            "@pump_running",
            "sensor,motor_speed ~= 2000 +/- 5% within 20s",
        ],
    },
    {
        # Same actions as constant_speed_2000_runs, but driven through the HA
        # ENTITY path (number/switch/select writes) instead of the op_id API --
        # exercises EntityWriter and HA's own unit handling. Expects are identical
        # (they read the same sensors regardless of how the write happened).
        "name": "constant_speed_via_entities",
        "backend": "entity",
        "actions": [
            {"set_mode": "constant_speed"},
            {"set_setpoint": ["constant_speed", 2000]},
            {"set_pump_state": "engaged"},
        ],
        "expect": [
            "@pump_running",
            "sensor,motor_speed ~= 2000 +/- 5% within 20s",
        ],
    },
    {
        "name": "underspeed_clamps_to_min",
        "actions": [
            {"set_mode": "constant_speed"},
            # 1400 RPM is below this pump's hardware minimum; the pump clamps it to
            # ~1650 and the settle event reports status=clamped with the stored
            # value. (Adjust 1650 to your pump's minimum.)
            {"set_setpoint": ["constant_speed", 1400],
             "settle": ["status == clamped", "value ~= 1650 +/- 50"]},
            {"set_pump_state": "engaged"},
        ],
        "expect": [
            "@pump_running",
            "sensor,motor_speed ~= 1650 +/- 10% within 20s",
        ],
    },
]
