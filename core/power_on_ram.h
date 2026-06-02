#ifndef NES_POWER_ON_RAM_H
#define NES_POWER_ON_RAM_H

#include <stddef.h>
#include <stdint.h>

namespace nes::power_on
{

/**
 * @brief 按当前配置把一块“上电态 RAM”填充成随机/固定图样。
 *
 * 说明：
 * 1. 这里服务的是“power-on”而不是 soft reset；
 * 2. `salt` 用来让不同 RAM 区域在 fixed-seed 模式下仍然稳定但不完全相同；
 * 3. 目前主要用于 CPU internal RAM 和 mapper 226 的固定 CHR-RAM。
 */
void fillVolatileRam(uint8_t *ram, size_t size, uint32_t salt = 0);

} // namespace nes::power_on

#endif
