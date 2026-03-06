/**
 * @file wled_sync.c
 * @brief WLED UDP audio sync — FFT analysis and V2 packet transmission
 *
 * FFT post-processing pipeline adapted from the WLED AudioReactive usermod
 * (usermods/audioreactive/audio_reactive.h), originally by softhack007 and
 * other WLED contributors.  WLED is licensed under the EUPL v1.2, which
 * lists GPL v3 as a compatible outbound licence.  The combined work is
 * distributed under GPL v3.
 *
 * Source: https://github.com/wled/WLED
 *         usermods/audioreactive/audio_reactive.h
 *
 * Adaptations for esp-airsync:
 *   - esp-dsp FFT (dsps_fft2r_fc32) instead of ArduinoFFT
 *   - 1024-point FFT at 44100 Hz (vs 512-point at 22050 Hz) — same
 *     frequency resolution (~43 Hz/bin), magnitudes divided by 256
 *     to compensate for 2× FFT size and full-scale PCM input levels
 *   - Input is decoded AirPlay PCM (int16 stereo) rather than mic I2S
 *   - Simple AGC (fast-attack/slow-decay peak tracking) instead of
 *     WLED's PI-controller AGC — our input has consistent signal
 *     characteristics so a simpler approach suffices
 *   - No noise gate/squelch — the PCM tap is only called during active
 *     playback, so silence is handled by the absence of data
 *   - PCM energy compensation table measured from real AirPlay input
 *     replaces WLED's mic-tuned pink noise table
 */

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

// FFT post-processing constants (from WLED AudioReactive)
// Downscaling factor for Flat-Top window
#define FFT_DOWNSCALE     0.46f
#define LOG_256           5.54517744f   // log(256)

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
// Pink noise / PCM energy compensation table
//
// WLED's original fftResultPink[] corrects for pink noise spectral roll-off
// when using mic input.  Decoded AirPlay PCM has a much more bass-heavy
// energy distribution than mic input, so the correction factors need to be
// larger for high-frequency bands.
//
// This table was computed by measuring median per-band energy from real
// AirPlay PCM (1024pt FFT, 44100 Hz, Flat-Top window), computing the
// correction ratio relative to band 0, then applying sqrt() compression
// to keep the range manageable (1× – 21×) and avoid destabilising the AGC.
// ---------------------------------------------------------------------------

static const float fft_result_pink[WLED_NUM_BINS] = {
     1.00f,  1.10f,  1.63f,  3.08f,  3.26f,  3.78f,  5.72f,  5.20f,
     4.24f,  4.54f,  6.43f,  6.25f,  8.65f,  9.67f, 14.89f, 20.68f
};

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

    // Flat-Top window coefficients — computed once at init, never modified.
    float               wind[FFT_SIZE];

    // Rise-fast / fall-slow smoothed FFT band values (persistent across frames)
    float               fft_avg[WLED_NUM_BINS];

    // Simple AGC — tracks running peak of band averages and computes a
    // gain multiplier to keep output in WLED's expected range.
    float               agc_peak;       // running peak (fast attack, slow decay)
    float               mult_agc;       // current gain multiplier

    // Semaphore for FFT task handoff — carries the buffer index to process.
    QueueHandle_t       fft_queue;
    TaskHandle_t        fft_task;
    TaskHandle_t        sender_task;
    QueueHandle_t       send_queue;
    int                 udp_sock;
    struct sockaddr_in  dest_addr;

    bool                initialised;
    uint32_t            last_tap_time;     // ms timestamp of last pcm_tap call
} s = {0};

// ---------------------------------------------------------------------------
// FFT helpers
// ---------------------------------------------------------------------------

// Average of several FFT result bins [from..to] inclusive (matches WLED fftAddAvg)
static float fft_add_avg(const float *magnitudes, int from, int to)
{
    float result = 0.0f;
    for (int i = from; i <= to; i++) {
        result += magnitudes[i];
    }
    return result / (float)(to - from + 1);
}

/**
 * Map FFT magnitude bins to 16 WLED frequency bands, applying the full
 * WLED AudioReactive post-processing pipeline:
 *
 *   1. Band averaging (log-spaced, from WLED softhack007 22 kHz mapping)
 *   2. Pink noise / PCM energy compensation
 *   3. Flat-Top window downscale
 *   4. AGC gain
 *   5. Rise-fast / fall-slow smoothing
 *   6. Square-root scaling to 0–255
 */
static void compute_fft_bands(const float *magnitudes, uint8_t *out_bands,
                               float *out_major_peak, float *out_magnitude)
{
    float fft_calc[WLED_NUM_BINS];

    // -----------------------------------------------------------------------
    // Stage 1: Band averaging
    //
    // Bin mapping from WLED (softhack007, optimised for 22050 Hz / 512pt).
    // Our 1024pt FFT at 44100 Hz has the same ~43 Hz/bin resolution, so
    // bin indices are identical.
    //                                          bins  frequency range
    // -----------------------------------------------------------------------
    fft_calc[ 0] = fft_add_avg(magnitudes,   1,   2); //  1    43 -   86  sub-bass
    fft_calc[ 1] = fft_add_avg(magnitudes,   2,   3); //  1    86 -  129  bass
    fft_calc[ 2] = fft_add_avg(magnitudes,   3,   5); //  2   129 -  216  bass
    fft_calc[ 3] = fft_add_avg(magnitudes,   5,   7); //  2   216 -  301  bass + midrange
    fft_calc[ 4] = fft_add_avg(magnitudes,   7,  10); //  3   301 -  430  midrange
    fft_calc[ 5] = fft_add_avg(magnitudes,  10,  13); //  3   430 -  560  midrange
    fft_calc[ 6] = fft_add_avg(magnitudes,  13,  19); //  5   560 -  818  midrange
    fft_calc[ 7] = fft_add_avg(magnitudes,  19,  26); //  7   818 - 1120  midrange (1 kHz centre)
    fft_calc[ 8] = fft_add_avg(magnitudes,  26,  33); //  7  1120 - 1421  midrange
    fft_calc[ 9] = fft_add_avg(magnitudes,  33,  44); //  9  1421 - 1895  midrange
    fft_calc[10] = fft_add_avg(magnitudes,  44,  56); // 12  1895 - 2412  midrange + high mid
    fft_calc[11] = fft_add_avg(magnitudes,  56,  70); // 14  2412 - 3015  high mid
    fft_calc[12] = fft_add_avg(magnitudes,  70,  86); // 16  3015 - 3704  high mid
    fft_calc[13] = fft_add_avg(magnitudes,  86, 104); // 18  3704 - 4479  high mid
    fft_calc[14] = fft_add_avg(magnitudes, 104, 165) * 0.88f;  // 4479 - 7106  with damping
    fft_calc[15] = fft_add_avg(magnitudes, 165, 215) * 0.70f;  // 7106 - 9259  with damping

    // -----------------------------------------------------------------------
    // Track peak frequency and magnitude (across all used bins)
    // -----------------------------------------------------------------------
    float peak_mag  = 0.0f;
    float peak_freq = 0.0f;
    for (int i = 1; i < FFT_SIZE / 2; i++) {
        if (magnitudes[i] > peak_mag) {
            peak_mag  = magnitudes[i];
            peak_freq = i * (44100.0f / FFT_SIZE);
        }
    }
    if (peak_freq > 11025.0f) peak_freq = 11025.0f;
    if (peak_freq < 1.0f)     peak_freq = 1.0f;

    // -----------------------------------------------------------------------
    // Stages 2–3: Pink noise / PCM energy compensation, window downscale
    // -----------------------------------------------------------------------
    for (int i = 0; i < WLED_NUM_BINS; i++) {
        fft_calc[i] *= fft_result_pink[i];          // Stage 2: pink noise
        fft_calc[i] *= FFT_DOWNSCALE;               // Stage 3: window correction
        if (fft_calc[i] < 0.0f) fft_calc[i] = 0.0f;
    }

    // -----------------------------------------------------------------------
    // Stage 4: Simple AGC
    //
    // Track the running peak of the loudest band (after pink/downscale).
    // Fast attack (immediate), slow decay (~3 seconds at ~43 fps).
    // Compute a multiplier to bring the peak band into WLED's working
    // range (~200, which after sqrt scaling lands in the middle of 0–255).
    //
    // AGC_DECAY:  0.9995^43 ≈ 0.978 per second → ~3s to halve
    // AGC_TARGET: the post-gain band value we're aiming for
    // -----------------------------------------------------------------------
    #define AGC_TARGET  200.0f
    #define AGC_DECAY   0.9995f
    #define AGC_MULT_MIN  0.001f
    #define AGC_MULT_MAX  50.0f

    // Find current frame's peak band value (after pink/downscale)
    float frame_peak = 0.0f;
    for (int i = 0; i < WLED_NUM_BINS; i++) {
        if (fft_calc[i] > frame_peak) frame_peak = fft_calc[i];
    }

    // Update running peak: fast attack, slow decay
    if (frame_peak > s.agc_peak) {
        s.agc_peak = frame_peak;
    } else {
        s.agc_peak *= AGC_DECAY;
    }
    if (s.agc_peak < 1.0f) s.agc_peak = 1.0f;  // avoid division by zero

    // Compute and clamp gain multiplier
    s.mult_agc = AGC_TARGET / s.agc_peak;
    if (s.mult_agc > AGC_MULT_MAX) s.mult_agc = AGC_MULT_MAX;
    if (s.mult_agc < AGC_MULT_MIN) s.mult_agc = AGC_MULT_MIN;

    // Apply AGC gain
    for (int i = 0; i < WLED_NUM_BINS; i++) {
        fft_calc[i] *= s.mult_agc;
    }

    // DEBUG: periodic logging (remove after tuning)
    // static int band_log_counter = 0;
    // if (++band_log_counter >= 20) {
    //     band_log_counter = 0;
    //     ESP_LOGI(TAG, "agc: peak=%.0f mult=%.3f  post: %.0f %.0f %.0f %.0f "
    //                    "%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f",
    //              s.agc_peak, s.mult_agc,
    //              fft_calc[0],  fft_calc[1],  fft_calc[2],  fft_calc[3],
    //              fft_calc[4],  fft_calc[5],  fft_calc[6],  fft_calc[7],
    //              fft_calc[8],  fft_calc[9],  fft_calc[10], fft_calc[11],
    //              fft_calc[12], fft_calc[13], fft_calc[14], fft_calc[15]);
    // }

    // -----------------------------------------------------------------------
    // Stage 5: Rise-fast / fall-slow smoothing
    //
    // WLED default decay time is 1400 ms → uses 0.17/0.83 blend.
    // -----------------------------------------------------------------------
    for (int i = 0; i < WLED_NUM_BINS; i++) {
        if (fft_calc[i] > s.fft_avg[i]) {
            // Rise fast: converge in ~2 cycles (~50 ms at ~23 ms/frame)
            s.fft_avg[i] = fft_calc[i] * 0.75f + 0.25f * s.fft_avg[i];
        } else {
            // Fall slow: converge in ~9 cycles (~225 ms)
            s.fft_avg[i] = fft_calc[i] * 0.17f + 0.83f * s.fft_avg[i];
        }

        // Constrain internal value (WLED caps at 1023)
        if (fft_calc[i] > 1023.0f) fft_calc[i] = 1023.0f;
        if (s.fft_avg[i] > 1023.0f) s.fft_avg[i] = 1023.0f;
    }

    // -----------------------------------------------------------------------
    // Stage 6: Square-root scaling to 0–255 (WLED FFTScalingMode == 3)
    //
    //   val *= 0.38
    //   val -= 6.0                         (noise floor headroom)
    //   val  = sqrt(val)                   (compress dynamic range)
    //   val *= 0.85 + (band_index / 4.5)  (high-frequency boost)
    //   val  = map(val, 0..16, 0..255)
    // -----------------------------------------------------------------------
    for (int i = 0; i < WLED_NUM_BINS; i++) {
        float val = s.fft_avg[i];

        val *= 0.38f;
        val -= 6.0f;
        if (val > 1.0f)
            val = sqrtf(val);
        else
            val = 0.0f;

        val *= 0.85f + ((float)i / 4.5f);   // high-frequency boost

        // Map [0..sqrt(256)] → [0..255], i.e. [0..16] → [0..255]
        val = val * (255.0f / 16.0f);

        int ival = (int)val;
        out_bands[i] = (uint8_t)(ival > 255 ? 255 : (ival < 0 ? 0 : ival));
    }

    *out_major_peak = peak_freq;
    *out_magnitude  = peak_mag;
}

static void run_fft_and_enqueue(int buf_idx)
{
    float *samples = s.sample_buf[buf_idx];

    // Apply Flat-Top window directly into interleaved FFT workspace.
    for (int i = 0; i < FFT_SIZE; i++) {
        s.fft_buf[i * 2]     = samples[i] * s.wind[i];
        s.fft_buf[i * 2 + 1] = 0.0f;
    }

    // FFT in-place
    dsps_fft2r_fc32(s.fft_buf, FFT_SIZE);
    dsps_bit_rev_fc32(s.fft_buf, FFT_SIZE);

    // DC removal (WLED: vReal[0] = 0)
    s.fft_buf[0] = 0.0f;
    s.fft_buf[1] = 0.0f;

    // Compute magnitudes and downscale.
    // WLED divides by 16 for 512pt FFT on mic input (~50k–100k raw peaks).
    // Our 1024pt FFT on full-scale AirPlay PCM produces ~1M raw peaks.
    // Dividing by 256 brings these into the ~4096 target range that WLED's
    // downstream pipeline (pink noise, gain, sqrt scaling) is tuned for.
    float magnitudes[FFT_SIZE / 2];
    float raw_max = 0.0f;
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float re = s.fft_buf[i * 2];
        float im = s.fft_buf[i * 2 + 1];
        float mag = sqrtf(re * re + im * im);
        if (mag > raw_max) raw_max = mag;
        magnitudes[i] = mag / 256.0f;
    }

    // DEBUG: log raw max magnitude (remove after tuning)
    // static int raw_log_counter = 0;
    // if (++raw_log_counter >= 20) {
    //     raw_log_counter = 0;
    //     ESP_LOGI(TAG, "raw_max=%.0f  after /256=%.0f",
    //              raw_max, raw_max / 256.0f);
    // }

    // Build packet
    pending_send_t pending;
    wled_udp_packet_t *pkt = &pending.packet;
    memset(pkt, 0, sizeof(*pkt));
    memcpy(pkt->header, "00002", 6);

    float major_peak, magnitude;
    compute_fft_bands(magnitudes, pkt->fftResult, &major_peak, &magnitude);

    // sampleRaw / sampleSmth: WLED uses these for volume-reactive effects.
    // We derive them from the peak magnitude, scaled by AGC.
    float vol = magnitude * s.mult_agc;
    if (vol > 255.0f) vol = 255.0f;

    pkt->sampleRaw    = vol;
    pkt->sampleSmth   = vol;
    pkt->samplePeak   = (vol > 200.0f) ? 1 : 0;
    pkt->fftMajorPeak = major_peak;
    pkt->fftMagnitude = magnitude * s.mult_agc;

    // Periodic logging (~every 500 ms at ~43 fps)
    // static int log_counter = 0;
    // if (++log_counter >= 20) {
    //     log_counter = 0;
    //     ESP_LOGI(TAG, "bands: %3u %3u %3u %3u %3u %3u %3u %3u "
    //                    "%3u %3u %3u %3u %3u %3u %3u %3u  "
    //                    "peak=%.0fHz mag=%.0f vol=%.1f",
    //              pkt->fftResult[ 0], pkt->fftResult[ 1],
    //              pkt->fftResult[ 2], pkt->fftResult[ 3],
    //              pkt->fftResult[ 4], pkt->fftResult[ 5],
    //              pkt->fftResult[ 6], pkt->fftResult[ 7],
    //              pkt->fftResult[ 8], pkt->fftResult[ 9],
    //              pkt->fftResult[10], pkt->fftResult[11],
    //              pkt->fftResult[12], pkt->fftResult[13],
    //              pkt->fftResult[14], pkt->fftResult[15],
    //              major_peak, magnitude, vol);
    // }

    if (xQueueSend(s.send_queue, &pending, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Send queue full, dropping FFT packet");
    }
}

static void fft_task(void *arg)
{
    int buf_idx;
    while (true) {
        if (xQueueReceive(s.fft_queue, &buf_idx, portMAX_DELAY) == pdTRUE) {
            run_fft_and_enqueue(buf_idx);
        }
    }
}

// ---------------------------------------------------------------------------
// PCM tap — called from audio output task ~50ms before playtime
// ---------------------------------------------------------------------------

void wled_sync_pcm_tap(const uint8_t *data, size_t len, void *user_ctx)
{
    if (!s.initialised) return;

    s.last_tap_time = (uint32_t)(esp_timer_get_time() / 1000ULL);

    const int16_t *stereo = (const int16_t *)data;
    int frames = (int)(len / 4);  // 4 bytes per stereo frame (2× int16)

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
            s.active_buf  = 1 - s.active_buf;
            s.sample_count = 0;
            xQueueSend(s.fft_queue, &completed, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Sender task
// ---------------------------------------------------------------------------

static void sender_task(void *arg)
{
    pending_send_t pending;

    while (true) {
        if (xQueueReceive(s.send_queue, &pending, pdMS_TO_TICKS(500)) != pdTRUE) {
            // No FFT data for 500ms — if we were recently active, send silence
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (s.last_tap_time > 0 && (now - s.last_tap_time) > 500) {
                memset(&pending, 0, sizeof(pending));
                memcpy(pending.packet.header, "00002", 6);
                pending.packet.fftMajorPeak = 1.0f;
                sendto(s.udp_sock, &pending.packet, sizeof(pending.packet),
                    0, (struct sockaddr *)&s.dest_addr, sizeof(s.dest_addr));
                s.last_tap_time = 0;  // only send silence once
            }
            continue;
        }

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
    s.mult_agc = 1.0f;     // AGC starts at unity gain
    s.agc_peak = 1.0f;     // avoid division by zero on first frame

    // Pre-compute Flat-Top window (matching WLED's ArduinoFFT windowing)
    dsps_wind_flat_top_f32(s.wind, FFT_SIZE);

    // Initialise FFT tables
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Allocate double sample buffers in PSRAM
    s.sample_buf[0] = heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    s.sample_buf[1] = heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
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

    uint8_t ttl = 4;
    setsockopt(s.udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

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

    s.send_queue = xQueueCreate(SENDER_QUEUE_DEPTH, sizeof(pending_send_t));
    if (!s.send_queue) {
        ESP_LOGE(TAG, "Failed to create send queue");
        heap_caps_free(s.sample_buf[0]);
        heap_caps_free(s.sample_buf[1]);
        heap_caps_free(s.fft_buf);
        close(s.udp_sock);
        return ESP_ERR_NO_MEM;
    }

    // Queue carries buffer index (0 or 1) from tap to FFT task
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