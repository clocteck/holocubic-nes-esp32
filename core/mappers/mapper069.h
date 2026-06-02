#ifndef MAPPER069_H
#define MAPPER069_H

#include "../mapper.h"

#define MAPPER069_NUM_PRG_BANKS_8K 17
#define MAPPER069_NUM_CHR_BANKS_1K 26

Mapper createMapper069(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif