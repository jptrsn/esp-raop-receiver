idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c" "config_manager.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web_config.html"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_server
        driver
        wifi_provisioning
        i2s_output
        esp-raop-receiver
        esp_system
        app_update
)