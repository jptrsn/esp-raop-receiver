#include "i2s_output.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s_output";

#define I2S_NAMESPACE "i2s_config"
#define I2S_BCK_PIN_KEY "bck_pin"
#define I2S_WS_PIN_KEY "ws_pin"
#define I2S_DOUT_PIN_KEY "dout_pin"

// Default pins (as per our pin assignments)
#define DEFAULT_BCK_PIN   (GPIO_NUM_26)
#define DEFAULT_WS_PIN    (GPIO_NUM_25)
#define DEFAULT_DOUT_PIN  (GPIO_NUM_22)

static i2s_chan_handle_t tx_handle = NULL;

static int load_pin_config(const char *key, int default_value)
{
    nvs_handle_t nvs_handle;
    int32_t pin_value = default_value;

    esp_err_t err = nvs_open(I2S_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        nvs_get_i32(nvs_handle, key, &pin_value);
        nvs_close(nvs_handle);
    }

    return (int)pin_value;
}

void i2s_output_init(void)
{
    esp_err_t ret;

    // Load pin configuration from NVS (or use defaults)
    int bck_pin = load_pin_config(I2S_BCK_PIN_KEY, DEFAULT_BCK_PIN);
    int ws_pin = load_pin_config(I2S_WS_PIN_KEY, DEFAULT_WS_PIN);
    int dout_pin = load_pin_config(I2S_DOUT_PIN_KEY, DEFAULT_DOUT_PIN);

    ESP_LOGI(TAG, "Initializing I2S with pins - BCK: %d, WS: %d, DOUT: %d",
             bck_pin, ws_pin, dout_pin);

    // Configure I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear DMA buffer on underflow

    ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return;
    }

    // Configure I2S standard mode (Philips standard)
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 44100,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bck_pin,
            .ws = ws_pin,
            .dout = dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return;
    }

    ESP_LOGI(TAG, "I2S initialized successfully");
}

void i2s_output_write(const uint8_t *data, size_t len)
{
    if (!tx_handle) {
        ESP_LOGW(TAG, "I2S not initialized");
        return;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, data, len, &bytes_written, portMAX_DELAY);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    } else if (bytes_written != len) {
        ESP_LOGW(TAG, "I2S partial write: %d/%d bytes", bytes_written, len);
    }
}

void i2s_output_deinit(void)
{
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        ESP_LOGI(TAG, "I2S deinitialized");
    }
}