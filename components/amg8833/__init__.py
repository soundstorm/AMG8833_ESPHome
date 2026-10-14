import inspect

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.automation import maybe_simple_id
from esphome.components import binary_sensor, i2c, sensor, text_sensor, web_server_base
from esphome.const import (
    CONF_ID,
    CONF_TRIGGER_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "web_server_base"]
CODEOWNERS = []

from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID  # noqa: E402
CONF_IMAGE_ENDPOINT = "image_endpoint"
CONF_BMP = "bmp"
CONF_SVG = "svg"
CONF_VIEWER = "viewer"
CONF_SCALE = "scale"
CONF_MAX_TEMPERATURE = "max_temperature"
CONF_FRAME = "frame"
CONF_FRAME_MIN = "frame_min_temperature"
CONF_FRAME_MAX = "frame_max_temperature"
CONF_FRAME_INTERVAL = "frame_interval"
CONF_PRESENCE = "presence"
CONF_COUNTING = "people_counter"
CONF_AXIS = "axis"
CONF_INVERT = "invert"
CONF_WARM_DELTA = "warm_delta"
CONF_BACKGROUND_RATE = "background_rate"
CONF_AUTO_RECALIBRATE_AFTER = "auto_recalibrate_after"
CONF_MIN_PIXELS = "min_pixels"
CONF_MIN_TRAVEL = "min_travel"
CONF_RELEASE_FRAMES = "release_frames"
CONF_ENTERED = "entered"
CONF_EXITED = "exited"
CONF_COUNT = "count"
CONF_ON_ENTER = "on_enter"
CONF_ON_EXIT = "on_exit"

amg8833_ns = cg.esphome_ns.namespace("amg8833")
AMG8833Component = amg8833_ns.class_(
    "AMG8833Component", cg.PollingComponent, i2c.I2CDevice
)
EnterTrigger = amg8833_ns.class_("EnterTrigger", automation.Trigger.template())
ExitTrigger = amg8833_ns.class_("ExitTrigger", automation.Trigger.template())
RecalibrateAction = amg8833_ns.class_("RecalibrateAction", automation.Action)
Axis = amg8833_ns.enum("Axis")
AXES = {"columns": Axis.AXIS_COLUMNS, "rows": Axis.AXIS_ROWS}

counter_sensor = sensor.sensor_schema(
    accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING, icon="mdi:counter"
)

COUNTING_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_AXIS, default="columns"): cv.enum(AXES, lower=True),
        cv.Optional(CONF_INVERT, default=False): cv.boolean,
        cv.Optional(CONF_WARM_DELTA, default=2.0): cv.float_range(min=0.5, max=20),
        cv.Optional(CONF_BACKGROUND_RATE, default=0.005): cv.float_range(min=0, max=0.1),
        cv.Optional(CONF_AUTO_RECALIBRATE_AFTER, default="2min"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MIN_PIXELS, default=3): cv.int_range(min=1, max=64),
        cv.Optional(CONF_MIN_TRAVEL, default=2.0): cv.float_range(min=0.5, max=7),
        cv.Optional(CONF_RELEASE_FRAMES, default=3): cv.int_range(min=1, max=100),
        cv.Optional(CONF_ON_ENTER): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EnterTrigger)}
        ),
        cv.Optional(CONF_ON_EXIT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ExitTrigger)}
        ),
        cv.Optional(CONF_ENTERED): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING, icon="mdi:login"
        ),
        cv.Optional(CONF_EXITED): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING, icon="mdi:logout"
        ),
        cv.Optional(CONF_COUNT): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:account-multiple"
        ),
    }
)

def _validate_image(config):
    if not (config[CONF_BMP] or config[CONF_SVG]):
        raise cv.Invalid("image_endpoint: at least one of 'bmp' or 'svg' must be enabled")
    return config


IMAGE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_BMP, default=True): cv.boolean,  # /thermal.bmp
            cv.Optional(CONF_SVG, default=False): cv.boolean,  # /thermal.svg
            cv.Optional(CONF_VIEWER, default=False): cv.boolean,  # /thermal (HTML page)
            cv.Optional(CONF_SCALE, default=8): cv.int_range(min=1, max=16),
        }
    ),
    _validate_image,
)

temp_schema = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AMG8833Component),
            cv.Optional(CONF_TEMPERATURE): temp_schema,
            cv.Optional(CONF_MAX_TEMPERATURE): temp_schema,
            cv.Optional(CONF_FRAME): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_FRAME_MIN, default=10.0): cv.temperature,
            cv.Optional(CONF_FRAME_MAX, default=40.0): cv.temperature,
            cv.Optional(CONF_FRAME_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
                device_class="occupancy"
            ),
            cv.Optional(CONF_COUNTING): COUNTING_SCHEMA,
            cv.Optional(CONF_IMAGE_ENDPOINT): IMAGE_SCHEMA,
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
        }
    )
    .extend(cv.polling_component_schema("100ms"))
    .extend(i2c.i2c_device_schema(0x69))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_TEMPERATURE in config:
        cg.add(var.set_thermistor_sensor(await sensor.new_sensor(config[CONF_TEMPERATURE])))
    if CONF_MAX_TEMPERATURE in config:
        cg.add(var.set_max_temperature_sensor(await sensor.new_sensor(config[CONF_MAX_TEMPERATURE])))
    if CONF_FRAME in config:
        cg.add(var.set_frame_text_sensor(await text_sensor.new_text_sensor(config[CONF_FRAME])))
    cg.add(var.set_frame_range(config[CONF_FRAME_MIN], config[CONF_FRAME_MAX]))
    cg.add(var.set_frame_interval(config[CONF_FRAME_INTERVAL]))
    if CONF_PRESENCE in config:
        cg.add(var.set_presence_sensor(await binary_sensor.new_binary_sensor(config[CONF_PRESENCE])))

    if CONF_COUNTING in config:
        c = config[CONF_COUNTING]
        cg.add(var.set_counting(c[CONF_AXIS], c[CONF_INVERT], c[CONF_WARM_DELTA],
                                c[CONF_MIN_PIXELS], c[CONF_MIN_TRAVEL], c[CONF_RELEASE_FRAMES],
                                c[CONF_BACKGROUND_RATE], c[CONF_AUTO_RECALIBRATE_AFTER]))
        for conf in c.get(CONF_ON_ENTER, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
        for conf in c.get(CONF_ON_EXIT, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
        if CONF_ENTERED in c:
            cg.add(var.set_entered_sensor(await sensor.new_sensor(c[CONF_ENTERED])))
        if CONF_EXITED in c:
            cg.add(var.set_exited_sensor(await sensor.new_sensor(c[CONF_EXITED])))
        if CONF_COUNT in c:
            cg.add(var.set_count_sensor(await sensor.new_sensor(c[CONF_COUNT])))

    if CONF_IMAGE_ENDPOINT in config:
        img = config[CONF_IMAGE_ENDPOINT]
        cg.add(var.set_image_endpoint(img[CONF_BMP], img[CONF_SVG], img[CONF_VIEWER], img[CONF_SCALE]))
        base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
        cg.add(var.set_web_server_base(base))


# play() finishes immediately, so the action is synchronous. Older ESPHome versions
# do not know the `synchronous` parameter yet.
_ACTION_KWARGS = (
    {"synchronous": True}
    if "synchronous" in inspect.signature(automation.register_action).parameters
    else {}
)


@automation.register_action(
    "amg8833.recalibrate",
    RecalibrateAction,
    maybe_simple_id({cv.GenerateID(): cv.use_id(AMG8833Component)}),
    **_ACTION_KWARGS,
)
async def recalibrate_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
