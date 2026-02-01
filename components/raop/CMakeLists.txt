idf_component_register(
    SRCS "raop.c" "rtp.c" "util.c" "dmap_parser.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer esp_netif mdns mbedtls lwip pthread
)

# Add ALAC include path
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/components/codecs/inc/alac"
)

# Link ALAC library
target_link_libraries(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/components/codecs/lib/libalac.a"
)