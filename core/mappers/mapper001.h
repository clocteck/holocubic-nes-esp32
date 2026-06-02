#ifndef MAPPER001_H
#define MAPPER001_H

#include "../mapper.h"

#define MAPPER001_NUM_PRG_BANKS_16K 8
#define MAPPER001_NUM_CHR_BANKS_8K 1
#define MAPPER001_NUM_CHR_BANKS_4K 2

Mapper createMapper001(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif