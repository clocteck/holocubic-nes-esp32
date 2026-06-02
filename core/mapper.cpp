#include "mapper.h"

#include "cartridge.h"

static void mapperLogAllocFailure(const char *tag, size_t size, const char *region)
{
    LOGF("[mapper] alloc %s %u bytes in %s failed.\n",
         tag ? tag : "buffer",
         (unsigned)size,
         region ? region : "memory");
}

static const char *mapperAllocRegionOf(const void *ptr, const char *fallback)
{
    (void)ptr;
    return fallback ? fallback : "memory";
}

static void mapperLogAllocOk(const char *tag, size_t size, const void *ptr, const char *region)
{
    LOGF("[mapper] alloc %s %u bytes in %s ok, free internal: %u, free psram: %u\n",
         tag ? tag : "buffer",
         (unsigned)size,
         mapperAllocRegionOf(ptr, region),
         nes_port_host() && nes_port_host()->heap.free_size
             ? (unsigned)nes_port_host()->heap.free_size(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT)
             : 0U,
         nes_port_host() && nes_port_host()->heap.free_size
             ? (unsigned)nes_port_host()->heap.free_size(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT)
             : 0U);
}

void* mapperAllocHot(size_t size, const char* tag)
{
    /**
     * 热路径缓冲优先放内部 RAM。
     * 若内部 RAM 暂时不够，则回退到 PSRAM，避免直接导致 mapper 创建失败。
     */
    void* ptr = nes_port_malloc(size, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!ptr)
    {
        ptr = nes_port_malloc(size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
    }
    if (!ptr)
    {
        mapperLogAllocFailure(tag, size, "internal-preferred");
        return nullptr;
    }

    mapperLogAllocOk(tag, size, ptr, "internal-preferred");
    return ptr;
}

void* mapperAllocHotZeroed(size_t size, const char* tag)
{
    void* ptr = mapperAllocHot(size, tag);
    if (ptr)
    {
        memset(ptr, 0, size);
    }
    return ptr;
}

void* mapperAllocPsram(size_t size, const char* tag)
{
    void* ptr = nes_port_malloc(size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
    if (!ptr)
    {
        mapperLogAllocFailure(tag, size, "PSRAM");
        return nullptr;
    }

    mapperLogAllocOk(tag, size, ptr, "PSRAM");
    return ptr;
}

void* mapperAllocPsramZeroed(size_t size, const char* tag)
{
    void* ptr = mapperAllocPsram(size, tag);
    if (ptr)
    {
        memset(ptr, 0, size);
    }
    return ptr;
}

void bankInit(BankCache* cache,
              Bank* banks,
              uint8_t num_banks,
              uint32_t bank_size,
              Cartridge* cart,
              MapperAllocPolicy policy)
{
    cache->banks = banks;
    cache->num_banks = num_banks;
    cache->tick = 0;
    cache->cart = cart;

    for (int i = 0; i < num_banks; i++)
    {
        cache->banks[i].bank_id = 0xFF;
        cache->banks[i].last_used = 0;
        cache->banks[i].size = bank_size;

        uint8_t* ptr = nullptr;
        if (policy == MapperAllocPolicy::Psram)
        {
            ptr = static_cast<uint8_t*>(mapperAllocPsram(bank_size, "bank_cache"));
        }
        else
        {
            ptr = static_cast<uint8_t*>(mapperAllocHot(bank_size, "bank_cache"));
        }
        cache->banks[i].bank_ptr = ptr;
    }
}

IRAM_ATTR uint8_t* getBank(BankCache* cache, uint8_t bank_id, Mapper::ROM_TYPE rom)
{
    cache->tick++;

    for (int i = 0, n = cache->num_banks; i < n; i++)
    {
        if (cache->banks[i].bank_id == bank_id)
        {
            cache->banks[i].last_used = cache->tick;
            return cache->banks[i].bank_ptr;
        }
    }

    int bank_index = 0;
    uint32_t min_use = cache->banks[0].last_used;
    for (int i = 1, n = cache->num_banks; i < n; i++)
    {
        if (cache->banks[i].last_used < min_use)
        {
            min_use = cache->banks[i].last_used;
            bank_index = i;
        }
    } 

    uint8_t* bank = cache->banks[bank_index].bank_ptr;
    uint32_t size = cache->banks[bank_index].size;
    if (rom == Mapper::ROM_TYPE::PRG_ROM) cache->cart->loadPRGBank(bank, size, bank_id * size);
    else if (rom == Mapper::ROM_TYPE::CHR_ROM) cache->cart->loadCHRBank(bank, size, bank_id * size);

    cache->banks[bank_index].bank_id = bank_id;
    cache->banks[bank_index].last_used = cache->tick;

    return bank;
}

uint8_t getBankIndex(BankCache* cache, uint8_t* ptr)
{
    for (int i = 0, banks = cache->num_banks; i < banks; i++)
    {
        if (cache->banks[i].bank_ptr == ptr)
            return cache->banks[i].bank_id;
    }

    return 0;
}

bool bankCacheReady(const BankCache *cache)
{
    if (!cache)
    {
        return false;
    }

    if (cache->num_banks == 0)
    {
        return true;
    }

    if (!cache->banks)
    {
        return false;
    }

    for (int i = 0, n = cache->num_banks; i < n; i++)
    {
        if (!cache->banks[i].bank_ptr)
        {
            return false;
        }
    }

    return true;
}

void invalidateCache(BankCache* cache)
{
    if (!cache || !cache->banks) return;

    cache->tick = 0;
    for (int i = 0, n = cache->num_banks; i < n; i++)
    {
        cache->banks[i].bank_id = 0xFF;
        cache->banks[i].last_used = 0;
    }
}

void releaseBankCache(BankCache *cache)
{
    if (!cache || !cache->banks)
    {
        return;
    }

    for (int i = 0, n = cache->num_banks; i < n; i++)
    {
        if (cache->banks[i].bank_ptr)
        {
            nes_port_free(cache->banks[i].bank_ptr);
            cache->banks[i].bank_ptr = nullptr;
        }
        cache->banks[i].bank_id = 0xFF;
        cache->banks[i].last_used = 0;
        cache->banks[i].size = 0;
    }

    cache->banks = nullptr;
    cache->num_banks = 0;
    cache->tick = 0;
    cache->cart = nullptr;
}

