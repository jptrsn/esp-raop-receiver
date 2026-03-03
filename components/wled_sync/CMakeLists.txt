idf_component_register(
    SRCS "wled_sync.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        lwip
    PRIV_REQUIRES
        espressif__esp-dsp
)