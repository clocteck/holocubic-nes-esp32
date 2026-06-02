#include "mapper003.h"
#include "../cartridge.h"

IRAM_ATTR bool Mapper003_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x8000) return false;

    Mapper003_state* state = (Mapper003_state*)mapper->state;
    data = state->PRG_bank[addr & 0x7FFF];
    return true;
}

IRAM_ATTR bool Mapper003_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x8000) return false;

    Mapper003_state* state = (Mapper003_state*)mapper->state;
    uint8_t bank = data & 0x03;
    state->ptr_CHR_bank_8K = getBank(&state->CHR_cache_8K, bank, Mapper::ROM_TYPE::CHR_ROM);
    return true;
}

IRAM_ATTR bool Mapper003_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF) return false;

    Mapper003_state* state = (Mapper003_state*)mapper->state;
    data = state->ptr_CHR_bank_8K[addr];
    return true;
}

IRAM_ATTR bool Mapper003_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
	return false;
}

IRAM_ATTR uint8_t* Mapper003_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF) return nullptr;

    Mapper003_state* state = (Mapper003_state*)mapper->state;
    return &state->ptr_CHR_bank_8K[addr];
}

void Mapper003_reset(Mapper* mapper)
{
    Mapper003_state* state = (Mapper003_state*)mapper->state;

    state->ptr_CHR_bank_8K = getBank(&state->CHR_cache_8K, 0, Mapper::ROM_TYPE::CHR_ROM);
    state->cart->loadPRGBank(state->PRG_bank, 32*1024, 0);
}

void Mapper003_dumpState(Mapper* mapper, File& state)
{
    Mapper003_state* s = (Mapper003_state*)mapper->state;

    uint8_t CHR_bank = getBankIndex(&s->CHR_cache_8K, s->ptr_CHR_bank_8K);
    state.write((uint8_t*)&CHR_bank, sizeof(CHR_bank));
}

void Mapper003_loadState(Mapper* mapper, File& state)
{
    Mapper003_state* s = (Mapper003_state*)mapper->state;

    uint8_t CHR_bank;
    state.read((uint8_t*)&CHR_bank, sizeof(CHR_bank));
    invalidateCache(&s->CHR_cache_8K);
    s->ptr_CHR_bank_8K = getBank(&s->CHR_cache_8K, CHR_bank, Mapper::ROM_TYPE::CHR_ROM);
}

static void Mapper003_destroy(Mapper *mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper003_state *state = (Mapper003_state *)mapper->state;
    releaseBankCache(&state->CHR_cache_8K);
    if (state->PRG_bank)
    {
        nes_port_free(state->PRG_bank);
        state->PRG_bank = nullptr;
    }
    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper003_vtable = 
{
    Mapper003_cpuRead,
    Mapper003_cpuWrite,
    Mapper003_ppuRead,
    Mapper003_ppuWrite,
    Mapper003_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper003_reset,
    Mapper003_dumpState,
    Mapper003_loadState,
    Mapper003_destroy,
};

Mapper createMapper003(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    Mapper mapper;
    mapper.vtable = &Mapper003_vtable;
    Mapper003_state* state = new Mapper003_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }
    bankInit(&state->CHR_cache_8K,
             state->CHR_banks_8K,
             MAPPER003_NUM_CHR_BANKS_8K,
             8*1024,
             cart,
             MapperAllocPolicy::Psram);

    state->number_PRG_banks = PRG_banks;
    state->number_CHR_banks = CHR_banks;
    state->cart = cart;
    state->PRG_bank = static_cast<uint8_t*>(mapperAllocPsramZeroed(32 * 1024, "mapper003_prg_bank"));

    mapper.state = state;
    if (!state->PRG_bank ||
        !bankCacheReady(&state->CHR_cache_8K))
    {
        Mapper003_destroy(&mapper);
        mapper.vtable = nullptr;
    }

    return mapper;
}

