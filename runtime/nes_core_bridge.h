#pragma once

#include <stddef.h>
#include <stdint.h>

#include "module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes_core_options_t {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t transfer_rows;
    uint32_t target_fps;
    uint32_t task_stack_bytes;
    uint32_t task_priority;
    int32_t task_core;
    uint8_t autorun;
    uint8_t audio_enabled;
    uint8_t audio_lua_fallback;
    uint16_t audio_channels;
    uint16_t audio_bits_per_sample;
    uint8_t audio_volume_percent;
    uint8_t reserved_audio[3];
    uint32_t audio_sample_rate;
    uint32_t audio_queue_bytes;
} nes_core_options_t;

typedef struct nes_core_status_t {
    int32_t state;
    uint8_t running;
    uint8_t paused;
    uint8_t loaded;
    uint8_t mapper;
    uint32_t frames;
    uint32_t started_ms;
    uint32_t stopped_ms;
    uint32_t last_gamepad_mask;
    uint32_t last_nes_mask;
    uint32_t task_stack_ptr;
    uint32_t step_pending;
    uint32_t stage;
    uint16_t transfer_rows;
    uint8_t display_stream_supported;
    uint8_t display_stream_active;
    uint8_t display_stream_slots;
    uint8_t display_stream_queued;
    uint8_t display_async_supported;
    uint8_t display_async_active;
    uint8_t display_async_slots;
    uint8_t task_stack_psram;
    uint8_t autorun;
    uint8_t audio_requested;
    uint8_t audio_active;
    uint32_t audio_queued_bytes;
    uint32_t audio_dropped_bytes;
    char audio_backend[12];
    char audio_error[64];
    char last_error[96];
} nes_core_status_t;

void *nes_core_create(const module_host_api_v1 *host);
void nes_core_destroy(void *runtime);
int nes_core_start(void *runtime, const char *rom_path, const nes_core_options_t *options, char *err, size_t err_size);
int nes_core_stop(void *runtime, uint32_t timeout_ms, int force, char *err, size_t err_size);
int nes_core_pause(void *runtime, int paused);
int nes_core_prepare(void *runtime, uint32_t timeout_ms, uint32_t level, char *err, size_t err_size);
int nes_core_step(void *runtime, uint32_t frames, char *err, size_t err_size);
void nes_core_set_input_mask(void *runtime, uint32_t mask);
int nes_core_input(void *runtime, uint32_t *out_gamepad_mask, uint32_t *out_nes_mask);
void nes_core_status(void *runtime, nes_core_status_t *out);
size_t nes_core_read_audio(void *runtime, uint8_t *dst, size_t max_bytes);

#ifdef __cplusplus
}
#endif
