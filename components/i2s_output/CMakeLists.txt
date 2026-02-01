idf_component_register(
    SRCS "i2s_output.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash
)