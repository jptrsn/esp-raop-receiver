#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Channel mode for FFT analysis
 */
typedef enum {
    WLED_CHANNEL_MONO,   ///< Mix L+R down to mono (default)
    WLED_CHANNEL_LEFT,   ///< Analyse left channel only
    WLED_CHANNEL_RIGHT,  ///< Analyse right channel only
} wled_channel_mode_t;

/**
 * @brief WLED sync configuration
 */
typedef struct {
    const char          *dest_host;     ///< Destination IP or multicast group
    uint16_t             dest_port;     ///< Destination UDP port
    wled_channel_mode_t  channel_mode;  ///< Mono mixdown, left, or right
} wled_sync_config_t;

/**
 * @brief Initialise WLED sync — allocates buffers and starts sender task.
 *        Call once after WiFi is up.
 */
esp_err_t wled_sync_init(const wled_sync_config_t *config);

/**
 * @brief Feed a decoded PCM frame into the FFT pipeline.
 *        Matches audio_pcm_tap_cb_t — called at output time, playtime ≈ now.
 *        Must not block.
 */
void wled_sync_pcm_tap(const uint8_t *data, size_t len, void *user_ctx);

/**
 * @brief Shut down WLED sync and free resources.
 */
void wled_sync_deinit(void);

#ifdef __cplusplus
}
#endif