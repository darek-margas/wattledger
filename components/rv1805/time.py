import esphome.codegen as cg
from esphome.components import i2c, time
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["i2c"]

CONF_TRICKLE_CHARGE = "trickle_charge"

rv1805_ns = cg.esphome_ns.namespace("rv1805")
RV1805Component = rv1805_ns.class_(
    "RV1805Component", time.RealTimeClock, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    time.TIME_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(RV1805Component),
            # Charges the backup supercapacitor on the SparkFun RV-1805 board.
            # Disable only if a primary (non-rechargeable) cell is fitted.
            cv.Optional(CONF_TRICKLE_CHARGE, default=True): cv.boolean,
        }
    ).extend(i2c.i2c_device_schema(0x69))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    await time.register_time(var, config)
    cg.add(var.set_trickle_charge(config[CONF_TRICKLE_CHARGE]))
