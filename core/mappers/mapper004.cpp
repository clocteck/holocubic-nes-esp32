#include "mapper004.h"

#include "../cartridge.h"

struct Mapper004_state
{
    Cartridge* cart;
    uint8_t number_PRG_banks;
    uint8_t number_CHR_banks;
    uint8_t* RAM;
    uint8_t* CHR_RAM;

    uint8_t bank_register[8];
    uint8_t* ptr_PRG_bank_8K[4];
    uint8_t* ptr_CHR_bank_1K[8];

    Bank PRG_banks_8K[MAPPER004_NUM_PRG_BANKS_8K];
    Bank CHR_banks_1K[MAPPER004_NUM_CHR_BANKS_1K];
    BankCache PRG_cache_8K;
    BankCache CHR_cache_1K;

    uint8_t bank_select = 0x00;
    uint8_t IRQ_latch = 0x00;
    uint8_t IRQ_counter = 0x00;
    bool IRQ_enable = false;
    bool IRQ_reload = false;
    bool IRQ_a12_low_seen = true;
    bool IRQ_a12_high_seen = false;

    bool PRG_RAM_enable = true;
    bool PRG_RAM_write_protect = false;

    uint8_t PRG_ROM_bank_mode = 0;
    uint8_t CHR_ROM_bank_mode = 0;
    uint16_t PRG_mask = 0;
    uint16_t CHR_mask = 0;

    static constexpr Cartridge::MIRROR mirror[2] =
    {
        Cartridge::MIRROR::VERTICAL,
        Cartridge::MIRROR::HORIZONTAL
    };
};
constexpr Cartridge::MIRROR Mapper004_state::mirror[2];

static void Mapper004_applyHardwareMirror(Mapper004_state* state)
{
    if (!state || !state->cart)
    {
        return;
    }

    state->cart->setMirrorMode((Cartridge::MIRROR)state->cart->hardware_mirror);
}

static void Mapper004_resolveBankRegisters(const Mapper004_state* state, uint8_t resolved[10])
{
    if (!state || !resolved)
    {
        return;
    }

    resolved[0] = (state->bank_register[0] & 0xFE) & state->CHR_mask;
    resolved[1] = (state->bank_register[1] & 0xFE) & state->CHR_mask;
    resolved[2] = state->bank_register[2] & state->CHR_mask;
    resolved[3] = state->bank_register[3] & state->CHR_mask;
    resolved[4] = state->bank_register[4] & state->CHR_mask;
    resolved[5] = state->bank_register[5] & state->CHR_mask;
    resolved[6] = state->bank_register[6] & state->PRG_mask;
    resolved[7] = state->bank_register[7] & state->PRG_mask;
    resolved[8] = (uint8_t)(((resolved[0] + 1U) & state->CHR_mask));
    resolved[9] = (uint8_t)(((resolved[1] + 1U) & state->CHR_mask));
}

static void Mapper004_bindChrRamWindows(Mapper004_state* state, const uint8_t resolved[10])
{
    if (!state || !state->CHR_RAM || !resolved)
    {
        return;
    }

    const uint8_t bank_index[8] =
    {
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[2] : resolved[0]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[3] : resolved[8]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[4] : resolved[1]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[5] : resolved[9]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[0] : resolved[2]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[8] : resolved[3]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[1] : resolved[4]),
        (uint8_t)(state->CHR_ROM_bank_mode ? resolved[9] : resolved[5]),
    };

    for (int i = 0; i < 8; i++)
    {
        state->ptr_CHR_bank_1K[i] = state->CHR_RAM + (((uint32_t)bank_index[i] & 0x07U) << 10);
    }
}

static void Mapper004_updateChrBanks(Mapper004_state* state)
{
    if (!state)
    {
        return;
    }

    uint8_t resolved[10] = {};
    Mapper004_resolveBankRegisters(state, resolved);

    if (state->CHR_RAM)
    {
        Mapper004_bindChrRamWindows(state, resolved);
        return;
    }

    if (state->CHR_ROM_bank_mode)
    {
        state->ptr_CHR_bank_1K[0] = getBank(&state->CHR_cache_1K, resolved[2], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[1] = getBank(&state->CHR_cache_1K, resolved[3], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[2] = getBank(&state->CHR_cache_1K, resolved[4], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[3] = getBank(&state->CHR_cache_1K, resolved[5], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[4] = getBank(&state->CHR_cache_1K, resolved[0], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[5] = getBank(&state->CHR_cache_1K, resolved[8], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[6] = getBank(&state->CHR_cache_1K, resolved[1], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[7] = getBank(&state->CHR_cache_1K, resolved[9], Mapper::ROM_TYPE::CHR_ROM);
    }
    else
    {
        state->ptr_CHR_bank_1K[0] = getBank(&state->CHR_cache_1K, resolved[0], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[1] = getBank(&state->CHR_cache_1K, resolved[8], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[2] = getBank(&state->CHR_cache_1K, resolved[1], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[3] = getBank(&state->CHR_cache_1K, resolved[9], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[4] = getBank(&state->CHR_cache_1K, resolved[2], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[5] = getBank(&state->CHR_cache_1K, resolved[3], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[6] = getBank(&state->CHR_cache_1K, resolved[4], Mapper::ROM_TYPE::CHR_ROM);
        state->ptr_CHR_bank_1K[7] = getBank(&state->CHR_cache_1K, resolved[5], Mapper::ROM_TYPE::CHR_ROM);
    }
}

static void Mapper004_updatePrgBanks(Mapper004_state* state)
{
    if (!state)
    {
        return;
    }

    uint8_t resolved[10] = {};
    Mapper004_resolveBankRegisters(state, resolved);

    const uint8_t fixed_second_last = (uint8_t)((state->number_PRG_banks * 2U) - 2U);
    const uint8_t fixed_last = (uint8_t)((state->number_PRG_banks * 2U) - 1U);

    if (state->PRG_ROM_bank_mode)
    {
        state->ptr_PRG_bank_8K[0] = getBank(&state->PRG_cache_8K, fixed_second_last, Mapper::ROM_TYPE::PRG_ROM);
        state->ptr_PRG_bank_8K[2] = getBank(&state->PRG_cache_8K, resolved[6], Mapper::ROM_TYPE::PRG_ROM);
    }
    else
    {
        state->ptr_PRG_bank_8K[0] = getBank(&state->PRG_cache_8K, resolved[6], Mapper::ROM_TYPE::PRG_ROM);
        state->ptr_PRG_bank_8K[2] = getBank(&state->PRG_cache_8K, fixed_second_last, Mapper::ROM_TYPE::PRG_ROM);
    }

    state->ptr_PRG_bank_8K[1] = getBank(&state->PRG_cache_8K, resolved[7], Mapper::ROM_TYPE::PRG_ROM);
    state->ptr_PRG_bank_8K[3] = getBank(&state->PRG_cache_8K, fixed_last, Mapper::ROM_TYPE::PRG_ROM);
}

static void Mapper004_clockIRQCounter(Mapper004_state* state)
{
    if (!state)
    {
        return;
    }

    /**
     * MMC3 的计数器不是“到 0 再触发下一次”，而是每次 A12 有效上升沿时：
     *
     * 1. 若当前计数器为 0，或者软件刚请求 reload，则先装入 latch；
     * 2. 否则先减 1；
     * 3. 装入/减完之后如果结果为 0 且 IRQ 已使能，就拉 IRQ。
     *
     * 这样 `IRQ_latch == 0` 时就会在每次有效 A12 上升沿都触发，
     * 也就是很多 Sharp MMC3 板卡、分屏状态栏游戏真正依赖的行为。
     */
    if (state->IRQ_counter == 0 || state->IRQ_reload)
    {
        state->IRQ_counter = state->IRQ_latch;
    }
    else
    {
        state->IRQ_counter--;
    }

    if (state->IRQ_counter == 0 && state->IRQ_enable)
    {
        state->cart->IRQ();
    }

    state->IRQ_reload = false;
}

IRAM_ATTR bool Mapper004_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x6000)
    {
        return false;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    if (addr < 0x8000)
    {
        data = state->PRG_RAM_enable ? state->RAM[addr & 0x1FFF] : 0xFF;
        return true;
    }

    const uint8_t bank = (addr >> 13) & 0x03;
    data = state->ptr_PRG_bank_8K[bank][addr & 0x1FFF];
    return true;
}

IRAM_ATTR bool Mapper004_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x6000)
    {
        return false;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    if (addr < 0x8000)
    {
        if (state->PRG_RAM_enable && !state->PRG_RAM_write_protect)
        {
            state->RAM[addr & 0x1FFF] = data;
        }
        return true;
    }

    switch (addr & 0xE001)
    {
        case 0x8000:
            state->bank_select = data & 0x07;
            state->PRG_ROM_bank_mode = (data >> 6) & 0x01;
            state->CHR_ROM_bank_mode = (data >> 7) & 0x01;
            Mapper004_updatePrgBanks(state);
            Mapper004_updateChrBanks(state);
            break;

        case 0x8001:
            state->bank_register[state->bank_select] = data;
            Mapper004_updatePrgBanks(state);
            Mapper004_updateChrBanks(state);
            break;

        case 0xA000:
            if (!state->cart->usesFixedFourScreenMirroring())
            {
                state->cart->setMirrorMode(state->mirror[data & 0x01]);
            }
            break;

        case 0xA001:
            state->PRG_RAM_write_protect = (data & 0x40) != 0;
            state->PRG_RAM_enable = (data & 0x80) != 0;
            break;

        case 0xC000:
            state->IRQ_latch = data;
            break;

        case 0xC001:
            state->IRQ_counter = 0;
            state->IRQ_reload = true;
            break;

        case 0xE000:
            state->IRQ_enable = false;
            break;

        case 0xE001:
            state->IRQ_enable = true;
            break;
    }

    return true;
}

IRAM_ATTR bool Mapper004_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    const uint8_t bank = (addr >> 10) & 0x07;
    data = state->ptr_CHR_bank_1K[bank][addr & 0x03FF];
    return true;
}

IRAM_ATTR bool Mapper004_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    if (!state->CHR_RAM)
    {
        return false;
    }

    const uint8_t bank = (addr >> 10) & 0x07;
    state->ptr_CHR_bank_1K[bank][addr & 0x03FF] = data;
    return true;
}

IRAM_ATTR uint8_t* Mapper004_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF)
    {
        return nullptr;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    const uint8_t bank = (addr >> 10) & 0x07;
    return &state->ptr_CHR_bank_1K[bank][addr & 0x03FF];
}

IRAM_ATTR void Mapper004_ppuAddress(Mapper* mapper, uint16_t addr)
{
    Mapper004_state* state = (Mapper004_state*)mapper->state;
    const bool a12_high = (addr & 0x1000) != 0;

    if (!a12_high)
    {
        state->IRQ_a12_low_seen = true;
        state->IRQ_a12_high_seen = false;
        return;
    }

    if (state->IRQ_a12_high_seen || !state->IRQ_a12_low_seen)
    {
        return;
    }

    state->IRQ_a12_high_seen = true;
    state->IRQ_a12_low_seen = false;
    Mapper004_clockIRQCounter(state);
}

void Mapper004_reset(Mapper* mapper)
{
    Mapper004_state* state = (Mapper004_state*)mapper->state;
    memset(state->RAM, 0, 8 * 1024);
    if (state->CHR_RAM)
    {
        memset(state->CHR_RAM, 0, 8 * 1024);
    }

    memset(state->bank_register, 0, sizeof(state->bank_register));
    state->bank_select = 0x00;
    state->IRQ_latch = 0x00;
    state->IRQ_counter = 0x00;
    state->IRQ_enable = false;
    state->IRQ_reload = false;
    state->IRQ_a12_low_seen = true;
    state->IRQ_a12_high_seen = false;
    state->PRG_RAM_enable = true;
    state->PRG_RAM_write_protect = false;
    state->PRG_ROM_bank_mode = 0;
    state->CHR_ROM_bank_mode = 0;
    state->PRG_mask = (uint16_t)((state->number_PRG_banks * 2U) - 1U);
    state->CHR_mask = state->CHR_RAM ? 0x0007 : (uint16_t)((state->number_CHR_banks * 8U) - 1U);

    invalidateCache(&state->PRG_cache_8K);
    invalidateCache(&state->CHR_cache_1K);
    Mapper004_updatePrgBanks(state);
    Mapper004_updateChrBanks(state);
    Mapper004_applyHardwareMirror(state);
}

void Mapper004_dumpState(Mapper* mapper, File& state)
{
    Mapper004_state* s = (Mapper004_state*)mapper->state;
    Cartridge::MIRROR mirror = s->cart->getMirrorMode();

    state.write(s->bank_register, sizeof(s->bank_register));
    state.write((uint8_t*)&s->bank_select, sizeof(s->bank_select));
    state.write((uint8_t*)&s->IRQ_latch, sizeof(s->IRQ_latch));
    state.write((uint8_t*)&s->IRQ_counter, sizeof(s->IRQ_counter));
    state.write((uint8_t*)&s->IRQ_enable, sizeof(s->IRQ_enable));
    state.write((uint8_t*)&s->IRQ_reload, sizeof(s->IRQ_reload));
    state.write((uint8_t*)&s->IRQ_a12_low_seen, sizeof(s->IRQ_a12_low_seen));
    state.write((uint8_t*)&s->IRQ_a12_high_seen, sizeof(s->IRQ_a12_high_seen));
    state.write((uint8_t*)&s->PRG_RAM_enable, sizeof(s->PRG_RAM_enable));
    state.write((uint8_t*)&s->PRG_RAM_write_protect, sizeof(s->PRG_RAM_write_protect));
    state.write((uint8_t*)&s->PRG_ROM_bank_mode, sizeof(s->PRG_ROM_bank_mode));
    state.write((uint8_t*)&s->CHR_ROM_bank_mode, sizeof(s->CHR_ROM_bank_mode));
    state.write((uint8_t*)&mirror, sizeof(mirror));
    state.write(s->RAM, 8 * 1024);
    if (s->CHR_RAM)
    {
        state.write(s->CHR_RAM, 8 * 1024);
    }
}

void Mapper004_loadState(Mapper* mapper, File& state)
{
    Mapper004_state* s = (Mapper004_state*)mapper->state;
    Cartridge::MIRROR mirror;

    state.read(s->bank_register, sizeof(s->bank_register));
    state.read((uint8_t*)&s->bank_select, sizeof(s->bank_select));
    state.read((uint8_t*)&s->IRQ_latch, sizeof(s->IRQ_latch));
    state.read((uint8_t*)&s->IRQ_counter, sizeof(s->IRQ_counter));
    state.read((uint8_t*)&s->IRQ_enable, sizeof(s->IRQ_enable));
    state.read((uint8_t*)&s->IRQ_reload, sizeof(s->IRQ_reload));
    state.read((uint8_t*)&s->IRQ_a12_low_seen, sizeof(s->IRQ_a12_low_seen));
    state.read((uint8_t*)&s->IRQ_a12_high_seen, sizeof(s->IRQ_a12_high_seen));
    state.read((uint8_t*)&s->PRG_RAM_enable, sizeof(s->PRG_RAM_enable));
    state.read((uint8_t*)&s->PRG_RAM_write_protect, sizeof(s->PRG_RAM_write_protect));
    state.read((uint8_t*)&s->PRG_ROM_bank_mode, sizeof(s->PRG_ROM_bank_mode));
    state.read((uint8_t*)&s->CHR_ROM_bank_mode, sizeof(s->CHR_ROM_bank_mode));
    state.read((uint8_t*)&mirror, sizeof(mirror));
    state.read(s->RAM, 8 * 1024);
    if (s->CHR_RAM)
    {
        state.read(s->CHR_RAM, 8 * 1024);
    }

    s->PRG_mask = (uint16_t)((s->number_PRG_banks * 2U) - 1U);
    s->CHR_mask = s->CHR_RAM ? 0x0007 : (uint16_t)((s->number_CHR_banks * 8U) - 1U);

    invalidateCache(&s->PRG_cache_8K);
    invalidateCache(&s->CHR_cache_1K);
    Mapper004_updatePrgBanks(s);
    Mapper004_updateChrBanks(s);
    s->cart->setMirrorMode(mirror);
}

static void Mapper004_destroy(Mapper* mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper004_state* state = (Mapper004_state*)mapper->state;
    releaseBankCache(&state->PRG_cache_8K);
    releaseBankCache(&state->CHR_cache_1K);

    if (state->RAM)
    {
        nes_port_free(state->RAM);
        state->RAM = nullptr;
    }
    if (state->CHR_RAM)
    {
        nes_port_free(state->CHR_RAM);
        state->CHR_RAM = nullptr;
    }

    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper004_vtable =
{
    Mapper004_cpuRead,
    Mapper004_cpuWrite,
    Mapper004_ppuRead,
    Mapper004_ppuWrite,
    Mapper004_ppuReadPtr,
    Mapper004_ppuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper004_reset,
    Mapper004_dumpState,
    Mapper004_loadState,
    Mapper004_destroy,
};

Mapper createMapper004(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    Mapper mapper;
    mapper.vtable = &Mapper004_vtable;
    Mapper004_state* state = new Mapper004_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }

    state->number_PRG_banks = PRG_banks;
    state->number_CHR_banks = CHR_banks;
    state->cart = cart;

    bankInit(&state->PRG_cache_8K,
             state->PRG_banks_8K,
             MAPPER004_NUM_PRG_BANKS_8K,
             8 * 1024,
             cart,
             MapperAllocPolicy::Psram);
    if (CHR_banks == 0)
    {
        state->CHR_RAM = static_cast<uint8_t*>(mapperAllocHotZeroed(8 * 1024, "mapper004_chr_ram"));
    }
    else
    {
        bankInit(&state->CHR_cache_1K,
                 state->CHR_banks_1K,
                 MAPPER004_NUM_CHR_BANKS_1K,
                 1 * 1024,
                 cart,
                 MapperAllocPolicy::Psram);
    }
    state->RAM = static_cast<uint8_t*>(mapperAllocPsramZeroed(8 * 1024, "mapper004_prg_ram"));

    mapper.state = state;

    const bool chr_ready = (CHR_banks == 0)
                               ? (state->CHR_RAM != nullptr)
                               : bankCacheReady(&state->CHR_cache_1K);
    if (!state->RAM || !bankCacheReady(&state->PRG_cache_8K) || !chr_ready)
    {
        Mapper004_destroy(&mapper);
        mapper.vtable = nullptr;
    }

    return mapper;
}

