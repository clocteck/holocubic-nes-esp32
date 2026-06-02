#ifndef MAPPER007_H
#define MAPPER007_H

#include "../mapper.h"

#define MAPPER007_NUM_PRG_BANKS_32K 6

Mapper createMapper007(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif
