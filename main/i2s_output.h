#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

// Initialize I2S with pins from NVS (or defaults)
void i2s_output_init(void);

// Write PCM audio data to I2S
void i2s_output_write(const uint8_t *data, size_t len);

// Stop I2S output
void i2s_output_deinit(void);

#endif // I2S_OUTPUT_H