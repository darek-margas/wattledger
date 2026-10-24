import json
import zlib

import esphome.codegen as cg
from esphome.components import (
    button,
    esp32,
    sensor,
    text_sensor,
    time,
    web_server_base,
)
from esphome.components.esp32 import add_partition
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_LAMBDA,
    CONF_POWER,
    CONF_SENSOR,
    CONF_TIME_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
    UNIT_PERCENT,
    UNIT_WATT,
)

CODEOWNERS = ["@darek-margas"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["sensor", "text_sensor", "button", "web_server_base"]

CONF_INPUTS = "inputs"
CONF_LINES = "lines"
CONF_OUTPUTS = "outputs"
CONF_IMPORT_ENERGY = "import_energy"
CONF_EXPORT_ENERGY = "export_energy"
CONF_STALE_TIMEOUT = "stale_timeout"
CONF_OUTPUT_SLOTS = "output_slots"
CONF_MIN1_RETENTION = "min1_retention"
CONF_MIN5_RETENTION = "min5_retention"
CONF_WEB_HANDLER_ID = "web_handler_id"
CONF_PVOUTPUT = "pvoutput"
CONF_API_KEY = "api_key"
CONF_SYSTEM_ID = "system_id"
CONF_DONOR = "donor"
CONF_GENERATION = "generation"
CONF_CONSUMPTION = "consumption"
CONF_VOLTAGE = "voltage"
CONF_RESERVE_REQUESTS = "reserve_requests"
CONF_BACKLOG = "backlog"
CONF_RATE_LIMIT_REMAINING = "rate_limit_remaining"
CONF_LAST_UPLOAD = "last_upload"
CONF_REPLAY_PROGRESS = "replay_progress"
CONF_LAST_ERROR = "last_error"
CONF_REPLAY = "replay"

PARTITION_NAME = "wattledger"
SECTOR_SIZE = 4096
EVENT_RECORD_SIZE = 32
EVENT_RECORDS = 1024
SPARE_OUTPUT_SLOTS = 4

wattledger_ns = cg.esphome_ns.namespace("wattledger")
WattLedgerComponent = wattledger_ns.class_(
    "WattLedgerComponent", cg.PollingComponent
)
WebHandler = wattledger_ns.class_("WebHandler", cg.Component)
PVOutputUploader = wattledger_ns.class_("PVOutputUploader", cg.PollingComponent)
ReplayButton = wattledger_ns.class_("ReplayButton", button.Button)

PVOUTPUT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PVOutputUploader),
        cv.Required(CONF_API_KEY): cv.string_strict,
        cv.Required(CONF_SYSTEM_ID): cv.string,
        # Donation mode: 100 statuses per request, 300 requests/h, 90 days of
        # history; otherwise 30, 60/h and 14 days.
        cv.Optional(CONF_DONOR, default=False): cv.boolean,
        # Output ids: their import energy is uploaded.
        cv.Required(CONF_GENERATION): cv.string,
        cv.Optional(CONF_CONSUMPTION): cv.string,
        # Line id for the voltage (v6).
        cv.Optional(CONF_VOLTAGE): cv.string,
        # Requests per hour left for other clients of the same API key.
        cv.Optional(CONF_RESERVE_REQUESTS, default=10): cv.int_range(min=0, max=250),
        cv.Optional(CONF_BACKLOG): sensor.sensor_schema(
            unit_of_measurement="records",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:tray-full",
        ),
        cv.Optional(CONF_RATE_LIMIT_REMAINING): sensor.sensor_schema(
            unit_of_measurement="requests",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:speedometer-slow",
        ),
        cv.Optional(CONF_LAST_UPLOAD): sensor.sensor_schema(
            device_class=DEVICE_CLASS_TIMESTAMP,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:cloud-upload",
        ),
        cv.Optional(CONF_REPLAY_PROGRESS): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:history",
        ),
        cv.Optional(CONF_LAST_ERROR): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
        cv.Optional(CONF_REPLAY): button.button_schema(
            ReplayButton, icon="mdi:cloud-refresh"
        ),
    }
).extend(cv.polling_component_schema("10s"))


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


def _validate_pvoutput(config):
    if (pvo := config.get(CONF_PVOUTPUT)) is None:
        return config
    outputs = [item[CONF_ID] for item in config[CONF_OUTPUTS]]
    lines = [item[CONF_ID] for item in config[CONF_LINES]]
    for key in (CONF_GENERATION, CONF_CONSUMPTION):
        if key in pvo and pvo[key] not in outputs:
            raise cv.Invalid(
                f"{CONF_PVOUTPUT}.{key}: '{pvo[key]}' is not an output (outputs: {', '.join(outputs)})"
            )
    if CONF_VOLTAGE in pvo and pvo[CONF_VOLTAGE] not in lines:
        raise cv.Invalid(
            f"{CONF_PVOUTPUT}.{CONF_VOLTAGE}: '{pvo[CONF_VOLTAGE]}' is not a line (lines: {', '.join(lines)})"
        )
    return config


def _validate_output_slots(config):
    outputs = len(config[CONF_OUTPUTS])
    config.setdefault(CONF_OUTPUT_SLOTS, outputs + SPARE_OUTPUT_SLOTS)
    if config[CONF_OUTPUT_SLOTS] < outputs:
        raise cv.Invalid(
            f"{CONF_OUTPUT_SLOTS} ({config[CONF_OUTPUT_SLOTS]}) must be at least the number of outputs ({outputs})"
        )
    return config


def interval_record_size(config):
    """Bytes per interval record; must match IntervalLayout::size()."""
    return (
        22
        + 8 * len(config[CONF_INPUTS])
        + 8 * config[CONF_OUTPUT_SLOTS]
        + 2 * len(config[CONF_LINES])
        + 16
        + 4
    )


def ring_sectors(records, record_size):
    """Must match Ring::sectors_for()."""
    per_sector = SECTOR_SIZE // record_size
    return -(-records // per_sector) + 1


def partition_size(config, min1_records, min5_records):
    """Must match Journal::required_size()."""
    size = interval_record_size(config)
    sectors = (
        1
        + ring_sectors(EVENT_RECORDS, EVENT_RECORD_SIZE)
        + ring_sectors(min1_records, size)
        + ring_sectors(min5_records, size)
    )
    return sectors * SECTOR_SIZE


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
            # Interval records start on wall-clock boundaries once time is valid.
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # Record room for outputs. Changing the record layout (inputs,
            # lines or slots) starts a new journal, so leave spare slots for
            # outputs added later. Default: configured outputs + 4.
            cv.Optional(CONF_OUTPUT_SLOTS): cv.int_range(min=1, max=64),
            cv.Optional(
                CONF_MIN1_RETENTION, default="48h"
            ): cv.positive_time_period_seconds,
            cv.Optional(
                CONF_MIN5_RETENTION, default="30d"
            ): cv.positive_time_period_seconds,
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.GenerateID(CONF_WEB_HANDLER_ID): cv.declare_id(WebHandler),
            cv.Optional(CONF_PVOUTPUT): PVOUTPUT_SCHEMA,
        }
    ).extend(cv.polling_component_schema("10s")),
    cv.only_on_esp32,
    _validate_unique_names,
    _validate_output_slots,
    _validate_pvoutput,
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
    min1_records = -(-int(config[CONF_MIN1_RETENTION].total_seconds) // 60)
    min5_records = -(-int(config[CONF_MIN5_RETENTION].total_seconds) // 300)
    add_partition(
        PARTITION_NAME,
        "data",
        "undefined",
        partition_size(config, min1_records, min5_records),
    )

    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_OUTPUT_SLOTS],
        min1_records,
        min5_records,
        EVENT_RECORDS,
    )
    await cg.register_component(var, config)
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT]))
    cg.add(var.set_defs_hash(definitions_hash(config)))
    if CONF_TIME_ID in config:
        time_ = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(time_))

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    handler = cg.new_Pvariable(config[CONF_WEB_HANDLER_ID], base, var)
    await cg.register_component(handler, {})

    if pvo := config.get(CONF_PVOUTPUT):
        await _pvoutput_to_code(config, pvo, var)

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


async def _pvoutput_to_code(config, pvo, ledger):
    cg.add_define("USE_WATTLEDGER_PVOUTPUT")
    esp32.request_http_client()
    esp32.request_tls()
    esp32.require_certificate_bundle()

    outputs = [item[CONF_ID] for item in config[CONF_OUTPUTS]]
    lines = [item[CONF_ID] for item in config[CONF_LINES]]
    var = cg.new_Pvariable(pvo[CONF_ID], ledger)
    await cg.register_component(var, pvo)
    cg.add(var.set_api_key(pvo[CONF_API_KEY]))
    cg.add(var.set_system_id(pvo[CONF_SYSTEM_ID]))
    cg.add(var.set_donor(pvo[CONF_DONOR]))
    cg.add(var.set_generation(outputs.index(pvo[CONF_GENERATION])))
    if CONF_CONSUMPTION in pvo:
        cg.add(var.set_consumption(outputs.index(pvo[CONF_CONSUMPTION])))
    if CONF_VOLTAGE in pvo:
        cg.add(var.set_voltage(lines.index(pvo[CONF_VOLTAGE])))
    cg.add(var.set_reserve_requests(pvo[CONF_RESERVE_REQUESTS]))

    for key, setter in (
        (CONF_BACKLOG, var.set_backlog_sensor),
        (CONF_RATE_LIMIT_REMAINING, var.set_rate_limit_sensor),
        (CONF_LAST_UPLOAD, var.set_last_upload_sensor),
        (CONF_REPLAY_PROGRESS, var.set_replay_progress_sensor),
    ):
        if key in pvo:
            cg.add(setter(await sensor.new_sensor(pvo[key])))
    if CONF_LAST_ERROR in pvo:
        cg.add(var.set_last_error_sensor(await text_sensor.new_text_sensor(pvo[CONF_LAST_ERROR])))
    if CONF_REPLAY in pvo:
        btn = await button.new_button(pvo[CONF_REPLAY])
        await cg.register_parented(btn, var)
