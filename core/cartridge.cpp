#include "cartridge.h"

#include <string.h>

#include "bus.h"
#include "nes_port.h"

Cartridge::Cartridge(const char *filename)
{
    struct cartridge_header
    {
        char name[4];
        uint8_t PRG_ROM_chunks;
        uint8_t CHR_ROM_chunks;
        uint8_t mapper1;
        uint8_t mapper2;
        uint8_t PRG_RAM_size;
        uint8_t tv_system;
        uint8_t tv_system2;
        char unused[5];
    } header;

    if (!filename || filename[0] == '\0')
    {
        m_last_error = "rom path missing";
        return;
    }

    if (!rom.open(filename, FILE_READ))
    {
        m_last_error = String("open rom failed: ") + filename;
        return;
    }

    if (rom.read((uint8_t *)&header, sizeof(cartridge_header)) != sizeof(cartridge_header))
    {
        m_last_error = "read rom header failed";
        rom.close();
        return;
    }

    if (header.name[0] != 'N' ||
        header.name[1] != 'E' ||
        header.name[2] != 'S' ||
        (uint8_t)header.name[3] != 0x1A)
    {
        m_last_error = "invalid iNES header";
        rom.close();
        return;
    }

    if ((header.mapper2 & 0x0C) == 0x08)
    {
        m_last_error = "NES 2.0 rom is not supported yet";
        rom.close();
        return;
    }

    if (header.mapper1 & 0x04)
    {
        rom.seek(rom.position() + 512);
    }

    mapper_ID = (header.mapper2 & 0xF0) | (header.mapper1 >> 4);
    hardware_mirror = (header.mapper1 & 0x08) ? FOURSCREEN : ((header.mapper1 & 0x01) ? VERTICAL : HORIZONTAL);
    mirror = hardware_mirror;

    number_PRG_banks = header.PRG_ROM_chunks;
    number_CHR_banks = header.CHR_ROM_chunks;

    prg_base = sizeof(cartridge_header) + (header.mapper1 & 0x04 ? 512 : 0);
    chr_base = prg_base + (number_PRG_banks * 16384);
    m_prg_rom_size = (uint32_t)number_PRG_banks * 16384U;
    m_chr_rom_size = (uint32_t)number_CHR_banks * 8192U;

    const RomPreloadResult preload_result = preloadRomToPsram();
    if (preload_result == RomPreloadResult::Fatal)
    {
        rom.close();
        return;
    }

    mapper = createMapperInstance();
    if (!mapper.vtable && preload_result == RomPreloadResult::Success)
    {
        /**
         * 说明：
         * 1. 全量预载会额外占一份 PSRAM；
         * 2. 若某些大 ROM 在“预载 + mapper 缓存”同时存在时触顶，
         *    这里自动回退到旧的文件装载模式，避免把原本能跑的 ROM 直接变成启动失败。
         */
        LOGF("[nes] mapper create failed with preloaded rom, retry without preload\n");
        releasePreloadedRom();
        mapper = createMapperInstance();
    }

    if (!mapper.vtable)
    {
        if (m_last_error.length() == 0)
        {
            m_last_error = "mapper create failed";
        }
        rom.close();
        return;
    }

    bindMapperVTable();
    bindMapperFeatureFlags();

    if (preload_result != RomPreloadResult::Success && !computeRomCrcFromFile())
    {
        rom.close();
        return;
    }

    LOGF("CRC32: %08X\n", CRC32);
    m_ready = true;
}

Cartridge::~Cartridge()
{
    releasePreloadedRom();

    if (mapper.vtable && mapper.vtable->destroy)
    {
        mapper.vtable->destroy(&mapper);
    }
    mapper.vtable = nullptr;
    mapper.state = nullptr;

    if (rom)
    {
        rom.close();
    }
}


IRAM_ATTR bool Cartridge::cpuRead(uint16_t addr, uint8_t& data)
{
    if (!mapper.vtable)
    {
        return false;
    }
	return mapper.vtable->cpuRead(&mapper, addr, data);
}

IRAM_ATTR bool Cartridge::cpuWrite(uint16_t addr, uint8_t data)
{
    if (!mapper.vtable)
    {
        return false;
    }
	return mapper.vtable->cpuWrite(&mapper, addr, data);
}

IRAM_ATTR bool Cartridge::ppuRead(uint16_t addr, uint8_t& data)
{
    if (!mapper.vtable)
    {
        return false;
    }
	return mapper.vtable->ppuRead(&mapper, addr, data);
}

IRAM_ATTR bool Cartridge::ppuWrite(uint16_t addr, uint8_t data)
{
    if (!mapper.vtable)
    {
        return false;
    }
	return mapper.vtable->ppuWrite(&mapper, addr, data);	
}

IRAM_ATTR void Cartridge::notifyPpuAddress(uint16_t addr)
{
    if (!m_has_ppu_address_callback || !mapper.vtable || !mapper.vtable->ppuAddress)
    {
        return;
    }

    mapper.vtable->ppuAddress(&mapper, addr);
}

IRAM_ATTR uint8_t* Cartridge::ppuReadPtr(uint16_t addr)
{
    if (!mapper.vtable)
    {
        return nullptr;
    }
	return mapper.vtable->ppuReadPtr(&mapper, addr);
}

void Cartridge::ppuScanline()
{
    if (m_has_ppu_scanline_callback && mapper.vtable)
    {
        mapper.vtable->scanline(&mapper);
    }
}

void Cartridge::cpuCycle(int cycles)
{
    if (m_has_cpu_cycle_callback && mapper.vtable)
    {
        mapper.vtable->cycle(&mapper, cycles);
    }
}

void Cartridge::reset()
{
    if (mapper.vtable)
    {
        mapper.vtable->reset(&mapper);
    }
}

IRAM_ATTR void Cartridge::loadPRGBank(uint8_t* bank, uint16_t size, uint32_t offset)
{
    if (!bank || size == 0)
    {
        return;
    }

    if (m_prg_rom && (uint32_t)offset + (uint32_t)size <= m_prg_rom_size)
    {
        memcpy(bank, m_prg_rom + offset, size);
        return;
    }

    if (!rom)
    {
        return;
    }
    rom.seek(prg_base + offset);
    rom.read(bank, size);
}

IRAM_ATTR void Cartridge::loadCHRBank(uint8_t* bank, uint16_t size, uint32_t offset)
{
    if (!bank || size == 0)
    {
        return;
    }

    if (m_chr_rom && (uint32_t)offset + (uint32_t)size <= m_chr_rom_size)
    {
        memcpy(bank, m_chr_rom + offset, size);
        return;
    }

    if (!rom)
    {
        return;
    }
    rom.seek(chr_base + offset);
    rom.read(bank, size);
}

IRAM_ATTR void Cartridge::setMirrorMode(MIRROR mirror)
{
    this->mirror = (uint8_t)mirror;
    if (bus)
    {
        bus->setPPUMirrorMode(mirror);
    }
}

Cartridge::MIRROR Cartridge::getMirrorMode()
{
    if (bus)
    {
        return bus->getPPUMirrorMode();
    }
    return (Cartridge::MIRROR)mirror;
}

void Cartridge::IRQ()
{
    if (bus)
    {
        bus->IRQ();
    }
}

void Cartridge::dumpState(File& state)
{
    if (mapper.vtable)
    {
        mapper.vtable->dumpState(&mapper, state);
    }
}

void Cartridge::loadState(File& state)
{
    if (mapper.vtable)
    {
        mapper.vtable->loadState(&mapper, state);
    }
}

bool Cartridge::ready() const
{
    return m_ready;
}

const String &Cartridge::lastError() const
{
    return m_last_error;
}

uint8_t Cartridge::mapperId() const
{
    return mapper_ID;
}

Mapper Cartridge::createMapperInstance()
{
    switch (mapper_ID)
    {
        case 0: return createMapper000(number_PRG_banks, number_CHR_banks, this);
        case 1: return createMapper001(number_PRG_banks, number_CHR_banks, this);
        case 2: return createMapper002(number_PRG_banks, number_CHR_banks, this);
        case 3: return createMapper003(number_PRG_banks, number_CHR_banks, this);
        case 4: return createMapper004(number_PRG_banks, number_CHR_banks, this);
        case 15: return createMapper015(number_PRG_banks, number_CHR_banks, this);
        case 7: return createMapper007(number_PRG_banks, number_CHR_banks, this);
        case 226: return createMapper226(number_PRG_banks, number_CHR_banks, this);
        case 69: return createMapper069(number_PRG_banks, number_CHR_banks, this);
        default:
            m_last_error = String("unsupported mapper: ") + String(mapper_ID);
            return Mapper{};
    }
}

void Cartridge::bindMapperVTable()
{
    if (!mapper.vtable)
    {
        return;
    }

    m_mapper_vtable = *mapper.vtable;
    m_mapper_vtable.cpuRead = (bool (*)(Mapper *, uint16_t, uint8_t &))nes_port_exec_ptr((const void *)m_mapper_vtable.cpuRead);
    m_mapper_vtable.cpuWrite = (bool (*)(Mapper *, uint16_t, uint8_t))nes_port_exec_ptr((const void *)m_mapper_vtable.cpuWrite);
    m_mapper_vtable.ppuRead = (bool (*)(Mapper *, uint16_t, uint8_t &))nes_port_exec_ptr((const void *)m_mapper_vtable.ppuRead);
    m_mapper_vtable.ppuWrite = (bool (*)(Mapper *, uint16_t, uint8_t))nes_port_exec_ptr((const void *)m_mapper_vtable.ppuWrite);
    m_mapper_vtable.ppuReadPtr = (uint8_t *(*)(Mapper *, uint16_t))nes_port_exec_ptr((const void *)m_mapper_vtable.ppuReadPtr);
    m_mapper_vtable.ppuAddress = (void (*)(Mapper *, uint16_t))nes_port_exec_ptr((const void *)m_mapper_vtable.ppuAddress);
    m_mapper_vtable.scanline = (void (*)(Mapper *))nes_port_exec_ptr((const void *)m_mapper_vtable.scanline);
    m_mapper_vtable.cycle = (void (*)(Mapper *, int))nes_port_exec_ptr((const void *)m_mapper_vtable.cycle);
    m_mapper_vtable.reset = (void (*)(Mapper *))nes_port_exec_ptr((const void *)m_mapper_vtable.reset);
    m_mapper_vtable.dumpState = (void (*)(Mapper *, File &))nes_port_exec_ptr((const void *)m_mapper_vtable.dumpState);
    m_mapper_vtable.loadState = (void (*)(Mapper *, File &))nes_port_exec_ptr((const void *)m_mapper_vtable.loadState);
    m_mapper_vtable.destroy = (void (*)(Mapper *))nes_port_exec_ptr((const void *)m_mapper_vtable.destroy);
    mapper.vtable = &m_mapper_vtable;
}

void Cartridge::bindMapperFeatureFlags()
{
    m_has_ppu_address_callback = false;
    m_has_ppu_scanline_callback = false;
    m_has_cpu_cycle_callback = false;

    switch (mapper_ID)
    {
        case 4:
            m_has_ppu_address_callback = true;
            break;
        case 69:
            m_has_cpu_cycle_callback = true;
            break;
        default:
            break;
    }
}

Cartridge::RomPreloadResult Cartridge::preloadRomToPsram()
{
    uint32_t crc = ~0U;

    if (m_prg_rom_size > 0)
    {
        m_prg_rom = static_cast<uint8_t *>(mapperAllocPsram(m_prg_rom_size, "cartridge_prg_rom"));
        if (!m_prg_rom)
        {
            LOGF("[nes] preload full PRG ROM skipped, fallback to file-backed bank load\n");
            releasePreloadedRom();
            return RomPreloadResult::Skipped;
        }

        if (!readRomSegment(prg_base, m_prg_rom, m_prg_rom_size, &crc))
        {
            releasePreloadedRom();
            return RomPreloadResult::Fatal;
        }
    }

    if (m_chr_rom_size > 0)
    {
        m_chr_rom = static_cast<uint8_t *>(mapperAllocPsram(m_chr_rom_size, "cartridge_chr_rom"));
        if (!m_chr_rom)
        {
            LOGF("[nes] preload full CHR ROM skipped, fallback to file-backed bank load\n");
            releasePreloadedRom();
            return RomPreloadResult::Skipped;
        }

        if (!readRomSegment(chr_base, m_chr_rom, m_chr_rom_size, &crc))
        {
            releasePreloadedRom();
            return RomPreloadResult::Fatal;
        }
    }

    CRC32 = crc ^ ~0U;
    LOGF("[nes] full ROM preloaded to PSRAM, PRG=%u bytes, CHR=%u bytes\n",
         (unsigned)m_prg_rom_size,
         (unsigned)m_chr_rom_size);
    return RomPreloadResult::Success;
}

bool Cartridge::readRomSegment(uint32_t file_offset, uint8_t *buffer, uint32_t size, uint32_t *crc)
{
    if (!buffer || size == 0)
    {
        return true;
    }

    if (!rom)
    {
        m_last_error = "rom file handle unavailable";
        return false;
    }

    if (!rom.seek(file_offset))
    {
        m_last_error = "seek rom segment failed";
        return false;
    }

    uint32_t total_read = 0;
    while (total_read < size)
    {
        const size_t chunk = rom.read(buffer + total_read, size - total_read);
        if (chunk == 0)
        {
            m_last_error = "read rom segment failed";
            return false;
        }

        if (crc)
        {
            *crc = crc32(buffer + total_read, chunk, *crc);
        }
        total_read += (uint32_t)chunk;
    }

    return true;
}

bool Cartridge::computeRomCrcFromFile()
{
    uint8_t buf[1024];
    uint32_t crc = ~0U;

    if (!rom)
    {
        m_last_error = "rom file handle unavailable";
        return false;
    }

    if (!rom.seek(prg_base))
    {
        m_last_error = "seek rom crc failed";
        return false;
    }

    size_t len = 0;
    while ((len = rom.read(buf, sizeof(buf))) > 0)
    {
        crc = crc32(buf, len, crc);
    }

    CRC32 = crc ^ ~0U;
    return true;
}

void Cartridge::releasePreloadedRom()
{
    if (m_prg_rom)
    {
        nes_port_free(m_prg_rom);
        m_prg_rom = nullptr;
    }

    if (m_chr_rom)
    {
        nes_port_free(m_chr_rom);
        m_chr_rom = nullptr;
    }
}

uint32_t Cartridge::crc32(const void* buf, size_t size, uint32_t seed)
{
    static constexpr uint32_t crc32_table[256] = 
    {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };

    const uint8_t* p = (const uint8_t*)buf;
    uint32_t crc;

    crc = seed;
    while (size--)
        crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

