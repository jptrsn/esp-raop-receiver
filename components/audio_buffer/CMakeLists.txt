idf_component_register(
    SRCS "audio_buffer.c"
    INCLUDE_DIRS "."
    REQUIRES i2s_output esp_timer freertos
)