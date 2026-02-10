#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "i2s_output.h"
#include "esp_raop_receiver.h"
#include "esp_mac.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "esp_netif.h"

static const char *TAG = "main";

static httpd_handle_t web_server = NULL;
static raop_handle_t *raop_handle = NULL;
static struct udp_pcb *dns_pcb = NULL;

// Audio output callback - forwards to I2S hardware
static void audio_output_callback(const uint8_t *data, size_t len, void *user_ctx)
{
    i2s_output_write(data, len);
}

static void raop_event_handler(raop_event_t event, void *event_data, void *user_ctx)
{
    switch (event) {
        case RAOP_EVENT_CONNECTED:
            ESP_LOGI(TAG, "AirPlay: client connected");
            break;
        case RAOP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "AirPlay: client disconnected");
            break;
        case RAOP_EVENT_BUFFERING:
            ESP_LOGI(TAG, "AirPlay: buffering");
            break;
        case RAOP_EVENT_PLAYING:
            ESP_LOGI(TAG, "AirPlay: playing");
            break;
        case RAOP_EVENT_STOPPED:
            ESP_LOGI(TAG, "AirPlay: stopped");
            break;
        case RAOP_EVENT_VOLUME: {
            float volume = *(float *)event_data;
            ESP_LOGI(TAG, "AirPlay: volume %.2f", volume);
            i2s_output_set_volume(volume);
            break;
        }
        case RAOP_EVENT_METADATA: {
            raop_metadata_t *meta = (raop_metadata_t *)event_data;
            ESP_LOGI(TAG, "AirPlay: %s - %s", meta->artist, meta->title);
            break;
        }
        case RAOP_EVENT_ARTWORK: {
            raop_artwork_t *art = (raop_artwork_t *)event_data;
            ESP_LOGI(TAG, "AirPlay: artwork %zu bytes", art->len);
            break;
        }
        case RAOP_EVENT_PROGRESS: {
            raop_progress_t *progress = (raop_progress_t *)event_data;
            ESP_LOGI(TAG, "AirPlay: progress %lu/%lu ms",
                     progress->current_ms, progress->total_ms);
            break;
        }
        case RAOP_EVENT_STALLED:
            ESP_LOGW(TAG, "AirPlay: stream stalled");
            break;
        default:
            break;
    }
}

static void start_airplay_receiver(void)
{
    raop_config_t config = {
        .volume_mode    = RAOP_VOLUME_SOFTWARE,
        .mdns_mode      = RAOP_MDNS_MANAGED,
        .audio_output_cb = audio_output_callback,
        .event_cb       = raop_event_handler,
    };

    esp_err_t err = raop_init(&config, &raop_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AirPlay: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "AirPlay receiver started: %s", raop_get_device_name(raop_handle));
}

static void dns_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
    if (p->len < 12) {
        pbuf_free(p);
        return;
    }

    // Simple DNS response - redirect everything to 192.168.4.1
    uint8_t *dns_query = (uint8_t *)p->payload;

    // Build DNS response
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, p->len + 16, PBUF_RAM);
    if (out) {
        uint8_t *resp = (uint8_t *)out->payload;

        // Copy query header
        memcpy(resp, dns_query, 12);

        // Set response flags
        resp[2] = 0x81; // Response, no error
        resp[3] = 0x80;
        resp[7] = 0x01; // 1 answer

        // Copy question section
        memcpy(resp + 12, dns_query + 12, p->len - 12);

        // Add answer section (point to our IP: 192.168.4.1)
        uint16_t offset = p->len;
        resp[offset++] = 0xc0; // Pointer to name
        resp[offset++] = 0x0c;
        resp[offset++] = 0x00; // Type A
        resp[offset++] = 0x01;
        resp[offset++] = 0x00; // Class IN
        resp[offset++] = 0x01;
        resp[offset++] = 0x00; // TTL
        resp[offset++] = 0x00;
        resp[offset++] = 0x00;
        resp[offset++] = 0x3c;
        resp[offset++] = 0x00; // Data length
        resp[offset++] = 0x04;
        resp[offset++] = 192;  // IP: 192.168.4.1
        resp[offset++] = 168;
        resp[offset++] = 4;
        resp[offset++] = 1;

        udp_sendto(pcb, out, addr, port);
        pbuf_free(out);
    }

    pbuf_free(p);
}

static void start_dns_server(void)
{
    dns_pcb = udp_new();
    if (dns_pcb) {
        udp_bind(dns_pcb, IP_ADDR_ANY, 53);
        udp_recv(dns_pcb, dns_recv_callback, NULL);
        ESP_LOGI(TAG, "DNS server started on port 53");
    }
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "ESP AirPlay Receiver starting...");

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize I2S output
    i2s_output_init();

    // Initialize WiFi
    ESP_LOGI(TAG, "About to initialize WiFi manager...");
    wifi_manager_init();
    ESP_LOGI(TAG, "WiFi manager initialized");

    if (wifi_manager_is_ap_mode()) {
        start_dns_server();
        ESP_LOGI(TAG, "Captive portal DNS server started (AP mode)");
    } else {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        int retry = 0;
        while (!wifi_manager_is_connected() && retry < 240) {
            vTaskDelay(pdMS_TO_TICKS(500));
            retry++;

            if (retry % 4 == 0) {  // Log every 2 seconds
                ESP_LOGI(TAG, "Still waiting for IP... (%d/60)", retry/2);
                // Manually check if IP was assigned
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                        ESP_LOGE(TAG, "DHCP assigned IP but event never fired! IP: " IPSTR, IP2STR(&ip_info.ip));
                        // Manually set connected state
                        // We need to add a function to do this
                        break;
                    }
                }
            }

            if (!wifi_manager_is_connected()) {
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    if (ip_info.ip.addr != 0) {
                        ESP_LOGI(TAG, "DHCP assigned IP but event didn't fire! IP: " IPSTR, IP2STR(&ip_info.ip));
                    }
                }
            }
        }
    }

    // Start web server for configuration
    web_server = web_server_start();

    // Wait for WiFi connection (give it some time)
    ESP_LOGI(TAG, "Waiting for network...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Wait for WiFi connection with proper timeout
    if (!wifi_manager_is_ap_mode()) {

        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "Connected! IP: %s", wifi_manager_get_ip());

            // Give network a moment to stabilize
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Now start AirPlay receiver
            start_airplay_receiver();
        } else {
            ESP_LOGE(TAG, "Failed to connect to WiFi after 30 seconds");
        }
    }

    ESP_LOGI(TAG, "System ready!");

    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "Connected to WiFi");
        ESP_LOGI(TAG, "IP Address: %s", wifi_manager_get_ip());
        ESP_LOGI(TAG, "Ready to receive AirPlay streams!");
    }

    // Main loop - could add status monitoring here
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Periodic status log
        ESP_LOGI(TAG, "Status: WiFi=%s, IP=%s",
                 wifi_manager_is_connected() ? "Connected" : "Disconnected",
                 wifi_manager_get_ip());
    }
}