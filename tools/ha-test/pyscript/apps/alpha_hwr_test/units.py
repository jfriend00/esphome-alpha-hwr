"""Unit conversion between an entity's DISPLAY units and the pump's NATIVE units.

The op_id API takes bare floats in the pump's native units (m3/h, C, minutes),
but HA number entities report/accept the USER's chosen display unit (e.g. gal/min,
F). Two directions are needed:
  * to_native  -- capture/read side (display -> native), and the API write path.
  * to_display -- the ENTITY write path (native -> display), since HA number
                  services interpret the value in the entity's display unit and
                  convert back to native themselves.

We use HA's OWN converters (allow_all_imports is on), so there are no hand-derived
factors and it tracks whatever units HA supports. The native unit per field is a
component fact, mapped below; the display unit comes from the live entity.

The HA import is lazy (inside convert_quantity) so a bad import can only fail a
single conversion (-> caller SKIPS that write) rather than break app load.
"""

# Registry ref -> (HA quantity, pump NATIVE unit). Only refs whose entity carries
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


def convert_quantity(quantity, value, from_unit, to_unit):
    """Convert `value` from `from_unit` to `to_unit` using HA's own converter for
    `quantity`. Lazy import so a converter problem fails one call, not app load."""
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
    return converters[quantity].convert(value, from_unit, to_unit)


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
        native = convert_quantity(quantity, value, unit, native_unit)
    except Exception as exc:
        return (False, None,
                f"convert {value} {unit} -> {native_unit} failed "
                f"({type(exc).__name__}): {exc}")
    return (True, native, f"{value} {unit} -> {native} {native_unit}")


def to_display(ref, native_value, display_unit):
    """Convert `native_value` (in `ref`'s native unit) to the entity's `display_unit`,
    for the ENTITY write path. Returns (ok, display_value, detail). Mirrors
    to_native: non-convertible refs and native==display pass through; a missing
    display unit or a convert failure returns ok=False so the caller skips."""
    mapping = _NATIVE.get(ref)
    if mapping is None:
        return (True, native_value, "native (no conversion)")
    quantity, native_unit = mapping
    if display_unit is None:
        return (False, None, f"no display unit on the entity; cannot convert from {native_unit}")
    if display_unit == native_unit:
        return (True, native_value, f"already {native_unit}")
    try:
        display = convert_quantity(quantity, native_value, native_unit, display_unit)
    except Exception as exc:
        return (False, None,
                f"convert {native_value} {native_unit} -> {display_unit} failed "
                f"({type(exc).__name__}): {exc}")
    return (True, display, f"{native_value} {native_unit} -> {display} {display_unit}")
