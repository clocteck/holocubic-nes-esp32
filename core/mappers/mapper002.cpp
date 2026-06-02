#include "mapper002.h"
#include "../cartridge.h"

IRAM_ATTR bool Mapper002_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x8000) return false;

    Mapper002_state* state = (Mapper002_state*)mapper->state;
    uint16_t offset = addr & 0x3FFF;
    uint8_t bank_id = (addr >> 14) & 1;
    data = state->ptr_16K_PRG_banks[bank_id][offset];
    return true;
}

IRAM_ATTR bool Mapper002_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x8000) return false;

    Mapper002_state* state = (Mapper002_state*)mapper->state;
    uint8_t bank = data & 0x0F;
    state->ptr_16K_PRG_banks[0] = getBank(&state->prg_cache, bank, Mapper::ROM_TYPE::PRG_ROM);
    return true;
}

IRAM_ATTR bool Mapper002_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF) return false;

    Mapper002_state* state = (Mapper002_state*)mapper->state;
    data = state->CHR_bank[addr];
    return true;
}

IRAM_ATTR bool Mapper002_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF) return false;
    
    Mapper002_state* state = (Mapper002_state*)mapper->state;
    if (state->number_CHR_banks == 0)
    {
        // Treat as RAM
        state->CHR_bank[addr] = data;
        return true;
    }

	return false;
}

IRAM_ATTR uint8_t* Mapper002_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF) return nullptr;

    Mapper002_state* state = (Mapper002_state*)mapper->state;
    return &state->CHR_bank[addr];
}

void Mapper002_reset(Mapper* mapper)
{
    Mapper002_state* state = (Mapper002_state*)mapper->state;
    state->ptr_16K_PRG_banks[0] = getBank(&state->prg_cache, 0, Mapper::ROM_TYPE::PRG_ROM);
    state->ptr_16K_PRG_banks[1] = state->PRG_bank;

    state->cart->loadPRGBank(state->ptr_16K_PRG_banks[1], 16*1024, 0x4000 * (state->number_PRG_banks - 1));
    state->cart->loadCHRBank(state->CHR_bank, 8*1024, 0);
}

void Mapper002_dumpState(Mapper* mapper, File& state)
{
    Mapper002_state* s = (Mapper002_state*)mapper->state;

    uint8_t PRG_16K;
    PRG_16K = getBankIndex(&s->prg_cache, s->ptr_16K_PRG_banks[0]);
    state.write((uint8_t*)&PRG_16K, sizeof(PRG_16K));
    if (s->number_CHR_banks == 0)
    {
        state.write(s->CHR_bank, 8 * 1024);
    }
}

void Mapper002_loadState(Mapper* mapper, File& state)
{
    Mapper002_state* s = (Mapper002_state*)mapper->state;

    uint8_t PRG_16K;
    state.read((uint8_t*)&PRG_16K, sizeof(PRG_16K));
    invalidateCache(&s->prg_cache);
    s->ptr_16K_PRG_banks[0] = getBank(&s->prg_cache, PRG_16K, Mapper::ROM_TYPE::PRG_ROM);
    if (s->number_CHR_banks == 0)
    {
        state.read(s->CHR_bank, 8 * 1024);
    }
}

static void Mapper002_destroy(Mapper *mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper002_state *state = (Mapper002_state *)mapper->state;
    releaseBankCache(&state->prg_cache);
    if (state->PRG_bank)
    {
        nes_port_free(state->PRG_bank);
        state->PRG_bank = nullptr;
    }
    if (state->CHR_bank)
    {
        nes_port_free(state->CHR_bank);
        state->CHR_bank = nullptr;
    }
    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper002_vtable = 
{
    Mapper002_cpuRead,
    Mapper002_cpuWrite,
    Mapper002_ppuRead,
    Mapper002_ppuWrite,
    Mapper002_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper002_reset,
    Mapper002_dumpState,
    Mapper002_loadState,
    Mapper002_destroy,
};

Mapper createMapper002(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    Mapper mapper;
    mapper.vtable = &Mapper002_vtable;
    Mapper002_state* state = new Mapper002_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }
    bankInit(&state->prg_cache,
             state->prg_banks,
             MAPPER002_NUM_PRG_BANKS_16K,
             16*1024,
             cart,
             MapperAllocPolicy::Psram);

    state->number_PRG_banks = PRG_banks;
    state->number_CHR_banks = CHR_banks;
    state->cart = cart;
    state->PRG_bank = static_cast<uint8_t*>(mapperAllocPsramZeroed(16 * 1024, "mapper002_prg_bank"));
    state->CHR_bank = static_cast<uint8_t*>(mapperAllocPsramZeroed(8 * 1024, "mapper002_chr_bank"));

    mapper.state = state;
    if (!state->PRG_bank ||
        !state->CHR_bank ||
        !bankCacheReady(&state->prg_cache))
    {
        Mapper002_destroy(&mapper);
        mapper.vtable = nullptr;
    }

    return mapper;
}

