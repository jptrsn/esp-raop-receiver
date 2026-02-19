#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize I2S output with the given pin assignments.
 *        Pin values should come from config_manager_load().
 *
 * @param bck_pin  Bit clock pin
 * @param ws_pin   Word select (LCK) pin
 * @param dout_pin Data out pin
 */
void i2s_output_init(int bck_pin, int ws_pin, int dout_pin);

// Write PCM audio data to I2S
void i2s_output_write(const uint8_t *data, size_t len);

// Stop and release I2S channel
void i2s_output_deinit(void);

// Set software volume (0.0 to 1.0)
void i2s_output_set_volume(float volume);

#endif // I2S_OUTPUT_H