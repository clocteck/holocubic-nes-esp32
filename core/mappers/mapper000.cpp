#include "mapper000.h"
#include "../cartridge.h"

IRAM_ATTR bool mapper000_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x8000) return false;

    Mapper000_state* state = (Mapper000_state*)mapper->state;
    uint16_t offset = addr & 0x3FFF;
    uint8_t bank_id = (addr >> 14) & 1;

    data = state->PRG_banks[bank_id][offset];
    return true;
}

IRAM_ATTR bool mapper000_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
	return false;
}

IRAM_ATTR bool mapper000_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF) return false;

    Mapper000_state* state = (Mapper000_state*)mapper->state;
    data = state->CHR_bank[addr];
    return true;
}

IRAM_ATTR bool mapper000_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF) return false;

    Mapper000_state* state = (Mapper000_state*)mapper->state;
    if (state->number_CHR_banks == 0)
    {
        // Treat as RAM
        state->CHR_bank[addr] = data;
        return true;
    }

	return false;
}

IRAM_ATTR uint8_t* mapper000_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF) return nullptr;
    
    Mapper000_state* state = (Mapper000_state*)mapper->state;
    return &state->CHR_bank[addr];
}

void mapper000_reset(Mapper* mapper)
{
    Mapper000_state* state = (Mapper000_state*)mapper->state;
    state->PRG_banks[0] = &state->PRG_ROM[0];
    state->PRG_banks[1] = (state->number_PRG_banks > 1) ? &state->PRG_ROM[16*1024] : &state->PRG_ROM[0];
    state->CHR_bank = &state->CHR_ROM[0];

    state->cart->loadPRGBank(state->PRG_banks[0], 16*1024, 0);
    if (state->number_PRG_banks > 1) state->cart->loadPRGBank(state->PRG_banks[1], 16*1024, 16*1024);
    state->cart->loadCHRBank(state->CHR_bank, 8*1024, 0);
}

void mapper000_dumpState(Mapper* mapper, File& state)
{
    Mapper000_state* s = (Mapper000_state*)mapper->state;

    if (s->number_CHR_banks == 0 && s->CHR_bank)
    {
        state.write(s->CHR_bank, 8*1024);
    }
}

void mapper000_loadState(Mapper* mapper, File& state)
{
    Mapper000_state* s = (Mapper000_state*)mapper->state;

    if (s->number_CHR_banks == 0 && s->CHR_bank)
    {
        state.read(s->CHR_bank, 8 * 1024);
    }
}

static void mapper000_destroy(Mapper *mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper000_state *state = (Mapper000_state *)mapper->state;
    if (state->PRG_ROM)
    {
        nes_port_free(state->PRG_ROM);
        state->PRG_ROM = nullptr;
    }
    if (state->CHR_ROM)
    {
        nes_port_free(state->CHR_ROM);
        state->CHR_ROM = nullptr;
    }
    delete state;
    mapper->state = nullptr;
}

const MapperVTable mapper000_vtable = 
{
    mapper000_cpuRead,
    mapper000_cpuWrite,
    mapper000_ppuRead,
    mapper000_ppuWrite,
    mapper000_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    mapper000_reset,
    mapper000_dumpState,
    mapper000_loadState,
    mapper000_destroy,
};

Mapper createMapper000(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    Mapper mapper;
    mapper.vtable = &mapper000_vtable; 

    Mapper000_state* state = new Mapper000_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }
    state->number_PRG_banks = PRG_banks;
    state->number_CHR_banks = CHR_banks;
    state->cart = cart;
    state->PRG_ROM = static_cast<uint8_t*>(mapperAllocPsramZeroed(32 * 1024, "mapper000_prg_rom"));
    state->CHR_ROM = static_cast<uint8_t*>(mapperAllocHotZeroed(8 * 1024, "mapper000_chr_rom"));

    mapper.state = state;
    if (!state->PRG_ROM || !state->CHR_ROM)
    {
        mapper000_destroy(&mapper);
        mapper.vtable = nullptr;
    }

    return mapper;
}

