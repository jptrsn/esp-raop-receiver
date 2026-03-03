#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

#define CONFIG_DEVICE_NAME_MAX_LEN  31  // 32 byte buffer, 1 byte for null terminator

#define CONFIG_DEFAULT_BCK_PIN   26
#define CONFIG_DEFAULT_WS_PIN    25
#define CONFIG_DEFAULT_DOUT_PIN  22

#define WLED_DEFAULT_HOST "239.0.0.1"
#define WLED_DEFAULT_PORT 11988

typedef struct {
    char device_name[CONFIG_DEVICE_NAME_MAX_LEN + 1];
    int  bck_pin;
    int  ws_pin;
    int  dout_pin;
    // WLED sync
    bool     wled_enabled;
    char     wled_host[64];
    uint16_t wled_port;
    uint8_t  wled_channel;  // 0=mono, 1=left, 2=right
} app_config_t;

/**
 * @brief Load full configuration from NVS, falling back to defaults where absent.
 *        Device name default is generated from MAC address.
 */
esp_err_t config_manager_load(app_config_t *config);

/**
 * @brief Save device name to NVS.
 *        Must be <= CONFIG_DEVICE_NAME_MAX_LEN characters, alphanumeric + hyphens only.
 */
esp_err_t config_manager_save_device(const char *device_name);

/**
 * @brief Save I2S pin assignments to NVS.
 */
esp_err_t config_manager_save_audio(int bck_pin, int ws_pin, int dout_pin);

/**
 * @brief Save WLED FFT configuration to NVS.
 */
esp_err_t config_manager_save_wled(bool enabled, const char *host,
                                   uint16_t port, uint8_t channel);

#endif // CONFIG_MANAGER_H