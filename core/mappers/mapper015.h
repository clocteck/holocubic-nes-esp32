#ifndef MAPPER015_H
#define MAPPER015_H

#include "../mapper.h"

/**
 * mapper 15 的菜单页切换比较频繁，这里适当放大一点 8 KiB cache，
 * 可以减少多合一卡在反复进出菜单时的 ROM bank 重装次数。
 */
#define MAPPER015_NUM_PRG_BANKS_8K 16

Mapper createMapper015(uint8_t PRG_banks, uint8_t CHR_banks, Cartridge* cart);

#endif
