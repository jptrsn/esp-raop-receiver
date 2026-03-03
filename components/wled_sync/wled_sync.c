#include "wled_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_dsp.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <math.h>

static const char *TAG = "wled_sync";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define FFT_SIZE          1024
#define WLED_NUM_BINS     16
#define WLED_UDP_SIZE     44
#define WLED_PORT_DEFAULT 11988
#define SENDER_QUEUE_DEPTH 16
#define SENDER_TASK_STACK  8192
#define SENDER_TASK_PRIO   2

// WLED V2 audio sync packet (packed, 44 bytes)
typedef struct __attribute__((packed)) {
    char     header[6];
    uint8_t  reserved1[2];
    float    sampleRaw;
    float    sampleSmth;
    uint8_t  samplePeak;
    uint8_t  reserved2;
    uint8_t  fftResult[16];
    uint16_t reserved3;
    float    fftMagnitude;
    float    fftMajorPeak;
} wled_udp_packet_t;

_Static_assert(sizeof(wled_udp_packet_t) == WLED_UDP_SIZE,
               "WLED packet size mismatch");

// ---------------------------------------------------------------------------
// Pending send slot
// ---------------------------------------------------------------------------

typedef struct {
    wled_udp_packet_t packet;
} pending_send_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct {
    wled_sync_config_t  config;

    // Double-buffered PCM accumulation
    // Core 0 (RTP task) writes to sample_buf[active_buf] via the tap.
    // When full, the FFT task is signalled and active_buf is flipped.
    // Core 1 (FFT task) reads from the buffer index passed via fft_buf_index.
    float               *sample_buf[2];
    int                 active_buf;
    int                 sample_count;

    // FFT workspace — only touched by the FFT task on Core 1.
    // Heap-allocated to avoid blowing the task stack (8KB).
    float               *fft_buf;

    // Hann window coefficients — computed once at init, never modified.
    float               wind[FFT_SIZE];

    // Semaphore for FFT task handoff — carries the buffer index to process.
    QueueHandle_t       fft_queue;   // replaces fft_ready semaphore
    TaskHandle_t        fft_task;
    TaskHandle_t        sender_task;
    QueueHandle_t       send_queue;
    int                 udp_sock;
    struct sockaddr_in  dest_addr;

    bool                initialised;
} s = {0};

// ---------------------------------------------------------------------------
// FFT helpers
// ---------------------------------------------------------------------------

// Map FFT magnitude bins to 16 WLED frequency bands (log-spaced)
static void compute_fft_bands(float *magnitudes, uint8_t *out_bands,
                               float *out_major_peak, float *out_magnitude)
{
    // Frequency resolution: 44100 / 1024 ≈ 43.07 Hz per bin
    // We use 16 log-spaced bands covering ~86 Hz to ~20 kHz
    // Band boundaries in FFT bin indices (for 44100 Hz, 1024 point FFT)
    static const uint16_t band_start[WLED_NUM_BINS] = {
        2, 4, 6, 8, 10, 14, 18, 24,
        32, 42, 56, 74, 98, 130, 172, 228
    };
    static const uint16_t band_end[WLED_NUM_BINS] = {
        4, 6, 8, 10, 14, 18, 24, 32,
        42, 56, 74, 98, 130, 172, 228, 300
    };

    float peak_mag = 0.0f;
    float peak_freq = 0.0f;

    for (int b = 0; b < WLED_NUM_BINS; b++) {
        float sum = 0.0f;
        int count = 0;
        for (int i = band_start[b]; i < band_end[b] && i < FFT_SIZE / 2; i++) {
            sum += magnitudes[i];
            count++;
            if (magnitudes[i] > peak_mag) {
                peak_mag = magnitudes[i];
                peak_freq = i * (44100.0f / FFT_SIZE);
            }
        }
        float avg = count > 0 ? sum / count : 0.0f;
        // Scale to 0-255, clamp
        int val = (int)(avg * 0.1f);  // tunable scalar
        out_bands[b] = val > 255 ? 255 : (uint8_t)val;
    }

    *out_major_peak = peak_freq;
    *out_magnitude  = peak_mag;
}

static void run_fft_and_enqueue(int buf_idx)
{
    float *samples = s.sample_buf[buf_idx];

    // Apply Hann window directly into interleaved FFT workspace.
    // fft_buf is heap-allocated so it doesn't blow the task stack.
    for (int i = 0; i < FFT_SIZE; i++) {
        s.fft_buf[i * 2]     = samples[i] * s.wind[i];
        s.fft_buf[i * 2 + 1] = 0.0f;
    }

    // FFT in-place
    dsps_fft2r_fc32(s.fft_buf, FFT_SIZE);
    dsps_bit_rev_fc32(s.fft_buf, FFT_SIZE);

    // Compute magnitudes
    float magnitudes[FFT_SIZE / 2];
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float re = s.fft_buf[i * 2];
        float im = s.fft_buf[i * 2 + 1];
        magnitudes[i] = sqrtf(re * re + im * im);
    }

    // Build packet
    pending_send_t pending;

    wled_udp_packet_t *pkt = &pending.packet;
    memset(pkt, 0, sizeof(*pkt));
    memcpy(pkt->header, "00002", 6);

    float major_peak, magnitude;
    compute_fft_bands(magnitudes, pkt->fftResult, &major_peak, &magnitude);

    pkt->sampleRaw    = magnitudes[1] * 10.0f;
    pkt->sampleSmth   = magnitude * 0.001f;
    pkt->samplePeak   = magnitude > 1000.0f ? 1 : 0;
    pkt->fftMajorPeak = major_peak;
    pkt->fftMagnitude = magnitude;

    if (xQueueSend(s.send_queue, &pending, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Send queue full, dropping FFT packet");
    }
}

static void fft_task(void *arg)
{
    int buf_idx;
    while (true) {
        // Block until the tap hands off a full buffer index
        if (xQueueReceive(s.fft_queue, &buf_idx, portMAX_DELAY) == pdTRUE) {
            run_fft_and_enqueue(buf_idx);
        }
    }
}

// ---------------------------------------------------------------------------
// PCM tap — called from RTP task, must not block
// ---------------------------------------------------------------------------

void wled_sync_pcm_tap(const uint8_t *data, size_t len, void *user_ctx)
{
    if (!s.initialised) return;

    const int16_t *stereo = (const int16_t *)data;
    int frames = (int)(len / 4);  // 4 bytes per stereo frame (2x int16)

    for (int i = 0; i < frames; i++) {
        float sample;
        switch (s.config.channel_mode) {
            case WLED_CHANNEL_LEFT:
                sample = (float)stereo[i * 2];
                break;
            case WLED_CHANNEL_RIGHT:
                sample = (float)stereo[i * 2 + 1];
                break;
            case WLED_CHANNEL_MONO:
            default:
                sample = ((float)stereo[i * 2] + (float)stereo[i * 2 + 1]) * 0.5f;
                break;
        }

        s.sample_buf[s.active_buf][s.sample_count++] = sample;

        if (s.sample_count == FFT_SIZE) {
            int completed = s.active_buf;
            s.active_buf  = 1 - s.active_buf;  // flip to other buffer
            s.sample_count = 0;
            // Hand off completed buffer index to FFT task on Core 1
            xQueueSend(s.fft_queue, &completed, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Sender task — wakes every 5ms, fires packets at their playtime
// ---------------------------------------------------------------------------
static void sender_task(void *arg)
{
    pending_send_t pending;

    while (true) {
        if (xQueueReceive(s.send_queue, &pending, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        // ESP_LOGI("wled_sync", "Sending packet, major_peak=%.1f", pending.packet.fftMajorPeak);
        sendto(s.udp_sock, &pending.packet, sizeof(pending.packet),
               0, (struct sockaddr *)&s.dest_addr, sizeof(s.dest_addr));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t wled_sync_init(const wled_sync_config_t *config)
{
    if (s.initialised) return ESP_ERR_INVALID_STATE;
    if (!config || !config->dest_host) return ESP_ERR_INVALID_ARG;

    memset(&s, 0, sizeof(s));
    s.config = *config;

    // Pre-compute Hann window
    dsps_wind_hann_f32(s.wind, FFT_SIZE);

    // Initialise FFT tables
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Allocate double sample buffers in PSRAM — one written by the tap (Core 0),
    // one read by the FFT task (Core 1). They are never the same buffer.
    s.sample_buf[0] = heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    s.sample_buf[1] = heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    // FFT workspace heap-allocated to avoid blowing the task stack (8KB)
    s.fft_buf       = heap_caps_malloc(FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_SPIRAM);

    if (!s.sample_buf[0] || !s.sample_buf[1] || !s.fft_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        return ESP_ERR_NO_MEM;
    }

    // Create UDP socket
    s.udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        return ESP_FAIL;
    }

    // Set multicast TTL so packets leave the local subnet
    uint8_t ttl = 4;
    setsockopt(s.udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Resolve destination
    memset(&s.dest_addr, 0, sizeof(s.dest_addr));
    s.dest_addr.sin_family = AF_INET;
    s.dest_addr.sin_port   = htons(config->dest_port ? config->dest_port : WLED_PORT_DEFAULT);
    if (inet_aton(config->dest_host, &s.dest_addr.sin_addr) == 0) {
        ESP_LOGE(TAG, "Invalid destination address: %s", config->dest_host);
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        close(s.udp_sock);
        return ESP_ERR_INVALID_ARG;
    }

    // Create send queue
    s.send_queue = xQueueCreate(SENDER_QUEUE_DEPTH, sizeof(pending_send_t));
    if (!s.send_queue) {
        ESP_LOGE(TAG, "Failed to create send queue");
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        close(s.udp_sock);
        return ESP_ERR_NO_MEM;
    }

    // Queue carries buffer index (0 or 1) from tap on Core 0 to FFT task on Core 1
    s.fft_queue = xQueueCreate(2, sizeof(int));
    if (!s.fft_queue) {
        ESP_LOGE(TAG, "Failed to create FFT queue");
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        vQueueDelete(s.send_queue);
        close(s.udp_sock);
        return ESP_ERR_NO_MEM;
    }


    xTaskCreatePinnedToCore(fft_task, "wled_fft", SENDER_TASK_STACK,
                            NULL, SENDER_TASK_PRIO, &s.fft_task, 0);

    xTaskCreatePinnedToCore(sender_task, "wled_sender", SENDER_TASK_STACK,
                            NULL, SENDER_TASK_PRIO, &s.sender_task, 1);

    s.initialised = true;
    ESP_LOGI(TAG, "WLED sync started → %s:%u (%s channel)",
             config->dest_host, config->dest_port,
             config->channel_mode == WLED_CHANNEL_LEFT  ? "left"  :
             config->channel_mode == WLED_CHANNEL_RIGHT ? "right" : "mono");

    return ESP_OK;
}

void wled_sync_deinit(void)
{
    if (!s.initialised) return;

    if (s.fft_task) {
        vTaskDelete(s.fft_task);
        s.fft_task = NULL;
    }
    if (s.sender_task) {
        vTaskDelete(s.sender_task);
        s.sender_task = NULL;
    }
    if (s.fft_queue) {
        vQueueDelete(s.fft_queue);
        s.fft_queue = NULL;
    }
    if (s.send_queue) {
        vQueueDelete(s.send_queue);
        s.send_queue = NULL;
    }
    if (s.udp_sock >= 0) {
        close(s.udp_sock);
        s.udp_sock = -1;
    }
    if (s.sample_buf[0]) {
        heap_caps_free(s.sample_buf[0]);
        s.sample_buf[0] = NULL;
    }
    if (s.sample_buf[1]) {
        heap_caps_free(s.sample_buf[1]);
        s.sample_buf[1] = NULL;
    }
    if (s.fft_buf) {
        heap_caps_free(s.fft_buf);
        s.fft_buf = NULL;
    }

    dsps_fft2r_deinit_fc32();
    s.initialised = false;
    ESP_LOGI(TAG, "WLED sync stopped");
}