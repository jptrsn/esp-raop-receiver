#include "audio_buffer.h"
#include "i2s_output.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "audio_buffer";

#define BUFFER_FRAMES 1024
#define MAX_FRAME_SIZE 2048
#define TIMING_THRESHOLD_MS 50  // Max drift before correction

typedef struct {
    uint8_t data[MAX_FRAME_SIZE];
    size_t len;
    uint32_t playtime;
    bool ready;
} audio_frame_t;

static struct {
    audio_frame_t *frames;
    uint32_t read_idx;
    uint32_t write_idx;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    bool running;
} audio_buf = {0};

static struct {
    uint32_t skip_frames;
    uint32_t pause_frames;
} timing_correction = {0, 0};

static uint32_t gettime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void audio_output_task(void *arg) {
    ESP_LOGI(TAG, "Audio output task started");

    while (audio_buf.running) {
        // Handle skip frames
        if (timing_correction.skip_frames > 0) {
            xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

            while (timing_correction.skip_frames > 0 && audio_buf.read_idx != audio_buf.write_idx) {
                audio_buf.frames[audio_buf.read_idx].ready = false;
                audio_buf.read_idx = (audio_buf.read_idx + 1) % BUFFER_FRAMES;
                timing_correction.skip_frames--;
            }

            xSemaphoreGive(audio_buf.mutex);
            continue;
        }

        // Handle pause frames (insert silence)
        if (timing_correction.pause_frames > 0) {
            uint8_t silence[MAX_FRAME_SIZE];
            memset(silence, 0, 1408); // 352 samples * 4 bytes
            i2s_output_write(silence, 1408);
            timing_correction.pause_frames--;
            continue;
        }

        // Normal playback
        audio_frame_t *frame = NULL;

        xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);
        if (audio_buf.read_idx != audio_buf.write_idx) {
            frame = &audio_buf.frames[audio_buf.read_idx];
            if (frame->ready) {
                uint8_t data[MAX_FRAME_SIZE];
                memcpy(data, frame->data, frame->len);
                size_t len = frame->len;

                frame->ready = false;
                audio_buf.read_idx = (audio_buf.read_idx + 1) % BUFFER_FRAMES;

                xSemaphoreGive(audio_buf.mutex);

                i2s_output_write(data, len);
            } else {
                xSemaphoreGive(audio_buf.mutex);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            xSemaphoreGive(audio_buf.mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    vTaskDelete(NULL);
}

void audio_buffer_init(void) {
    // Clean up if already initialized
    if (audio_buf.running) {
        audio_buffer_deinit();
    }

    memset(&audio_buf, 0, sizeof(audio_buf));

    // Allocate frame buffer in PSRAM
    audio_buf.frames = heap_caps_malloc(BUFFER_FRAMES * sizeof(audio_frame_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buf.frames) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer in PSRAM");
        return;
    }

    memset(audio_buf.frames, 0, BUFFER_FRAMES * sizeof(audio_frame_t));

    audio_buf.mutex = xSemaphoreCreateMutex();
    audio_buf.running = true;

    xTaskCreatePinnedToCore(
        audio_output_task,
        "audio_output",
        4096,
        NULL,
        3,
        &audio_buf.task,
        1
    );

    ESP_LOGI(TAG, "Audio buffer initialized");
}

bool audio_buffer_write(const uint8_t *data, size_t len, uint32_t playtime) {
    if (len > MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "Frame too large: %zu bytes", len);
        return false;
    }

    // Try up to 5 times with 10ms delays between attempts
    for (int retries = 0; retries < 5; retries++) {
        xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

        uint32_t next_write = (audio_buf.write_idx + 1) % BUFFER_FRAMES;

        // Check if buffer has space
        if (next_write != audio_buf.read_idx) {
            audio_frame_t *frame = &audio_buf.frames[audio_buf.write_idx];
            memcpy(frame->data, data, len);
            frame->len = len;
            frame->playtime = playtime;
            frame->ready = true;

            audio_buf.write_idx = next_write;

            xSemaphoreGive(audio_buf.mutex);
            return true;
        }

        xSemaphoreGive(audio_buf.mutex);

        // Buffer full, wait for I2S to drain
        if (retries < 4) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGW(TAG, "Buffer full after retries, dropping frame");
    return false;
}

void audio_buffer_flush(void) {
    xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

    // Mark all frames as not ready
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        audio_buf.frames[i].ready = false;
    }

    audio_buf.read_idx = 0;
    audio_buf.write_idx = 0;

    xSemaphoreGive(audio_buf.mutex);

    ESP_LOGI(TAG, "Buffer flushed");
}

void audio_buffer_deinit(void) {
    audio_buf.running = false;
    if (audio_buf.task) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (audio_buf.mutex) {
      vSemaphoreDelete(audio_buf.mutex);
    }
    if (audio_buf.frames) {
      free(audio_buf.frames);
      audio_buf.frames = NULL;
    }
}

void audio_buffer_get_timing(uint32_t *frames_buffered, uint32_t *head_playtime) {

    if (!audio_buf.frames || !audio_buf.mutex) {
        *frames_buffered = 0;
        *head_playtime = 0;
        return;
    }

    xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

    // Calculate how many frames are buffered
    if (audio_buf.write_idx >= audio_buf.read_idx) {
        *frames_buffered = audio_buf.write_idx - audio_buf.read_idx;
    } else {
        *frames_buffered = BUFFER_FRAMES - audio_buf.read_idx + audio_buf.write_idx;
    }

    // Get the playtime of the most recent frame
    if (*frames_buffered > 0) {
        uint32_t head_idx = (audio_buf.write_idx == 0) ? BUFFER_FRAMES - 1 : audio_buf.write_idx - 1;
        *head_playtime = audio_buf.frames[head_idx].playtime;
    } else {
        *head_playtime = 0;
    }

    xSemaphoreGive(audio_buf.mutex);
}

void audio_buffer_skip_frames(uint32_t count) {
    timing_correction.skip_frames = count;
    ESP_LOGI(TAG, "Will skip %u frames", count);
}

void audio_buffer_pause_frames(uint32_t count) {
    timing_correction.pause_frames = count;
    ESP_LOGI(TAG, "Will pause %u frames", count);
}

bool audio_buffer_is_ready(void) {
    return audio_buf.frames != NULL && audio_buf.running;
}