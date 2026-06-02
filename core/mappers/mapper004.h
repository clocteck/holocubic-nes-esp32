#ifndef MAPPER004_H
#define MAPPER004_H

#include "../mapper.h"

#define MAPPER004_NUM_PRG_BANKS_8K 18
#define MAPPER004_NUM_CHR_BANKS_1K 26

Mapper createMapper004(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif