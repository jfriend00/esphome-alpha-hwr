"""Convert captured entity values to the units the pump's op_id API expects.

The API takes bare floats in the pump's native units (m3/h, C, minutes), but HA
reports each number entity in the USER's chosen display unit (e.g. gal/min, F).
The old number.set_value path let HA convert display->native on the way in; the
op_id services bypass HA, so we convert here, right before the write.

We use HA's OWN converters (allow_all_imports is on), so there are no
hand-derived factors and it tracks whatever units HA supports. The source unit
comes from the snapshot (captured verbatim from the entity, in display units);
the native target per field is a component fact, mapped below.

The HA import is done lazily inside to_native() so a bad import can only fail a
single conversion (-> caller SKIPS that write) rather than break app load.
"""

# Registry ref -> (HA quantity, API native unit). Only refs whose entity carries
# a convertible device_class are listed; every other number (RPM, meters) is
# already native and passes through untouched. Native units are component facts,
# NOT the user's display choice.
_NATIVE = {
    "number,constant_flow_setpoint":  ("volume_flow_rate", "m³/h"),
    "number,cycle_flow":              ("volume_flow_rate", "m³/h"),
    "number,temperature_range_min":   ("temperature", "°C"),
    "number,temperature_range_max":   ("temperature", "°C"),
    "number,cycle_time_on":           ("duration", "min"),
    "number,cycle_time_off":          ("duration", "min"),
}


def to_native(ref, value, unit):
    """Convert float `value` (in the entity's display `unit`) to `ref`'s native
    unit. Returns (ok, native_value, detail).

    ok=False means DON'T write -- the caller records the detail and skips, rather
    than sending a guessed value. Cases:
      * ref not convertible          -> pass through (already native).
      * unit already == native unit  -> pass through (no float round-trip noise).
      * unit missing / convert fails -> ok=False.
    """
    mapping = _NATIVE.get(ref)
    if mapping is None:
        return (True, value, "native (no conversion)")
    quantity, native_unit = mapping
    if unit is None:
        return (False, None, f"no unit captured; cannot convert to {native_unit}")
    if unit == native_unit:
        return (True, value, f"already {native_unit}")
    try:
        from homeassistant.util.unit_conversion import (
            TemperatureConverter,
            VolumeFlowRateConverter,
            DurationConverter,
        )
        converters = {
            "temperature": TemperatureConverter,
            "volume_flow_rate": VolumeFlowRateConverter,
            "duration": DurationConverter,
        }
        native = converters[quantity].convert(value, unit, native_unit)
    except Exception as exc:
        return (False, None,
                f"convert {value} {unit} -> {native_unit} failed "
                f"({type(exc).__name__}): {exc}")
    return (True, native, f"{value} {unit} -> {native} {native_unit}")
