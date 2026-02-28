import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import speaker, i2s_audio
from esphome.const import CONF_ID
from esphome.core import CORE

from . import raop_receiver_ns

CODEOWNERS = ["@jptrsn"]
DEPENDENCIES = ["esp32", "wifi", "psram", "mdns", "i2s_audio"]

# Must use ESP-IDF framework
def validate_framework(config):
    if CORE.using_arduino:
        raise cv.Invalid("RAOP receiver requires ESP-IDF framework")
    return config

# Our C++ class
RAOPSpeaker = raop_receiver_ns.class_(
    "RAOPSpeaker",
    i2s_audio.I2SAudioOut,
    speaker.Speaker,
    cg.Component
)

CONF_DEVICE_NAME = "device_name"
CONF_I2S_DOUT_PIN = "i2s_dout_pin"

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(RAOPSpeaker),
            cv.Required(CONF_I2S_DOUT_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_DEVICE_NAME): cv.string,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2s_audio.i2s_audio_component_schema(
        RAOPSpeaker,
        default_sample_rate=44100,
        default_channel="stereo",
        default_bits_per_sample="16bit"
    )),
    validate_framework,
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    await i2s_audio.register_i2s_audio_component(var, config)

    cg.add(var.set_dout_pin(config[CONF_I2S_DOUT_PIN]))

    # Optional device name
    if CONF_DEVICE_NAME in config:
        cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))