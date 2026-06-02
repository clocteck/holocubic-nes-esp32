#include "power_on_ram.h"

#include <string.h>

#include "../nes_config.h"
#include "nes_port.h"

namespace nes::power_on
{
    namespace
    {

        enum class RamInitMode : uint8_t
        {
            Random = 0,
            Zero = 1,
            FF = 2,
            FixedSeed = 3
        };

        static uint32_t normalizeSeed(uint32_t seed)
        {
            /**
             * xorshift32 不能以 0 作为内部状态。
             *
             * 这里统一兜一个固定非零值，避免 fixed-seed 模式在极端配置下退化成全 0。
             */
            return seed == 0 ? 0x6D2B79F5u : seed;
        }

        static uint32_t nextXorShift32(uint32_t *state)
        {
            uint32_t x = normalizeSeed(*state);
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            *state = normalizeSeed(x);
            return *state;
        }

        static void fillWithWordStream(uint8_t *ram,
                                       size_t size,
                                       uint32_t (*next_word)(uint32_t *),
                                       uint32_t initial_state)
        {
            if (!ram || size == 0 || !next_word)
            {
                return;
            }

            uint32_t state = initial_state;
            for (size_t offset = 0; offset < size;)
            {
                const uint32_t word = next_word(&state);
                for (int i = 0; i < 4 && offset < size; i++, offset++)
                {
                    ram[offset] = (uint8_t)(word >> (i * 8));
                }
            }
        }

        static uint32_t nextHwRandomWord(uint32_t *state)
        {
            (void)state;
            return nes_port_random();
        }

    } // namespace

    void fillVolatileRam(uint8_t *ram, size_t size, uint32_t salt)
    {
        if (!ram || size == 0)
        {
            return;
        }

        switch ((RamInitMode)NES_POWER_ON_RAM_MODE)
        {
        case RamInitMode::Zero:
            memset(ram, 0x00, size);
            return;

        case RamInitMode::FF:
            memset(ram, 0xFF, size);
            return;

        case RamInitMode::FixedSeed:
        {
            const uint32_t seed = normalizeSeed(NES_POWER_ON_RAM_FIXED_SEED ^ salt ^ (uint32_t)size);
            fillWithWordStream(ram, size, nextXorShift32, seed);
            return;
        }

        case RamInitMode::Random:
        default:
            fillWithWordStream(ram, size, nextHwRandomWord, salt);
            return;
        }
    }

} // namespace nes::power_on
