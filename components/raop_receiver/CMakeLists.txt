idf_component_register(
    SRCS "raop_speaker.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES i2s_audio speaker
)

# Link the precompiled RAOP library
target_link_libraries(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libesp-raop-receiver.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/libalac.a"
)