#pragma once

#include <stdint.h>

/* Build-time options for the NES dynamic module. */

#ifndef FRAMESKIP
#define FRAMESKIP 1
#endif

/*
 * Set to 1 when the target display expects RGB565 bytes in swapped order.
 * Default output is normal RGB565.
 */
#ifndef NES_SCREEN_SWAP_BYTES
#define NES_SCREEN_SWAP_BYTES 0
#endif

/* Left-side overscan crop in pixels. */
#ifndef NES_OVERSCAN_CROP_LEFT
#define NES_OVERSCAN_CROP_LEFT 8
#endif

/*
 * Power-on RAM initialization:
 * 0 = pseudo-random, 1 = 0x00, 2 = 0xFF, 3 = fixed pseudo-random seed.
 */
#ifndef NES_POWER_ON_RAM_MODE
#define NES_POWER_ON_RAM_MODE 0
#endif

#ifndef NES_POWER_ON_RAM_FIXED_SEED
#define NES_POWER_ON_RAM_FIXED_SEED 0x22604276u
#endif

/* Mapper 226 compatibility options. */
#ifndef NES_MAPPER226_IGNORE_CHR_RAM_WRITE_PROTECT
#define NES_MAPPER226_IGNORE_CHR_RAM_WRITE_PROTECT 1
#endif

#ifndef NES_MAPPER226_ENABLE_BUS_CONFLICT
#define NES_MAPPER226_ENABLE_BUS_CONFLICT 0
#endif

/* Mapper 226 diagnostic logging. Keep disabled for normal builds. */
#ifndef NES_MAPPER226_TRACE_WRITES
#define NES_MAPPER226_TRACE_WRITES 0
#endif

#ifndef NES_MAPPER226_TRACE_MAX_WRITES
#define NES_MAPPER226_TRACE_MAX_WRITES 64
#endif

#ifndef NES_MAPPER226_TRACE_UNMAPPED_READS
#define NES_MAPPER226_TRACE_UNMAPPED_READS 0
#endif

#ifndef NES_MAPPER226_TRACE_MAX_UNMAPPED_READS
#define NES_MAPPER226_TRACE_MAX_UNMAPPED_READS 256
#endif

namespace nes::config
{

static constexpr uint16_t kDefaultFrameWidth = 256;
static constexpr uint16_t kDefaultFrameHeight = 240;
static constexpr uint16_t kDefaultTargetFps = 60;
static constexpr uint16_t kDefaultTransferRows = 48;
static constexpr bool kDefaultDirectRender = true;

static constexpr bool kDefaultAudioEnabled = false;
static constexpr uint32_t kDefaultAudioSampleRate = 22050;
static constexpr uint16_t kDefaultAudioRingbufSamples = 2048;

static constexpr uint32_t kDefaultRuntimeTaskStackBytes = 3 * 1024;

} // namespace nes::config
