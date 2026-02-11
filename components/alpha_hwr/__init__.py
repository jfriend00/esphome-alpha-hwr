import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor, binary_sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_WATT,
)

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["ble_client", "text_sensor"]

alpha_hwr_ns = cg.esphome_ns.namespace("alpha_hwr")
AlphaHwrComponent = alpha_hwr_ns.class_(
    "AlphaHwrComponent", cg.PollingComponent, ble_client.BLEClientNode
)

CONF_FLOW = "flow"
CONF_HEAD = "head"
CONF_POWER = "power"
CONF_RPM = "rpm"
CONF_TEMP_MEDIA = "temp_media"
CONF_TEMP_CONVERTER = "temp_converter"
CONF_TEMP_PCB = "temp_pcb"
CONF_TEMP_CONTROL_BOX = "temp_control_box"
CONF_VOLTAGE = "voltage"
CONF_VOLTAGE_DC = "voltage_dc"
CONF_CURRENT = "current"
CONF_INLET_PRESSURE = "inlet_pressure"
CONF_OUTLET_PRESSURE = "outlet_pressure"
CONF_PAIRING_STATUS = "pairing_status"
CONF_ENABLE_PAIRING = "enable_pairing"
CONF_ALARMS = "alarms"
CONF_WARNINGS = "warnings"
CONF_SCHEDULE = "schedule"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AlphaHwrComponent),
        cv.Required("ble_client_id"): cv.use_id(ble_client.BLEClient),
        cv.Optional(CONF_ENABLE_PAIRING, default=False): cv.boolean,
        cv.Optional(CONF_FLOW): sensor.sensor_schema(
            unit_of_measurement="m³/h",
            accuracy_decimals=3,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HEAD): sensor.sensor_schema(
            unit_of_measurement="m",
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_RPM): sensor.sensor_schema(
            unit_of_measurement="RPM",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMP_MEDIA): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMP_CONVERTER): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMP_PCB): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMP_CONTROL_BOX): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_DC): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT): sensor.sensor_schema(
            unit_of_measurement="A",
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_INLET_PRESSURE): sensor.sensor_schema(
            unit_of_measurement="bar",
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_OUTLET_PRESSURE): sensor.sensor_schema(
            unit_of_measurement="bar",
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PAIRING_STATUS): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
        cv.Optional(CONF_ALARMS): text_sensor.text_sensor_schema(
            icon="mdi:alert-circle",
        ),
        cv.Optional(CONF_WARNINGS): text_sensor.text_sensor_schema(
            icon="mdi:alert",
        ),
        cv.Optional(CONF_SCHEDULE): text_sensor.text_sensor_schema(
            icon="mdi:calendar-clock",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    ble_client_var = await cg.get_variable(config["ble_client_id"])
    var = cg.new_Pvariable(config[CONF_ID], ble_client_var)
    await cg.register_component(var, config)

    # Set pairing enabled flag
    cg.add(var.set_pairing_enabled(config[CONF_ENABLE_PAIRING]))

    if CONF_FLOW in config:
        sens = await sensor.new_sensor(config[CONF_FLOW])
        cg.add(var.set_flow_sensor(sens))

    if CONF_HEAD in config:
        sens = await sensor.new_sensor(config[CONF_HEAD])
        cg.add(var.set_head_sensor(sens))

    if CONF_POWER in config:
        sens = await sensor.new_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(sens))

    if CONF_RPM in config:
        sens = await sensor.new_sensor(config[CONF_RPM])
        cg.add(var.set_rpm_sensor(sens))

    if CONF_TEMP_MEDIA in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_MEDIA])
        cg.add(var.set_temp_media_sensor(sens))

    if CONF_TEMP_CONVERTER in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_CONVERTER])
        cg.add(var.set_temp_converter_sensor(sens))

    if CONF_TEMP_PCB in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_PCB])
        cg.add(var.set_temp_pcb_sensor(sens))

    if CONF_TEMP_CONTROL_BOX in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_CONTROL_BOX])
        cg.add(var.set_temp_control_box_sensor(sens))

    if CONF_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_VOLTAGE])
        cg.add(var.set_voltage_sensor(sens))

    if CONF_VOLTAGE_DC in config:
        sens = await sensor.new_sensor(config[CONF_VOLTAGE_DC])
        cg.add(var.set_voltage_dc_sensor(sens))

    if CONF_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_CURRENT])
        cg.add(var.set_current_sensor(sens))

    if CONF_INLET_PRESSURE in config:
        sens = await sensor.new_sensor(config[CONF_INLET_PRESSURE])
        cg.add(var.set_inlet_pressure_sensor(sens))

    if CONF_OUTLET_PRESSURE in config:
        sens = await sensor.new_sensor(config[CONF_OUTLET_PRESSURE])
        cg.add(var.set_outlet_pressure_sensor(sens))

    if CONF_PAIRING_STATUS in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_PAIRING_STATUS])
        cg.add(var.set_pairing_status_binary_sensor(sens))

    if CONF_ALARMS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_ALARMS])
        cg.add(var.set_alarms_text_sensor(sens))

    if CONF_WARNINGS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_WARNINGS])
        cg.add(var.set_warnings_text_sensor(sens))

    if CONF_SCHEDULE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCHEDULE])
        cg.add(var.set_schedule_text_sensor(sens))
