#ifndef MAPPER_H
#define MAPPER_H

#include <cstring>
#include <SD.h>
#include <stdint.h>
#include <stdlib.h>

#include "Arduino.h"
#include "config.h"
#include "../debug.h"

class Cartridge;
struct MapperVTable;

class Mapper
{
public: 
    enum ROM_TYPE
    {
        PRG_ROM,
        CHR_ROM
    };

    const MapperVTable* vtable = nullptr;
    void* state = nullptr;
};

struct MapperVTable
{
    bool (*cpuRead)(Mapper* mapper, uint16_t addr, uint8_t& data);
    bool (*cpuWrite)(Mapper* mapper, uint16_t addr, uint8_t data);
    bool (*ppuRead)(Mapper* mapper, uint16_t addr, uint8_t& data);
    bool (*ppuWrite)(Mapper* mapper, uint16_t addr, uint8_t data);
    uint8_t* (*ppuReadPtr)(Mapper* mapper, uint16_t addr);
    void (*ppuAddress)(Mapper* mapper, uint16_t addr);
    void (*scanline)(Mapper* mapper);
    void (*cycle)(Mapper* mapper, int cycles);
    void (*reset)(Mapper* mapper);
    void (*dumpState)(Mapper* mapper, File& state);
    void (*loadState)(Mapper* mapper, File& state);
    void (*destroy)(Mapper* mapper);
};
IRAM_ATTR static void mapperNoPpuAddress(Mapper*, uint16_t) {}
IRAM_ATTR static void mapperNoScanline(Mapper*) {}
IRAM_ATTR static void mapperNoCycle(Mapper*, int) {}
static void mapperNoDestroy(Mapper*) {}

struct Bank
{
    uint8_t bank_id;
    uint8_t* bank_ptr;
    uint32_t last_used;
    uint32_t size;
};

struct BankCache
{
    Bank* banks;
    uint8_t num_banks;
    uint32_t tick;
    Cartridge* cart;
};

enum class MapperAllocPolicy : uint8_t
{
    Hot = 0,
    Psram
};

/**
 * @brief 为 mapper 热路径数据申请一块“内部 RAM 优先”的缓冲。
 *
 * 这类缓冲会在 CPU/PPU 的读写热路径中被频繁直接解引用，
 * 包括 bank cache、当前 PRG/CHR window、CHR RAM 等。
 * 默认优先内部 RAM；若内部 RAM 暂时不够，再自动回退到 PSRAM，
 * 避免因为一次大 mapper 初始化直接报 `mapper create failed`。
 *
 * @param size 申请字节数。
 * @param tag 仅用于日志诊断的分配标签，可传 nullptr。
 * @return void* 成功返回缓冲指针，失败返回 nullptr。
 */
void* mapperAllocHot(size_t size, const char* tag);

/**
 * @brief 为 mapper 热路径数据申请一块清零后的“内部优先”缓冲。
 *
 * @param size 申请字节数。
 * @param tag 仅用于日志诊断的分配标签，可传 nullptr。
 * @return void* 成功返回缓冲指针，失败返回 nullptr。
 */
void* mapperAllocHotZeroed(size_t size, const char* tag);

/**
 * @brief 为 mapper 冷路径/低频访问数据显式申请 PSRAM。
 *
 * 这里只适合保存 RAM 之类相对低频的数据块，不适合拿来承载
 * PRG/CHR bank cache 或当前映射窗口这类热数据。
 *
 * @param size 申请字节数。
 * @param tag 仅用于日志诊断的分配标签，可传 nullptr。
 * @return void* 成功返回缓冲指针，失败返回 nullptr。
 */
void* mapperAllocPsram(size_t size, const char* tag);

/**
 * @brief 为 mapper 冷路径数据申请一块清零后的 PSRAM。
 *
 * @param size 申请字节数。
 * @param tag 仅用于日志诊断的分配标签，可传 nullptr。
 * @return void* 成功返回缓冲指针，失败返回 nullptr。
 */
void* mapperAllocPsramZeroed(size_t size, const char* tag);

void bankInit(BankCache* cache,
              Bank* banks,
              uint8_t num_banks,
              uint32_t bank_size,
              Cartridge* cart,
              MapperAllocPolicy policy = MapperAllocPolicy::Hot);
uint8_t* getBank(BankCache* cache, uint8_t bank_id, Mapper::ROM_TYPE rom);
uint8_t getBankIndex(BankCache* cache, uint8_t* ptr);
bool bankCacheReady(const BankCache* cache);
void invalidateCache(BankCache* cache);
void releaseBankCache(BankCache* cache);

#endif
