#ifndef MAPPER003_H
#define MAPPER003_H

#include "../mapper.h"

#define MAPPER003_NUM_CHR_BANKS_8K 16
struct Mapper003_state
{
    Cartridge* cart;
    uint8_t number_PRG_banks;
    uint8_t number_CHR_banks;

    uint8_t* ptr_CHR_bank_8K;
    Bank CHR_banks_8K[MAPPER003_NUM_CHR_BANKS_8K];
    BankCache CHR_cache_8K;
    uint8_t* PRG_bank;
};

Mapper createMapper003(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif
