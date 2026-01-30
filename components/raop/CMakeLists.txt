idf_component_register(
    SRCS "raop.c" "rtp.c" "dmap_parser.c" "util.c" "raop_sink.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_netif esp_http_server mbedtls lwip esp_timer
    REQUIRES mdns
)