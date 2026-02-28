#include "i2s_output.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s_output";
static float current_volume = 1.0f;
static int16_t *volume_buffer = NULL;
static size_t volume_buffer_size = 0;

static i2s_chan_handle_t tx_handle = NULL;

void i2s_output_set_volume(float volume)
{
    current_volume = (volume < 0.0f) ? 0.0f : (volume > 1.0f) ? 1.0f : volume;
    ESP_LOGI(TAG, "Volume set to %.2f (%d%%)", current_volume, (int)(current_volume * 100));
}

void i2s_output_init(int bck_pin, int ws_pin, int dout_pin)
{
    esp_err_t ret;

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

    // Ensure buffer is large enough (allocate once, reuse)
    if (volume_buffer_size < len) {
        if (volume_buffer) free(volume_buffer);
        volume_buffer = malloc(len);
        if (!volume_buffer) {
            ESP_LOGE(TAG, "Failed to allocate volume buffer");
            return;
        }
        volume_buffer_size = len;
    }

    const uint8_t *output_data;

    // Skip scaling if at full volume
    if (current_volume >= 0.99f) {
        output_data = data;
    } else {
        // Apply volume scaling
        int16_t *samples = (int16_t *)data;
        size_t sample_count = len / sizeof(int16_t);

        for (size_t i = 0; i < sample_count; i++) {
            int32_t scaled_sample = (int32_t)(samples[i] * current_volume);
            if (scaled_sample > 32767) scaled_sample = 32767;
            if (scaled_sample < -32768) scaled_sample = -32768;
            volume_buffer[i] = (int16_t)scaled_sample;
        }
        output_data = (uint8_t *)volume_buffer;
    }

    // if (call_count++ % 100 == 0) {
    //     ESP_LOGI(TAG, "Writing %zu bytes to I2S (call #%lu)",
    //              len, call_count);
    // }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, output_data, len, &bytes_written, portMAX_DELAY);

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