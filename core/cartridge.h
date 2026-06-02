#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <Arduino.h>

#include "mapper.h"
#include "mappers/mapper000.h"
#include "mappers/mapper001.h"
#include "mappers/mapper002.h"
#include "mappers/mapper003.h"
#include "mappers/mapper004.h"
#include "mappers/mapper015.h"
#include "mappers/mapper007.h"
#include "mappers/mapper226.h"
#include "mappers/mapper069.h"

class Bus;
class Cartridge
{
public:
    Cartridge(const char *filename);
    ~Cartridge();

    enum MIRROR
    {
        HORIZONTAL,
        VERTICAL,
        ONESCREEN_LOW,
        ONESCREEN_HIGH,
        FOURSCREEN,
        HARDWARE
    };

    bool cpuRead(uint16_t addr, uint8_t &data);
    bool cpuWrite(uint16_t addr, uint8_t data);
    bool ppuRead(uint16_t addr, uint8_t &data);
    uint8_t* ppuReadPtr(uint16_t addr);
    bool ppuWrite(uint16_t addr, uint8_t data);
    void notifyPpuAddress(uint16_t addr);
    void ppuScanline();
    void cpuCycle(int cycles);
    void reset();
    
    void loadPRGBank(uint8_t* bank, uint16_t size, uint32_t offset);
    void loadCHRBank(uint8_t* bank, uint16_t size, uint32_t offset);
    void setMirrorMode(MIRROR mirror);
    Cartridge::MIRROR getMirrorMode();
    void connectBus(Bus* n) { bus = n; }
    void IRQ();
    bool usesFixedFourScreenMirroring() const { return hardware_mirror == FOURSCREEN; }

    void dumpState(File& state);
    void loadState(File& state);
    bool ready() const;
    const String &lastError() const;
    uint8_t mapperId() const;

    uint8_t hardware_mirror = HORIZONTAL;
    uint8_t mirror = HORIZONTAL;
    uint32_t CRC32 = ~0U;

private:
    /**
     * @brief ROM 预载结果。
     *
     * 说明：
     * 1. `Success` 表示 PRG/CHR ROM 已完整预载到 PSRAM；
     * 2. `Skipped` 表示因为 PSRAM 不足等非致命原因跳过预载，后续继续沿用文件读取回退；
     * 3. `Fatal` 表示 ROM 文件本身读取失败，构造应直接终止。
     */
    enum class RomPreloadResult : uint8_t
    {
        Success = 0,
        Skipped,
        Fatal
    };

    Bus *bus = nullptr;
    uint32_t prg_base;
    uint32_t chr_base;
    uint32_t m_prg_rom_size = 0;
    uint32_t m_chr_rom_size = 0;

    File rom;
    Mapper mapper;
    uint8_t mapper_ID = 0;
    uint8_t number_PRG_banks = 0;
    uint8_t number_CHR_banks = 0;
    uint8_t *m_prg_rom = nullptr;
    uint8_t *m_chr_rom = nullptr;
    bool m_ready = false;
    bool m_has_ppu_address_callback = false;
    bool m_has_ppu_scanline_callback = false;
    bool m_has_cpu_cycle_callback = false;
    String m_last_error;
    MapperVTable m_mapper_vtable = {};

    /**
     * @brief 创建当前 mapper 实例。
     *
     * 这里把 switch 集中起来，方便在“开启 ROM 预载”和“回退文件模式”之间重试。
     */
    Mapper createMapperInstance();

    /**
     * @brief 绑定一份当前 mapper 函数表的可执行地址副本。
     *
     * ESP-ELFLoader 会把 `.so` 的 text 映射到 IROM 执行区，但 const
     * 函数表里的指针可能仍保留数据区别名；热路径只调用这份已修正副本。
     */
    void bindMapperVTable();

    /**
     * @brief 标记当前 mapper 哪些可选时序回调不是 no-op。
     *
     * 这只跳过 `mapperNo*` 空回调，读写、复位、IRQ 等 mapper 语义保持不变。
     */
    void bindMapperFeatureFlags();

    /**
     * @brief 尝试把整个 PRG/CHR ROM 预载到 PSRAM，并顺手完成 CRC32。
     */
    RomPreloadResult preloadRomToPsram();

    /**
     * @brief 从文件里读取一整段 ROM 到指定缓冲。
     *
     * @param file_offset ROM 文件内偏移。
     * @param buffer 目标缓冲。
     * @param size 读取字节数。
     * @param crc 可选 CRC 累加器，非空时会边读边累计。
     */
    bool readRomSegment(uint32_t file_offset, uint8_t *buffer, uint32_t size, uint32_t *crc = nullptr);

    /**
     * @brief 在未启用整包预载时，单独从 ROM 文件计算 CRC32。
     */
    bool computeRomCrcFromFile();

    /**
     * @brief 释放当前已预载到 PSRAM 的整包 ROM。
     */
    void releasePreloadedRom();

    uint32_t crc32(const void *buf, size_t size, uint32_t seed = ~0U);
};

#endif
