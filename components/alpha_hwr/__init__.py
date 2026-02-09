import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_WATT,
)

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["ble_client"]

alpha_hwr_ns = cg.esphome_ns.namespace("alpha_hwr")
AlphaHwrComponent = alpha_hwr_ns.class_("AlphaHwrComponent", cg.PollingComponent, ble_client.BLEClientNode)

CONF_FLOW = "flow"
CONF_HEAD = "head"
CONF_POWER = "power"
CONF_RPM = "rpm"
CONF_TEMP_MEDIA = "temp_media"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AlphaHwrComponent),
    cv.Required("ble_client_id"): cv.use_id(ble_client.BLEClient),
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
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    ble_client_var = await cg.get_variable(config["ble_client_id"])
    var = cg.new_Pvariable(config[CONF_ID], ble_client_var)
    await cg.register_component(var, config)
    
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

