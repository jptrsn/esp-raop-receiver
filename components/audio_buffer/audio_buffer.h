#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Initialize the audio buffer and start output thread
void audio_buffer_init(void);

// Write audio data with timing information (called from RTP data callback)
// Returns true if written, false if buffer full
bool audio_buffer_write(const uint8_t *data, size_t len, uint32_t playtime);

// Flush all buffered audio
void audio_buffer_flush(void);

// Cleanup
void audio_buffer_deinit(void);

// Get current buffer level for sync calculations
void audio_buffer_get_timing(uint32_t *frames_buffered, uint32_t *head_playtime);

// Timing correction
void audio_buffer_skip_frames(uint32_t count);
void audio_buffer_pause_frames(uint32_t count);

// Initialization check
bool audio_buffer_is_ready(void);