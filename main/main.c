#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "i2s_output.h"
#include "raop.h"
#include "esp_mac.h"
#include "lwip/udp.h"
#include "lwip/dns.h"

static const char *TAG = "main";

static httpd_handle_t web_server = NULL;
static struct raop_ctx_s *raop_ctx = NULL;
static struct udp_pcb *dns_pcb = NULL;

// RAOP command callback
static bool raop_cmd_handler(raop_event_t event, ...)
{
    va_list args;
    va_start(args, event);

    switch (event) {
        case RAOP_SETUP: {
            uint8_t **buffer = va_arg(args, uint8_t**);
            size_t *size = va_arg(args, size_t*);

            ESP_LOGI(TAG, "RAOP: Setup - audio stream starting");
            ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

            // Pre-allocate buffer for RTP (352 frames * 4 bytes * frame_size)
            // Allocate in PSRAM if available since it's large
            *size = 352 * 4 * 1024; // Allocate generous buffer
            *buffer = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

            if (*buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate audio buffer!");
                *buffer = NULL;
                *size = 0;
            } else {
                ESP_LOGI(TAG, "Allocated %zu byte audio buffer in PSRAM", *size);
            }
            break;
        }

        case RAOP_STREAM:
            ESP_LOGI(TAG, "RAOP: Stream started");
            break;

        case RAOP_STOP:
            ESP_LOGI(TAG, "RAOP: Stream stopped");
            break;

        case RAOP_FLUSH:
            ESP_LOGI(TAG, "RAOP: Flush requested");
            break;

        case RAOP_VOLUME: {
            float volume = va_arg(args, double); // float promoted to double in varargs
            ESP_LOGI(TAG, "RAOP: Volume changed to %.2f", volume);
            break;
        }

        case RAOP_PROGRESS: {
            int current = va_arg(args, int);
            int total = va_arg(args, int);
            ESP_LOGI(TAG, "RAOP: Progress %d/%d ms", current, total);
            break;
        }

        case RAOP_METADATA: {
            char *artist = va_arg(args, char*);
            char *album = va_arg(args, char*);
            char *title = va_arg(args, char*);
            uint32_t timestamp = va_arg(args, uint32_t);
            ESP_LOGI(TAG, "RAOP: Metadata (ts:%u)", timestamp);
            ESP_LOGI(TAG, "  Artist: %s", artist ? artist : "N/A");
            ESP_LOGI(TAG, "  Album:  %s", album ? album : "N/A");
            ESP_LOGI(TAG, "  Title:  %s", title ? title : "N/A");
            break;
        }

        case RAOP_ARTWORK: {
            uint8_t *data = va_arg(args, uint8_t*);
            size_t len = va_arg(args, size_t);
            uint32_t timestamp = va_arg(args, uint32_t);
            ESP_LOGI(TAG, "RAOP: Artwork received (%zu bytes, ts:%u)", len, timestamp);
            break;
        }

        default:
            ESP_LOGW(TAG, "RAOP: Unknown event %d", event);
            break;
    }

    va_end(args);
    return true;
}

// RAOP data callback - this is where audio PCM data arrives
static void raop_data_handler(uint8_t *data, size_t len)
{
    // Write audio data to I2S
    i2s_output_write(data, len);
}

static void start_mdns_service(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    char hostname[32];
    snprintf(hostname, sizeof(hostname), "esp-airplay-%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    mdns_hostname_set(hostname);
    mdns_instance_name_set("ESP AirPlay Receiver");

    ESP_LOGI(TAG, "mDNS hostname set to: %s.local", hostname);
}

static void start_airplay_receiver(void)
{
    uint8_t mac[6];
    char device_name[32];

    esp_efuse_mac_get_default(mac);
    snprintf(device_name, sizeof(device_name), "ESP-AirPlay-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    // Get local IP
    uint32_t ip = 0;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }

    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ip = ip_info.ip.addr;
        }
    }

    if (ip == 0) {
        ESP_LOGE(TAG, "Failed to get IP address for AirPlay");
        return;
    }

    ESP_LOGI(TAG, "Starting AirPlay receiver on IP: %d.%d.%d.%d",
         (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
         (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));

    // Create RAOP context with 88200 frames latency (2 seconds at 44.1kHz)
    raop_ctx = raop_create(ip, device_name, mac, 88200,
                          raop_cmd_handler, raop_data_handler);

    if (raop_ctx) {
        ESP_LOGI(TAG, "AirPlay receiver started successfully");
        ESP_LOGI(TAG, "Device name: %s", device_name);
    } else {
        ESP_LOGE(TAG, "Failed to start AirPlay receiver");
    }
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

    // Initialize mDNS
    start_mdns_service();

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