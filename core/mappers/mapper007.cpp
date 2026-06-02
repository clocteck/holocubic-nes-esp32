#include "mapper007.h"

#include "../cartridge.h"
#include "../power_on_ram.h"

struct Mapper007_state
{
    Cartridge* cart = nullptr;
    uint8_t number_PRG_banks_32K = 0;
    uint8_t bank_select = 0x00;
    uint8_t* ptr_PRG_bank_32K = nullptr;
    uint8_t* CHR_RAM = nullptr;

    Bank PRG_banks_32K[MAPPER007_NUM_PRG_BANKS_32K];
    BankCache PRG_cache_32K;

    static constexpr Cartridge::MIRROR mirror_lut[2] =
    {
        Cartridge::MIRROR::ONESCREEN_LOW,
        Cartridge::MIRROR::ONESCREEN_HIGH
    };
};
constexpr Cartridge::MIRROR Mapper007_state::mirror_lut[2];

static uint8_t Mapper007_wrap32KBank(const Mapper007_state* state, uint32_t bank)
{
    if (!state || state->number_PRG_banks_32K == 0)
    {
        return 0;
    }

    return (uint8_t)(bank % state->number_PRG_banks_32K);
}

static void Mapper007_applyBank(Mapper007_state* state)
{
    if (!state)
    {
        return;
    }

    const uint8_t bank = Mapper007_wrap32KBank(state, state->bank_select & 0x07U);
    state->ptr_PRG_bank_32K = getBank(&state->PRG_cache_32K, bank, Mapper::ROM_TYPE::PRG_ROM);
    state->cart->setMirrorMode(Mapper007_state::mirror_lut[(state->bank_select >> 4) & 0x01]);
}

IRAM_ATTR bool Mapper007_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x8000)
    {
        return false;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;
    data = state->ptr_PRG_bank_32K[addr & 0x7FFF];
    return true;
}

IRAM_ATTR bool Mapper007_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x8000)
    {
        return false;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;

    /**
     * AxROM 在 `$8000-$FFFF` 整个高半区都映射到同一个写寄存器：
     *
     * 1. bit0-2 选择 32 KiB PRG bank；
     * 2. bit4 选择 one-screen mirroring 的 1 KiB nametable page；
     * 3. 其余位当前忽略。
     */
    state->bank_select = data;
    Mapper007_applyBank(state);
    return true;
}

IRAM_ATTR bool Mapper007_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;
    data = state->CHR_RAM[addr];
    return true;
}

IRAM_ATTR bool Mapper007_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;
    state->CHR_RAM[addr] = data;
    return true;
}

IRAM_ATTR uint8_t* Mapper007_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF)
    {
        return nullptr;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;
    return &state->CHR_RAM[addr];
}

void Mapper007_reset(Mapper* mapper)
{
    Mapper007_state* state = (Mapper007_state*)mapper->state;
    state->bank_select = 0x00;
    invalidateCache(&state->PRG_cache_32K);
    Mapper007_applyBank(state);
}

void Mapper007_dumpState(Mapper* mapper, File& state)
{
    Mapper007_state* s = (Mapper007_state*)mapper->state;
    state.write((uint8_t*)&s->bank_select, sizeof(s->bank_select));
    state.write(s->CHR_RAM, 8 * 1024);
}

void Mapper007_loadState(Mapper* mapper, File& state)
{
    Mapper007_state* s = (Mapper007_state*)mapper->state;
    state.read((uint8_t*)&s->bank_select, sizeof(s->bank_select));
    state.read(s->CHR_RAM, 8 * 1024);

    invalidateCache(&s->PRG_cache_32K);
    Mapper007_applyBank(s);
}

static void Mapper007_destroy(Mapper* mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper007_state* state = (Mapper007_state*)mapper->state;
    releaseBankCache(&state->PRG_cache_32K);
    if (state->CHR_RAM)
    {
        nes_port_free(state->CHR_RAM);
        state->CHR_RAM = nullptr;
    }

    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper007_vtable =
{
    Mapper007_cpuRead,
    Mapper007_cpuWrite,
    Mapper007_ppuRead,
    Mapper007_ppuWrite,
    Mapper007_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper007_reset,
    Mapper007_dumpState,
    Mapper007_loadState,
    Mapper007_destroy,
};

Mapper createMapper007(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    (void)CHR_banks;

    Mapper mapper;
    mapper.vtable = &Mapper007_vtable;
    Mapper007_state* state = new Mapper007_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }

    state->cart = cart;
    state->number_PRG_banks_32K = (uint8_t)((PRG_banks >= 2) ? (PRG_banks / 2U) : 1U);

    bankInit(&state->PRG_cache_32K,
             state->PRG_banks_32K,
             MAPPER007_NUM_PRG_BANKS_32K,
             32 * 1024,
             cart,
             MapperAllocPolicy::Psram);

    state->CHR_RAM = static_cast<uint8_t*>(mapperAllocHot(8 * 1024, "mapper007_chr_ram"));
    if (state->CHR_RAM)
    {
        nes::power_on::fillVolatileRam(state->CHR_RAM, 8 * 1024, 0x7000C8A0u);
    }

    mapper.state = state;
    if (!state->CHR_RAM || !bankCacheReady(&state->PRG_cache_32K))
    {
        Mapper007_destroy(&mapper);
        mapper.vtable = nullptr;
    }

    return mapper;
}

