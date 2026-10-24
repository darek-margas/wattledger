import json
import zlib

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_LAMBDA,
    CONF_POWER,
    CONF_SENSOR,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
    UNIT_WATT,
)

CODEOWNERS = ["@darek-margas"]
AUTO_LOAD = ["sensor"]

CONF_INPUTS = "inputs"
CONF_LINES = "lines"
CONF_OUTPUTS = "outputs"
CONF_IMPORT_ENERGY = "import_energy"
CONF_EXPORT_ENERGY = "export_energy"
CONF_STALE_TIMEOUT = "stale_timeout"
CONF_INTERVAL = "interval"

wattledger_ns = cg.esphome_ns.namespace("wattledger")
WattLedgerComponent = wattledger_ns.class_(
    "WattLedgerComponent", cg.PollingComponent
)


def variable_name(value):
    """A name usable as a C++ parameter in output lambdas."""
    value = cv.validate_id_name(value)
    if not value.isidentifier():
        raise cv.Invalid(f"'{value}' must be a valid C++ identifier")
    return value


INPUT_SCHEMA = cv.Schema(
    {
        # The name of this input inside output lambdas.
        cv.Required(CONF_ID): variable_name,
        # A power sensor in W; its raw (unfiltered) state is used.
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
    }
)

LINE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): variable_name,
        # A voltage sensor in V.
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
    }
)

ENERGY_SENSOR_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)

OUTPUT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): variable_name,
        # Power (W) from the inputs, which are passed as float parameters
        # named by their ids. Evaluated on every sample; must be pure.
        cv.Required(CONF_LAMBDA): cv.returning_lambda,
        # Average power since the previous update.
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Energy since boot, for dashboards. HA's history comes from the journal.
        cv.Optional(CONF_IMPORT_ENERGY): ENERGY_SENSOR_SCHEMA,
        cv.Optional(CONF_EXPORT_ENERGY): ENERGY_SENSOR_SCHEMA,
    }
)


def _validate_unique_names(config):
    names = [
        item[CONF_ID]
        for key in (CONF_INPUTS, CONF_LINES, CONF_OUTPUTS)
        for item in config[key]
    ]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise cv.Invalid(
            f"Input, line and output ids must be unique: {', '.join(duplicates)}"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WattLedgerComponent),
            cv.Required(CONF_INPUTS): cv.All(
                cv.ensure_list(INPUT_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(CONF_LINES, default=[]): cv.ensure_list(LINE_SCHEMA),
            cv.Optional(CONF_OUTPUTS, default=[]): cv.ensure_list(OUTPUT_SCHEMA),
            # An input not updated for this long makes the time a gap.
            cv.Optional(
                CONF_STALE_TIMEOUT, default="5s"
            ): cv.positive_time_period_milliseconds,
            # Length of a recorded interval (the 1-minute tier).
            cv.Optional(
                CONF_INTERVAL, default="60s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.polling_component_schema("10s")),
    _validate_unique_names,
)


def definitions_hash(config):
    """Identifies the output definitions a record was produced with (design 5.3)."""
    definitions = {
        "inputs": [item[CONF_ID] for item in config[CONF_INPUTS]],
        "outputs": [
            [item[CONF_ID], item[CONF_LAMBDA].value] for item in config[CONF_OUTPUTS]
        ],
    }
    return zlib.crc32(json.dumps(definitions).encode())


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT]))
    cg.add(var.set_interval(config[CONF_INTERVAL]))
    cg.add(var.set_defs_hash(definitions_hash(config)))

    input_names = [item[CONF_ID] for item in config[CONF_INPUTS]]
    for item in config[CONF_INPUTS]:
        sens = await cg.get_variable(item[CONF_SENSOR])
        cg.add(var.add_input(item[CONF_ID], sens))
    for item in config[CONF_LINES]:
        sens = await cg.get_variable(item[CONF_SENSOR])
        cg.add(var.add_line(item[CONF_ID], sens))

    for item in config[CONF_OUTPUTS]:
        lambda_ = await cg.process_lambda(
            item[CONF_LAMBDA],
            [(cg.float_, name) for name in input_names],
            return_type=cg.float_,
        )
        # Adapt the named-parameter lambda to OutputFn (float(const float *)).
        args = ", ".join(f"inputs[{i}]" for i in range(len(input_names)))
        output_fn = cg.RawExpression(
            f"[](const float *inputs) -> float {{ return ({lambda_})({args}); }}"
        )
        sensors = []
        for key in (CONF_POWER, CONF_IMPORT_ENERGY, CONF_EXPORT_ENERGY):
            if key in item:
                sensors.append(await sensor.new_sensor(item[key]))
            else:
                sensors.append(cg.nullptr)
        cg.add(var.add_output(item[CONF_ID], output_fn, *sensors))
