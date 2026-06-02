#include "mapper226.h"

#include "../../nes_config.h"
#include "../cartridge.h"
#include "../power_on_ram.h"

struct Mapper226_state
{
    Cartridge* cart = nullptr;

    // iNES 里传进来的 PRG bank 数，单位是 16 KiB
    uint8_t number_PRG_banks = 0;

    uint8_t register_8000 = 0x00;
    uint8_t register_8001 = 0x00;

    uint8_t* ptr_PRG_bank_16K[2] = {};
    uint8_t* CHR_RAM = nullptr;

    Bank PRG_banks_16K[MAPPER226_NUM_PRG_BANKS_16K];
    BankCache PRG_cache_16K;

    uint16_t trace_writes_left = NES_MAPPER226_TRACE_MAX_WRITES;

    static constexpr Cartridge::MIRROR mirror_lut[2] =
    {
        Cartridge::MIRROR::HORIZONTAL,
        Cartridge::MIRROR::VERTICAL
    };
};
constexpr Cartridge::MIRROR Mapper226_state::mirror_lut[2];

static uint8_t Mapper226_wrap16KBank(const Mapper226_state* state, uint32_t bank)
{
    if (!state || state->number_PRG_banks == 0)
    {
        return 0;
    }

    return static_cast<uint8_t>(bank % state->number_PRG_banks);
}

static uint8_t Mapper226_decodeLogical16KBank(const Mapper226_state* state)
{
    if (!state)
    {
        return 0;
    }

    /**
     * MAME / NESdev 当前对 226 的主流实现都可归纳为：
     *
     * `$8000`: 提供 PRG 低 6 位，同时含 mirroring / mode 位；
     * `$8001`: bit0 提供 PRG 最高位。
     *
     * 其中：
     * 1. 线性逻辑 bank 单位可以直接看成 16 KiB；
     * 2. 当 bit5=0 时工作在 32 KiB 成对模式，最低位会被硬件忽略；
     * 3. 当 bit5=1 时工作在 16 KiB 重复模式，整个 `$8000-$FFFF` 复用同一 16 KiB bank。
     *
     * 这个 7-bit 线性 bank 写法和 226.txt 里“32 KiB page + q 半块位”的描述
     * 在功能上是等价的，但更适合后面处理 63-in-1 那种非线性物理封装。
     */
    return static_cast<uint8_t>(
        (state->register_8000 & 0x1FU) |
        ((state->register_8000 & 0x80U) >> 2) |
        ((state->register_8001 & 0x01U) << 6)
    );
}

static bool Mapper226_is16KMode(const Mapper226_state* state)
{
    return state && ((state->register_8000 & 0x20U) != 0);
}

static uint8_t Mapper226_resolvePhysical16KBank(const Mapper226_state* state, uint8_t logical_bank)
{
    if (!state || state->number_PRG_banks == 0)
    {
        return 0;
    }

    /**
     * 63-in-1 常见为 1.5 MiB PRG，也就是 96 个 16 KiB bank。
     *
     * MAME 对这块板的说明是：
     * 1. 实板常见为 3 颗 512 KiB ROM；
     * 2. 逻辑上 7-bit bank 的高两位像是在选 4 个“chip slot”；
     * 3. 但其中有一个 slot 会再次映射到第一颗 512 KiB 芯片。
     *
     * 结果就是：逻辑 bank 0x20-0x3F 实际应回到物理 0x00-0x1F，
     * 而不是简单线性落到文件里的第二段。
     *
     * 如果这里继续对 96 直接 `% 96`，63-in-1 的菜单经常会切到错误游戏页，
     * 现象就是“菜单有了，但选择项没有正确启动”。
     */
    if (state->number_PRG_banks == 96)
    {
        logical_bank &= 0x7FU;
        if (logical_bank >= 0x20U)
        {
            return static_cast<uint8_t>(logical_bank - 0x20U);
        }
        return logical_bank;
    }

    return Mapper226_wrap16KBank(state, logical_bank);
}

static uint8_t Mapper226_currentPrgRomByte(const Mapper226_state* state, uint16_t addr)
{
    if (!state || addr < 0x8000)
    {
        return 0xFF;
    }

    const uint8_t bank = (addr >> 14) & 0x01U;
    const uint8_t* const window = state->ptr_PRG_bank_16K[bank];
    if (!window)
    {
        return 0xFF;
    }

    return window[addr & 0x3FFFU];
}

static uint8_t Mapper226_applyBusConflict(const Mapper226_state* state, uint16_t addr, uint8_t data)
{
#if NES_MAPPER226_ENABLE_BUS_CONFLICT
    return static_cast<uint8_t>(data & Mapper226_currentPrgRomByte(state, addr));
#else
    (void)state;
    (void)addr;
    return data;
#endif
}

static void Mapper226_traceWrite(Mapper226_state* state,
                                 uint16_t addr,
                                 uint8_t raw_data,
                                 uint8_t effective_data,
                                 uint8_t sampled_rom_data)
{
#if NES_MAPPER226_TRACE_WRITES
    if (!state || state->trace_writes_left == 0)
    {
        return;
    }

    const uint8_t logical_bank = Mapper226_decodeLogical16KBank(state);
    const uint8_t physical_bank_0 = getBankIndex(&state->PRG_cache_16K, state->ptr_PRG_bank_16K[0]);
    const uint8_t physical_bank_1 = getBankIndex(&state->PRG_cache_16K, state->ptr_PRG_bank_16K[1]);
    const char* const mode_text = Mapper226_is16KMode(state) ? "16Kx2" : "32Kpair";

    LOGF("[nes][m226] write addr=%04X raw=%02X eff=%02X rom=%02X reg0=%02X reg1=%02X mode=%s logical=%u phys0=%u phys1=%u left=%u\n",
         addr,
         raw_data,
         effective_data,
         sampled_rom_data,
         state->register_8000,
         state->register_8001,
         mode_text,
         (unsigned)logical_bank,
         (unsigned)physical_bank_0,
         (unsigned)physical_bank_1,
         (unsigned)(state->trace_writes_left - 1U));
    state->trace_writes_left--;
#else
    (void)state;
    (void)addr;
    (void)raw_data;
    (void)effective_data;
    (void)sampled_rom_data;
#endif
}

static void Mapper226_updateMirroring(Mapper226_state* state)
{
    if (!state || !state->cart)
    {
        return;
    }

    state->cart->setMirrorMode(
        Mapper226_state::mirror_lut[(state->register_8000 >> 6) & 0x01U]
    );
}

static void Mapper226_updatePrgBanks(Mapper226_state* state)
{
    if (!state || state->number_PRG_banks == 0)
    {
        return;
    }

    const uint8_t logical_bank = Mapper226_decodeLogical16KBank(state);

    if (Mapper226_is16KMode(state))
    {
        const uint8_t bank = Mapper226_resolvePhysical16KBank(state, logical_bank);
        state->ptr_PRG_bank_16K[0] =
            getBank(&state->PRG_cache_16K, bank, Mapper::ROM_TYPE::PRG_ROM);
        state->ptr_PRG_bank_16K[1] = state->ptr_PRG_bank_16K[0];
        return;
    }

    // 32 KiB 模式：硬件会忽略最低位，映射连续两页
    const uint8_t bank0 = Mapper226_resolvePhysical16KBank(state, (uint8_t)(logical_bank & 0xFEU));
    const uint8_t bank1 = Mapper226_resolvePhysical16KBank(state, (uint8_t)(logical_bank | 0x01U));

    state->ptr_PRG_bank_16K[0] =
        getBank(&state->PRG_cache_16K, bank0, Mapper::ROM_TYPE::PRG_ROM);
    state->ptr_PRG_bank_16K[1] =
        getBank(&state->PRG_cache_16K, bank1, Mapper::ROM_TYPE::PRG_ROM);
}

IRAM_ATTR bool Mapper226_cpuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr < 0x8000)
    {
        return false;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;
    const uint8_t bank = (addr >> 14) & 0x01U;
    data = state->ptr_PRG_bank_16K[bank][addr & 0x3FFFU];
    return true;
}

IRAM_ATTR bool Mapper226_cpuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr < 0x8000)
    {
        return false;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;
    const uint8_t raw_data = data;
    const uint8_t sampled_rom_data = Mapper226_currentPrgRomByte(state, addr);
    data = Mapper226_applyBusConflict(state, addr, data);

    // NESdev: range = $8000-$FFFF, mask = $8001
    // 所以只看 A0：偶地址 -> $8000，奇地址 -> $8001
    if (addr & 0x0001U)
    {
        state->register_8001 = data;
    }
    else
    {
        state->register_8000 = data;
    }

    Mapper226_updatePrgBanks(state);
    Mapper226_updateMirroring(state);
    Mapper226_traceWrite(state, addr, raw_data, data, sampled_rom_data);
    return true;
}

IRAM_ATTR bool Mapper226_ppuRead(Mapper* mapper, uint16_t addr, uint8_t& data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;
    data = state->CHR_RAM[addr];
    return true;
}

IRAM_ATTR bool Mapper226_ppuWrite(Mapper* mapper, uint16_t addr, uint8_t data)
{
    if (addr > 0x1FFF)
    {
        return false;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;

    // hack-friendly：忽略 CHR-RAM 写保护位，始终允许写
    state->CHR_RAM[addr] = data;
    return true;
}

IRAM_ATTR uint8_t* Mapper226_ppuReadPtr(Mapper* mapper, uint16_t addr)
{
    if (addr > 0x1FFF)
    {
        return nullptr;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;
    return &state->CHR_RAM[addr];
}

void Mapper226_reset(Mapper* mapper)
{
    Mapper226_state* state = (Mapper226_state*)mapper->state;
    if (!state)
    {
        return;
    }

    state->register_8000 = 0x00;
    state->register_8001 = 0x00;
    state->trace_writes_left = NES_MAPPER226_TRACE_MAX_WRITES;

    invalidateCache(&state->PRG_cache_16K);
    Mapper226_updatePrgBanks(state);
    Mapper226_updateMirroring(state);
}

void Mapper226_dumpState(Mapper* mapper, File& stateFile)
{
    Mapper226_state* s = (Mapper226_state*)mapper->state;
    stateFile.write((uint8_t*)&s->register_8000, sizeof(s->register_8000));
    stateFile.write((uint8_t*)&s->register_8001, sizeof(s->register_8001));
    stateFile.write(s->CHR_RAM, 8 * 1024);
}

void Mapper226_loadState(Mapper* mapper, File& stateFile)
{
    Mapper226_state* s = (Mapper226_state*)mapper->state;
    stateFile.read((uint8_t*)&s->register_8000, sizeof(s->register_8000));
    stateFile.read((uint8_t*)&s->register_8001, sizeof(s->register_8001));
    stateFile.read(s->CHR_RAM, 8 * 1024);

    invalidateCache(&s->PRG_cache_16K);
    Mapper226_updatePrgBanks(s);
    Mapper226_updateMirroring(s);
}

static void Mapper226_destroy(Mapper* mapper)
{
    if (!mapper || !mapper->state)
    {
        return;
    }

    Mapper226_state* state = (Mapper226_state*)mapper->state;

    releaseBankCache(&state->PRG_cache_16K);

    if (state->CHR_RAM)
    {
        nes_port_free(state->CHR_RAM);
        state->CHR_RAM = nullptr;
    }

    delete state;
    mapper->state = nullptr;
}

const MapperVTable Mapper226_vtable =
{
    Mapper226_cpuRead,
    Mapper226_cpuWrite,
    Mapper226_ppuRead,
    Mapper226_ppuWrite,
    Mapper226_ppuReadPtr,
    mapperNoPpuAddress,
    mapperNoScanline,
    mapperNoCycle,
    Mapper226_reset,
    Mapper226_dumpState,
    Mapper226_loadState,
    Mapper226_destroy,
};

Mapper createMapper226(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart)
{
    Mapper mapper{};
    mapper.vtable = &Mapper226_vtable;

    Mapper226_state* state = new Mapper226_state{};
    if (!state)
    {
        mapper.vtable = nullptr;
        return mapper;
    }

    state->cart = cart;
    state->number_PRG_banks = PRG_banks;

    // Mapper 226 通常视为固定 8 KiB CHR-RAM
    (void)CHR_banks;

    bankInit(&state->PRG_cache_16K,
             state->PRG_banks_16K,
             MAPPER226_NUM_PRG_BANKS_16K,
             16 * 1024,
             cart,
             MapperAllocPolicy::Psram);

    state->CHR_RAM = static_cast<uint8_t*>(mapperAllocHot(8 * 1024, "mapper226_chr_ram"));
    if (state->CHR_RAM)
    {
        nes::power_on::fillVolatileRam(state->CHR_RAM, 8 * 1024, 0x2260C8A0u);
    }

    mapper.state = state;

    if (PRG_banks == 0 || !state->CHR_RAM || !bankCacheReady(&state->PRG_cache_16K))
    {
        Mapper226_destroy(&mapper);
        mapper.vtable = nullptr;
        return mapper;
    }

    // 立即建立初始映射，避免后续路径读到空 bank 指针
    state->register_8000 = 0x00;
    state->register_8001 = 0x00;
    invalidateCache(&state->PRG_cache_16K);
    Mapper226_updatePrgBanks(state);
    Mapper226_updateMirroring(state);

    return mapper;
}

