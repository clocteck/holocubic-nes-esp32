#pragma once

#include <stdint.h>

#include "nes_config.h"

namespace nes
{

struct VideoSpec
{
    uint16_t width = config::kDefaultFrameWidth;
    uint16_t height = config::kDefaultFrameHeight;
    bool center_on_screen = true;
    uint16_t transfer_rows = config::kDefaultTransferRows;
};

} // namespace nes
