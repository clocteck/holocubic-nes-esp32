#include "mapper015.h"

#include "../cartridge.h"
#include "../power_on_ram.h"

struct Mapper015_state
{
    Cartridge* cart = nullptr;
    uint8_t number_PRG_banks_16K = 0;
    uint8_t number_PRG_banks_8K = 0;

    uint8_t register_data = 0x00;
    uint8_t register_mode = 0x00;

    uint8_t* PRG_RAM = nullptr;
    uint8_t* CHR_RAM = nullptr;
    uint8_t* ptr_PRG_bank_8K[4] = {};

    Bank PRG_banks_8K[MAPPER015_NUM_PRG_BANKS_8K];
    BankCache PRG_cache_8K;

    static constexpr Cartridge::MIRROR mirror_lut[2] =
    {
        Cartridge::MIRROR::VERTICAL,
        Cartridge::MIRROR::HORIZONTAL
    };
};
constexpr Cartridge::MIRROR Mapper015_state::mirror_lut[2];

/**
 * @brief 把 8 KiB PRG bank 号安全收敛到当前 ROM 实际范围内。
 */
static uint8_t Mapper015_wrap8KBank(const Mapper015_state* state, uint32_t bank)
{
    if (!state || state->number_PRG_banks_8K == 0)
    {
        return 0;
    }

    return static_cast<uint8_t>(bank % state->number_PRG_banks_8K);
}

/**
 * @brief 把 16 KiB PRG bank 号安全收敛到当前 ROM 实际范围内。
 */
static uint8_t Mapper015_wrap16KBank(const Mapper015_state* state, uint32_t bank)
{
    if (!state || state->number_PRG_banks_16K == 0)
    {
        return 0;
    }

    return static_cast<uint8_t>(bank % state->number_PRG_banks_16K);
}

/**
 * @brief 把一个 16 KiB bank 展开成两个连续的 8 KiB CPU window。
 */
static void Mapper015_map16KWindow(Mapper015_state* state, uint8_t window_index, uint8_t bank_16K)
{
    if (!state || window_index >= 2)
    {
        return;
    }

    const uint8_t wrapped_bank_16K = Mapper015_wrap16KBank(state, bank_16K);
    const uint8_t wrapped_bank_8K_low = Mapper015_wrap8KBank(state, (uint32_t)wrapped_bank_16K * 2U);
    const uint8_t wrapped_bank_8K_high = Mapper015_wrap8KBank(state, (uint32_t)wrapped_bank_16K * 2U + 1U);
    const uint8_t slot = static_cast<uint8_t>(window_index * 2U);

    state->ptr_PRG_bank_8K[slot] =
        getBank(&state->PRG_cache_8K, wrapped_bank_8K_low, Mapper::ROM_TYPE::PRG_ROM);
    state->ptr_PRG_bank_8K[slot + 1] =
        getBank(&state->PRG_cache_8K, wrapped_bank_8K_high, Mapper::ROM_TYPE::PRG_ROM);
}

/**
 * @brief 根据当前 latch 的 data + mode 重新计算 mapper15 的 PRG/镜像映射。
 */
static void Mapper015_applyMapping(Mapper015_state* state)
{
    if (!state || !state->cart)
    {
        return;
    }

    const uint8_t bank_16K = state->register_data & 0x3FU;
    const uint8_t mode = state->register_mode & 0x03U;

    state->cart->setMirrorMode(
        Mapper015_state::mirror_lut[(state->register_data >> 6) & 0x01U]
    );

    /**
     * mapper 15 的 4 个模式本质上是在改 PRG A14 / A13 的来源：
     *
     * 0. 32 KiB 连续映射，A14 由 CPU A14 驱动；
     * 1. UNROM 风格，下半 16 KiB 可切，上半 16 KiB 强制落到当前 128 KiB 区块末页；
     * 2. 8 KiB 全窗口镜像，A14 固定取寄存器，A13 取 data bit7；
     * 3. 16 KiB 全窗口镜像，整个 `$8000-$FFFF` 复用同一 16 KiB。
     *
     * 这里统一展开成 4 个 8 KiB window，避免再额外引入一套特判路径。
     */
    switch (mode)
    {
    case 0:
        Mapper015_map16KWindow(state, 0, (uint8_t)(bank_16K & 0x3EU));
        Mapper015_map16KWindow(state, 1, (uint8_t)((bank_16K & 0x3EU) | 0x01U));
        break;

    case 1:
        Mapper015_map16KWindow(state, 0, bank_16K);
        Mapper015_map16KWindow(state, 1, (uint8_t)((bank_16K & 0x38U) | 0x07U));
        break;

    case 2:
    {
        const uint8_t bank_8K = Mapper015_wrap8KBank(
            state,
            ((uint32_t)(state->register_data & 0x3FU) << 1U) |
            ((state->register_data >> 7) & 0x01U)
        );
        uint8_t* const bank_ptr = getBank(&state->PRG_cache_8K, bank_8K, Mapper::ROM_TYPE::PRG_ROM);
        for (uint8_t i = 0; i < 4; i++)
        {
            state->ptr_PRG_bank_8K[i] = bank_ptr;
        }
        break;
    }

    case 3:
    default:
        Mapper015_map16KWindow(state, 0, bank_16K);
        Mapper015_map16KWindow(state, 1, bank_16K);
        break;
    }
}

/**
 * @brief 处理 CPU 对 `$6000-$FFFF` 的读访问。
 */
IRAM_ATTR bool Mapper015_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x6000)
    {
        return false;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;
    if (addr < 0x8000)
    {
        data = state->PRG_RAM[addr & 0x1FFFU];
        return true;
    }

    data = state->ptr_PRG_bank_8K[(addr >> 13) & 0x03U][addr & 0x1FFFU];
    return true;
}

/**
 * @brief 处理 CPU 对 `$6000-$FFFF` 的写访问，并在高半区更新 mapper latch。
 */
IRAM_ATTR bool Mapper015_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x6000)
    {
        return false;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;
    if (addr < 0x8000)
    {
        state->PRG_RAM[addr & 0x1FFFU] = data;
        return true;
    }

    /**
     * mapper 15 没有多组寄存器，整个 `$8000-$FFFF` 都是同一个 latch，
     * 只有 CPU A0-A1 会决定当前落入哪一种 PRG 映射模式。
     */
    state->register_data = data;
    state->register_mode = (uint8_t)(addr & 0x0003U);
    Mapper015_applyMapping(state);
    return true;
}

/**
 * @brief 读取 mapper15 的固定 8 KiB CHR-RAM。
 */
IRAM_ATTR bool Mapper015_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;
    data = state->CHR_RAM[addr];
    return true;
}

/**
 * @brief 写入 mapper15 的固定 8 KiB CHR-RAM。
 */
IRAM_ATTR bool Mapper015_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;

    /**
     * NESdev 明确提到很多 mapper15 hack ROM 依赖：
     * 1. 8 KiB PRG-RAM 出现在 `$6000-$7FFF`；
     * 2. 不要严格执行 CHR 的写保护。
     *
     * 这里按兼容性优先，统一把 PPU `$0000-$1FFF` 视为 8 KiB CHR-RAM。
     */
    state->CHR_RAM[addr] = data;
    return true;
}

/**
 * @brief 为 PPU 热路径返回直接可解引用的 CHR-RAM 指针。
 */
IRAM_ATTR uint8_t* Mapper015_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF)
    {
        return nullptr;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;
    return &state->CHR_RAM[addr];
}

/**
 * @brief 把 mapper15 复位到缺省 latch 状态，并重建初始映射。
 */
void Mapper015_reset(Mapper* mapper)
{
    Mapper015_state* state = (Mapper015_state*)mapper->state;
    if (!state)
    {
        return;
    }

    memset(state->PRG_RAM, 0, 8 * 1024);
    state->register_data = 0x00;
    state->register_mode = 0x00;

    invalidateCache(&state->PRG_cache_8K);
    Mapper015_applyMapping(state);
}

/**
 * @brief 序列化 mapper15 的寄存器与 RAM 状态。
 */
void Mapper015_dumpState(Mapper* mapper, File& stateFile)
{
    Mapper015_state* s = (Mapper015_state*)mapper->state;
    stateFile.write((uint8_t*)&s->register_data, sizeof(s->register_data));
    stateFile.write((uint8_t*)&s->register_mode, sizeof(s->register_mode));
    stateFile.write(s->PRG_RAM, 8 * 1024);
    stateFile.write(s->CHR_RAM, 8 * 1024);
}

/**
 * @brief 反序列化 mapper15 的寄存器与 RAM 状态，并恢复 bank 映射。
 */
void Mapper015_loadState(Mapper* mapper, File& stateFile)
{
    Mapper015_state* s = (Mapper015_state*)mapper->state;
    stateFile.read((uint8_t*)&s->register_data, sizeof(s->register_data));
    stateFile.read((uint8_t*)&s->register_mode, sizeof(s->register_mode));
    stateFile.read(s->PRG_RAM, 8 * 1024);
    stateFile.read(s->CHR_RAM, 8 * 1024);

    invalidateCache(&s->PRG_cache_8K);
    Mapper015_applyMapping(s);
}

/**
 * @brief 释放 mapper15 自己申请的 bank cache 与 RAM 缓冲。
 */
static void Mapper015_destroy(Mapper* mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper015_state* state = (Mapper015_state*)mapper->state;
    releaseBankCache(&state->PRG_cache_8K);

    if (state->PRG_RAM)
    {
        nes_port_free(state->PRG_RAM);
        state->PRG_RAM = nullptr;
    }

    if (state->CHR_RAM)
    {
        nes_port_free(state->CHR_RAM);
        state->CHR_RAM = nullptr;
    }

    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper015_vtable =
{
    Mapper015_cpuRead,
    Mapper015_cpuWrite,
    Mapper015_ppuRead,
    Mapper015_ppuWrite,
    Mapper015_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper015_reset,
    Mapper015_dumpState,
    Mapper015_loadState,
    Mapper015_destroy,
};

/**
 * @brief Create mapper 15 with PRG-RAM and CHR-RAM compatibility enabled.
 */
Mapper createMapper015(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    /* iNES mapper 15 dumps commonly use 8 KiB CHR-RAM. */
    (void)CHR_banks;

    Mapper mapper{};
    mapper.vtable = &Mapper015_vtable;

    Mapper015_state* state = new Mapper015_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }

    state->cart = cart;
    state->number_PRG_banks_16K = PRG_banks;
    state->number_PRG_banks_8K = static_cast<uint8_t>(PRG_banks * 2U);

    bankInit(&state->PRG_cache_8K,
             state->PRG_banks_8K,
             MAPPER015_NUM_PRG_BANKS_8K,
             8 * 1024,
             cart,
             MapperAllocPolicy::Psram);

    state->PRG_RAM = static_cast<uint8_t*>(mapperAllocPsramZeroed(8 * 1024, "mapper015_prg_ram"));
    state->CHR_RAM = static_cast<uint8_t*>(mapperAllocHot(8 * 1024, "mapper015_chr_ram"));
    if (state->CHR_RAM)
    {
        nes::power_on::fillVolatileRam(state->CHR_RAM, 8 * 1024, 0x1500C8A0u);
    }

    mapper.state = state;
    if (PRG_banks == 0 ||
        !state->PRG_RAM ||
        !state->CHR_RAM ||
        !bankCacheReady(&state->PRG_cache_8K))
    {
        Mapper015_destroy(&mapper);
        mapper.vtable = nullptr;
        return mapper;
    }

    invalidateCache(&state->PRG_cache_8K);
    Mapper015_applyMapping(state);
    return mapper;
}

