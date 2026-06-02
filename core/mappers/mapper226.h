#ifndef MAPPER226_H
#define MAPPER226_H

#include "../mapper.h"

// Mapper 226 已知最大到 2 MiB PRG-ROM
// 2 MiB / 16 KiB = 128 banks
#define MAPPER226_NUM_PRG_BANKS_16K 128

Mapper createMapper226(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif