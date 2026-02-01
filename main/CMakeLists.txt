idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_server
        mdns
        driver
        raop
        wifi_provisioning
        audio_buffer
        i2s_output
)