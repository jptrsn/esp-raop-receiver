#include "config_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "config_manager";

#define DEVICE_NAMESPACE  "device_config"
#define DEVICE_NAME_KEY   "device_name"

#define I2S_NAMESPACE     "i2s_config"
#define I2S_BCK_PIN_KEY   "bck_pin"
#define I2S_WS_PIN_KEY    "ws_pin"
#define I2S_DOUT_PIN_KEY  "dout_pin"

#define WLED_NAMESPACE    "wled_config"
#define WLED_ENABLED_KEY  "enabled"
#define WLED_HOST_KEY     "host"
#define WLED_PORT_KEY     "port"
#define WLED_CHANNEL_KEY  "channel"

static void generate_default_device_name(char *buf, size_t buf_len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, buf_len, "ESP-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static bool validate_device_name(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > CONFIG_DEVICE_NAME_MAX_LEN) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t config_manager_load(app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // --- Device name ---
    nvs_handle_t device_nvs;
    esp_err_t err = nvs_open(DEVICE_NAMESPACE, NVS_READONLY, &device_nvs);
    if (err == ESP_OK) {
        size_t len = sizeof(config->device_name);
        err = nvs_get_str(device_nvs, DEVICE_NAME_KEY, config->device_name, &len);
        nvs_close(device_nvs);
    }

    if (err != ESP_OK || strlen(config->device_name) == 0) {
        generate_default_device_name(config->device_name, sizeof(config->device_name));
        ESP_LOGI(TAG, "Using default device name: %s", config->device_name);
    } else {
        ESP_LOGI(TAG, "Loaded device name: %s", config->device_name);
    }

    // --- I2S pins ---
    nvs_handle_t i2s_nvs;
    err = nvs_open(I2S_NAMESPACE, NVS_READONLY, &i2s_nvs);
    if (err == ESP_OK) {
        int32_t val;

        val = CONFIG_DEFAULT_BCK_PIN;
        nvs_get_i32(i2s_nvs, I2S_BCK_PIN_KEY, &val);
        config->bck_pin = (int)val;

        val = CONFIG_DEFAULT_WS_PIN;
        nvs_get_i32(i2s_nvs, I2S_WS_PIN_KEY, &val);
        config->ws_pin = (int)val;

        val = CONFIG_DEFAULT_DOUT_PIN;
        nvs_get_i32(i2s_nvs, I2S_DOUT_PIN_KEY, &val);
        config->dout_pin = (int)val;

        nvs_close(i2s_nvs);
    } else {
        config->bck_pin  = CONFIG_DEFAULT_BCK_PIN;
        config->ws_pin   = CONFIG_DEFAULT_WS_PIN;
        config->dout_pin = CONFIG_DEFAULT_DOUT_PIN;
        ESP_LOGI(TAG, "Using default I2S pins: BCK=%d WS=%d DOUT=%d",
                 config->bck_pin, config->ws_pin, config->dout_pin);
    }

    // --- WLED sync ---
    nvs_handle_t wled_nvs;
    err = nvs_open(WLED_NAMESPACE, NVS_READONLY, &wled_nvs);
    ESP_LOGI(TAG, "WLED NVS open result: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        uint8_t enabled = 0;
        nvs_get_u8(wled_nvs, WLED_ENABLED_KEY, &enabled);
        config->wled_enabled = (bool)enabled;
        ESP_LOGI(TAG, "Loaded WLED: enabled=%d host=%s port=%u",
                config->wled_enabled, config->wled_host, config->wled_port);

        size_t host_len = sizeof(config->wled_host);
        if (nvs_get_str(wled_nvs, WLED_HOST_KEY, config->wled_host, &host_len) != ESP_OK) {
            strlcpy(config->wled_host, WLED_DEFAULT_HOST, sizeof(config->wled_host));
        }

        uint16_t port = WLED_DEFAULT_PORT;
        nvs_get_u16(wled_nvs, WLED_PORT_KEY, &port);
        config->wled_port = port;

        uint8_t channel = 0;
        nvs_get_u8(wled_nvs, WLED_CHANNEL_KEY, &channel);
        config->wled_channel = channel;

        nvs_close(wled_nvs);
    } else {
        ESP_LOGI(TAG, "NVS open error! Failed to read WLED NVS");
        config->wled_enabled = false;
        strlcpy(config->wled_host, WLED_DEFAULT_HOST, sizeof(config->wled_host));
        config->wled_port    = WLED_DEFAULT_PORT;
        config->wled_channel = 0;
    }

    return ESP_OK;
}

esp_err_t config_manager_save_device(const char *device_name)
{
    if (!validate_device_name(device_name)) {
        ESP_LOGE(TAG, "Invalid device name: '%s'", device_name ? device_name : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(DEVICE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, DEVICE_NAME_KEY, device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device name: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved device name: %s", device_name);
    } else {
        ESP_LOGE(TAG, "Failed to commit device name: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_save_audio(int bck_pin, int ws_pin, int dout_pin)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(I2S_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(nvs_handle, I2S_BCK_PIN_KEY, (int32_t)bck_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save BCK pin: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_i32(nvs_handle, I2S_WS_PIN_KEY, (int32_t)ws_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WS pin: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_i32(nvs_handle, I2S_DOUT_PIN_KEY, (int32_t)dout_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save DOUT pin: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved I2S pins: BCK=%d WS=%d DOUT=%d", bck_pin, ws_pin, dout_pin);
    } else {
        ESP_LOGE(TAG, "Failed to commit I2S pins: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_save_wled(bool enabled, const char *host,
                                   uint16_t port, uint8_t channel)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WLED_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WLED NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(nvs_handle,  WLED_ENABLED_KEY,  (uint8_t)enabled);
    nvs_set_str(nvs_handle, WLED_HOST_KEY,      host);
    nvs_set_u16(nvs_handle, WLED_PORT_KEY,      port);
    nvs_set_u8(nvs_handle,  WLED_CHANNEL_KEY,   channel);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved WLED config: enabled=%d host=%s port=%u channel=%u",
                 enabled, host, port, channel);
    }
    return err;
}