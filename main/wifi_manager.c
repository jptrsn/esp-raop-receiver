#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "wifi_manager";

#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASS_KEY "wifi_pass"
#define WIFI_NAMESPACE "wifi_config"

#define AP_SSID_PREFIX "ESPAIRPLAY-"
#define AP_PASSWORD "espairplay"

static bool s_wifi_connected = false;
static char s_ip_address[16] = "";
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_ap_mode = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected to AP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        s_wifi_connected = false;
        strcpy(s_ip_address, "");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_address);
        s_wifi_connected = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station %02X:%02X:%02X:%02X:%02X:%02X joined AP, AID=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGE(TAG, "Station %02X:%02X:%02X:%02X:%02X:%02X left AP, AID=%d, reason=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5],
                 event->aid, event->reason);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started successfully");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t* event = (ip_event_ap_staipassigned_t*) event_data;
        ESP_LOGI(TAG, "DHCP assigned IP " IPSTR " to station", IP2STR(&event->ip));
    } else {
        ESP_LOGW(TAG, "Unhandled WiFi event: base=%s, id=%ld",
                 event_base == WIFI_EVENT ? "WIFI_EVENT" : "IP_EVENT", event_id);
    }
}

static bool load_wifi_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved WiFi credentials found: %s", esp_err_to_name(err));
        return false;
    }

    size_t ssid_len = 32;
    size_t pass_len = 64;

    err = nvs_get_str(nvs_handle, WIFI_SSID_KEY, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_str(nvs_handle, WIFI_PASS_KEY, password, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Loaded WiFi credentials:");
    ESP_LOGI(TAG, "  SSID: '%s' (len=%d)", ssid, strlen(ssid));

    return true;
}

static void start_ap_mode(void)
{
    uint8_t mac[6];
    char ap_ssid[32];

    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X%02X",
             AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 3,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            // .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pairwise_cipher = WIFI_CIPHER_TYPE_NONE,
            .beacon_interval = 100,
            .pmf_cfg = {
                .required = false,
                .capable = false,
            },
        },
    };

    // Copy SSID
    strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ap_ssid);

    // Copy password explicitly
    // strncpy((char *)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password) - 1);

    // Set WiFi country code
    wifi_country_t country = {
        .cc = "US",
        .schan = 1,
        .nchan = 11,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_LOGI(TAG, "Starting AP with SSID: '%s' (len=%d), Password: (len=%d)",
             (char*)wifi_config.ap.ssid, wifi_config.ap.ssid_len,
             (char*)wifi_config.ap.password, strlen((char*)wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_mode = true;

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Started in AP mode, connect to WiFi:");
    ESP_LOGI(TAG, "  SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "  Config page: http://192.168.4.1");
    ESP_LOGI(TAG, "===========================================");
}

static void start_sta_mode(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};

    strcpy((char *)sta_config.sta.ssid, ssid);
    strcpy((char *)sta_config.sta.password, password);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
}

void wifi_manager_init(void)
{
    ESP_LOGI(TAG, "=== WiFi Manager Init START ===");

    esp_err_t ret = nvs_flash_init();
    ESP_LOGI(TAG, "NVS init result: %d", ret);

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing TCP/IP stack...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "Creating default event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing WiFi with default config...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Explicitly clear any stale WiFi state from previous crashed session
    ESP_LOGI(TAG, "Clearing any stale WiFi associations...");
    esp_wifi_set_mode(WIFI_MODE_NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Registering event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL));

    char ssid[32] = {0};
    char password[64] = {0};

    ESP_LOGI(TAG, "Checking for saved WiFi credentials...");
    if (!load_wifi_credentials(ssid, password)) {
        ESP_LOGI(TAG, "No credentials found, starting AP mode");

        // Create AP netif only when needed
        s_ap_netif = esp_netif_create_default_wifi_ap();

        start_ap_mode();
    } else {
        s_ap_mode = false;
        ESP_LOGI(TAG, "Credentials found, connecting to WiFi: %s", ssid);

        // Create STA netif only when needed
        s_sta_netif = esp_netif_create_default_wifi_sta();

        // Stop DHCP if it's running (shouldn't be, but just in case)
        esp_netif_dhcpc_stop(s_sta_netif);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Check DHCP status first
        esp_netif_dhcp_status_t dhcp_status;
        esp_netif_dhcpc_get_status(s_sta_netif, &dhcp_status);
        ESP_LOGI(TAG, "Current DHCP status: %d", dhcp_status);

        if (dhcp_status == ESP_NETIF_DHCP_STOPPED) {
            ESP_ERROR_CHECK(esp_netif_dhcpc_start(s_sta_netif));
            ESP_LOGI(TAG, "DHCP client started");
        } else if (dhcp_status == ESP_NETIF_DHCP_STARTED) {
            ESP_LOGI(TAG, "DHCP already running");
        } else {
            // Stop and restart to clear any stale state
            esp_netif_dhcpc_stop(s_sta_netif);
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_ERROR_CHECK(esp_netif_dhcpc_start(s_sta_netif));
            ESP_LOGI(TAG, "DHCP restarted");
        }

        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        // Disconnect any stale connections BEFORE setting new config
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    ESP_LOGI(TAG, "=== WiFi Manager Init COMPLETE ===");
}

bool wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    ESP_LOGI(TAG, "Saving credentials:");
    ESP_LOGI(TAG, "  SSID: '%s' (len=%d)", ssid, strlen(ssid));
    ESP_LOGI(TAG, "  Pass: '%s' (len=%d)", password, strlen(password));

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs_handle, WIFI_SSID_KEY, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_set_str(nvs_handle, WIFI_PASS_KEY, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved successfully");
        return true;
    }

    ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    return false;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}

const char* wifi_manager_get_ip(void)
{
    return s_ip_address;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}