#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Initialize the audio buffer (allocates memory, does NOT start playback)
void audio_buffer_init(void);

// Start audio playback task
void audio_buffer_start(void);

// Set the time when playback should actually begin
void audio_buffer_set_start_time(uint32_t playtime);

// Stop audio playback task (buffer remains allocated)
void audio_buffer_stop(void);

// Write audio data with timing information (called from RTP data callback)
// Returns true if written, false if buffer full
bool audio_buffer_write(const uint8_t *data, size_t len, uint32_t playtime);

// Flush all buffered audio
void audio_buffer_flush(void);

// Cleanup (stops playback and frees memory)
void audio_buffer_deinit(void);

// Get current buffer level for sync calculations
void audio_buffer_get_timing(uint32_t *frames_buffered, uint32_t *head_playtime);

// Timing correction
void audio_buffer_skip_frames(uint32_t count);
void audio_buffer_pause_frames(uint32_t count);

// Initialization check
bool audio_buffer_is_ready(void);