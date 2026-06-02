#include "ppu2C02.h"
#include "bus.h"


#define READ_PALETTE(x) palette_table[((x) & 0x1F) ^ (((x) & 0x13) == 0x10 ? 0x10 : 0x00)]
constexpr uint8_t Ppu2C02::palette_mirror[32];

Ppu2C02::Ppu2C02()
{
    memset(nametable, 0, sizeof(nametable));
    memset(palette_table, 0, sizeof(palette_table));
    memset(scanline_buffer, 0, sizeof(scanline_buffer));
    memset(scanline_metadata, 0, sizeof(scanline_metadata));
    memset(sprite, 0, sizeof(sprite));
    resetDisplayTransfer();
}

Ppu2C02::~Ppu2C02()
{

}

inline void Ppu2C02::ppuWrite(uint16_t addr, uint8_t data)
{
    addr &= 0x3FFF;
    
    if (cart->ppuWrite(addr, data)) return;
    else if (addr >= 0x2000 && addr <= 0x3EFF)
    {
        ptr_nametable[(addr >> 10) & 3][addr & 0x03FF] = data;
    }
    else if (addr >= 0x3F00 && addr <= 0x3FFF)
    {
        addr = palette_mirror[addr & 0x001F];
        palette_table[addr] = data;
    }
}

inline uint8_t Ppu2C02::ppuRead(uint16_t addr)
{
    uint8_t data = 0x00;
    addr &= 0x3FFF;
    
    if (cart->ppuRead(addr, data)) return data;
    else if (addr >= 0x2000 && addr <= 0x3EFF)
    {
        data = ptr_nametable[(addr >> 10) & 3][addr & 0x03FF];
    }
    else if (addr >= 0x3F00 && addr <= 0x3FFF)
    {
        addr &= 0x001F;
        switch (addr)
        {
        case 0x0010: addr = 0x0000; break;
        case 0x0014: addr = 0x0004; break;
        case 0x0018: addr = 0x0008; break;
        case 0x001C: addr = 0x000C; break;
        }
        data = palette_table[addr] & (mask.grayscale ? 0x30 : 0x3F);
    }

    return data;
}

IRAM_ATTR void Ppu2C02::cpuWrite(uint16_t addr, uint8_t data)
{
    switch (addr)
    {
    case 0x0000: // PPUCTRL
        control.reg = data;
        t.nametable_x = control.nametable_x;
        t.nametable_y = control.nametable_y;
        break;
    case 0x0001: // PPUMASK
        mask.reg = data;
        break;
    case 0x0003: // OAMADDR
        OAMADDR = data;
        break;
    case 0x0004: // OAMDATA
        ptr_sprite[OAMADDR++] = data;
        break;
    case 0x0005: // PPUSCROLL
        if (w == 0)
        {
            x = data & 0x07;
            t.coarse_x = data >> 3;
        }
        else
        {
            t.fine_y = data & 0x07;
            t.coarse_y = data >> 3;
        }
        w = ~w;
        break;
    case 0x0006: // PPUADDR
        if (w == 0)
        {
            t.reg = (t.reg & 0x00FF) | (uint16_t)((data & 0x3F) << 8);
        }
        else
        {
            t.reg = (t.reg & 0xFF00) | data;
            v.reg = t.reg;
        }
        w = ~w;
        break;
    case 0x0007: // PPUDATA
        ppuWrite(v.reg, data);
        v.reg += (control.VRAM_addr_increment ? 32 : 1);
        break;
    }
}

IRAM_ATTR uint8_t Ppu2C02::cpuRead(uint16_t addr)
{
    uint8_t data = 0x00;
    switch (addr)
    {
    case 0x0002: // PPUSTATUS
        data = status.reg & 0xE0;
        status.VBlank = 0;
        w = 0;
        break;
    case 0x0004: // OAMDATA
        data = ptr_sprite[OAMADDR];
        break;
    case 0x0007: // PPUDATA
        data = PPUDATA_buffer;
        PPUDATA_buffer = ppuRead(v.reg);

        if ((v.reg & 0x3F00) == 0x3F00) data = PPUDATA_buffer;
        v.reg += (control.VRAM_addr_increment ? 32 : 1);
        break;
    }

    return data;
}

IRAM_ATTR void Ppu2C02::setVBlank()
{
    status.VBlank = 1;
    if (control.Vblank_NMI) bus->NMI();
}

IRAM_ATTR void Ppu2C02::clearVBlank()
{
    status.VBlank = 0;
    status.sprite_zero_hit = 0;
    status.sprite_overflow = 0;
}

IRAM_ATTR void Ppu2C02::renderScanline(uint16_t current_scanline)
{
    scanline = current_scanline;
    transferScroll();
    renderBackground();
    renderSprites();
    incrementY();
    finishScanline();
}

inline void Ppu2C02::transferScroll()
{
    if (!(mask.reg & (1 << 3) || mask.reg & (1 << 4))) return;
    v.reg = (scanline == 0) ? t.reg : v.reg = (v.reg & ~0x041F) | (t.reg & 0x041F);
}

inline void Ppu2C02::incrementY()
{
    if (!(mask.render_background || mask.render_sprite)) return;

    if (v.fine_y < 7)
    {
        v.fine_y++;
        return;
    }
    
    v.fine_y = 0;
    if (v.coarse_y == 29)
    {
        v.coarse_y = 0;
        v.nametable_y = ~v.nametable_y;
    }
    else if (v.coarse_y == 31) v.coarse_y = 0;
    else v.coarse_y++;
}

inline void Ppu2C02::renderBackground()
{   
    // Show transparency pixel if not rendering background
    if (!mask.render_background)
    {
        uint8_t bg_color = palette_table[0];
        memset(scanline_buffer, bg_color, BUFFER_SIZE);
        memset(scanline_metadata, 0x80, BUFFER_SIZE);
        ptr_buffer = scanline_buffer + x;
        ptr_scanline_meta = scanline_metadata + x;
        return;
    }

    uint8_t x_tile, y_tile, tile_index, nametable_index, bg_color;
    uint8_t attribute_byte, attribute_shift, attribute;
    uint16_t offset, nametable_byte_base, attribute_byte_base;
    uint8_t* ptr_pattern_tile;
    uint8_t* ptr_attribute;
    uint8_t* ptr_tile;

    bg_color = palette_table[0];
    ptr_buffer = scanline_buffer;
    ptr_scanline_meta = scanline_metadata;
    x_tile = v.coarse_x;
    y_tile = v.coarse_y;
    offset = (control.background_table_addr ? 0x1000 : 0x0000) + v.fine_y;
    nametable_index = (v.reg >> 10) & 3;

    nametable_byte_base = v.reg & 0x03E0;
    ptr_tile = &ptr_nametable[nametable_index][nametable_byte_base + x_tile];

    attribute_byte_base = 0x03C0 + ((y_tile & 0x1C) << 1);
    ptr_attribute = &ptr_nametable[nametable_index][attribute_byte_base + (x_tile >> 2)];
    attribute_byte = *ptr_attribute++;
    attribute_shift = ((y_tile & 2) << 1) + (x_tile & 2);
    attribute = ((attribute_byte >> attribute_shift) & 3) << 2;

    static constexpr DRAM_ATTR uint8_t pixel_shift[8] = { 14, 6, 12, 4, 10, 2, 8, 0 }; // Shifts to get the bits of a pixel
    static constexpr DRAM_ATTR uint8_t pixel_metadata[4] = { 0x80, 0x00, 0x00, 0x00 };
    for (int tile = 0; tile < 33; tile++)
    {
        tile_index = *ptr_tile++;
        ptr_pattern_tile = cart->ppuReadPtr(offset + (tile_index << 4)); 

        // draw to framebuffer
        uint16_t pattern = ((ptr_pattern_tile[8] & 0xAA) << 8) | ((ptr_pattern_tile[8] & 0x55) << 1)
                    | ((ptr_pattern_tile[0] & 0xAA) << 7) | (ptr_pattern_tile[0] & 0x55);
        uint8_t tile_palette[4];
        tile_palette[0] = bg_color;
        for (int t = 1; t < 4; t++) tile_palette[t] = READ_PALETTE(attribute + t);
        for (int i = 0; i < 8; i++)
        {   
            uint8_t pixel = (pattern >> pixel_shift[i]) & 3;
            *ptr_buffer++ = tile_palette[pixel];
            *ptr_scanline_meta++ = pixel_metadata[pixel]; // Store if pixel is transparent for sprite rendering
        }

        x_tile++;
        if ((x_tile & 1) == 0)
        {
            if ((x_tile & 3) == 0)
            {
                if (x_tile == 32)
                {
                    // switch nametable
                    x_tile = 0;
                    nametable_index ^= 1;

                    // recalculate pointer to tile and attribute data
                    ptr_tile = &ptr_nametable[nametable_index][nametable_byte_base];
                    ptr_attribute = &ptr_nametable[nametable_index][attribute_byte_base];
                }

                attribute_byte = *ptr_attribute++;
            }

            attribute_shift ^= 2;
            attribute = ((attribute_byte >> attribute_shift) & 0x03) << 2;
        }
    }
    ptr_buffer = scanline_buffer + x;
    ptr_scanline_meta = scanline_metadata + x;
    if (!mask.render_background_left)
    {
        for (int i = 0; i < 8; i++)
        {
            ptr_buffer[i] = bg_color;
            ptr_scanline_meta[i] = 0x80;
        }
    }
}

inline void Ppu2C02::renderSprites()
{
    if (!mask.render_sprite) 
    {
        return;
    }

    OAM* ptr_sprite_OAM;
    uint8_t* buffer_offset;
    uint8_t* metadata_offset;
    uint8_t* ptr_tile;
    uint8_t sprite_x, sprite_y;
    uint8_t sprite_size;
    uint8_t sprite_count = 0;
    uint8_t tile_index, bg_color, attribute_byte, attribute, palette_offset;
    uint8_t pixel[8];
    uint8_t tile_palette[4];
    uint16_t offset, tile_addr, pattern;
    int16_t y_offset;

    bg_color = palette_table[0];
    tile_palette[0] = bg_color;

    ptr_sprite_OAM = sprite;
    offset = (control.sprite_table_addr ? 0x1000 : 0);

    sprite_size = (control.sprite_size ? 16 : 8);

    buffer_offset = scanline_buffer + x;
    metadata_offset = scanline_metadata + x;
    for (int i = 0; i < 64; i++, ptr_sprite_OAM++)
    {
        sprite_y = ptr_sprite_OAM->y + 1;
        // Check if sprite is in scanline
        if ((sprite_y > scanline) || (sprite_y <= (scanline - sprite_size)) 
            || (sprite_y == 0) || (sprite_y >= 240))
            continue;

        sprite_x = ptr_sprite_OAM->x;
        tile_index = ptr_sprite_OAM->index;
        attribute_byte = ptr_sprite_OAM->attribute;
        attribute = ((attribute_byte & 0x03) << 2);
    
        ptr_buffer = buffer_offset + sprite_x;
        ptr_scanline_meta = metadata_offset + sprite_x;

        // If 8x16 sprite mode
        tile_addr = (control.sprite_size) ? ((tile_index & 0x01) << 12) | ((tile_index & 0xFE) << 4) : offset + (tile_index << 4);
        ptr_tile = cart->ppuReadPtr(tile_addr);

        y_offset = scanline - sprite_y;
        if (y_offset > 7) y_offset += 8;
        if (attribute_byte & 0x80) // If flip sprite vertically
        {
            y_offset -= (control.sprite_size) ? 23 : 7;
            ptr_tile -= y_offset;
        }
        else ptr_tile += y_offset;

        // Draw to buffer
        pattern = ((ptr_tile[8] & 0xAA) << 8) | ((ptr_tile[8] & 0x55) << 1)
                    | ((ptr_tile[0] & 0xAA) << 7) | (ptr_tile[0] & 0x55);
        if (pattern)
        {
            palette_offset = 16 + attribute;
            for (int t = 1; t < 4; t++) tile_palette[t] = READ_PALETTE(palette_offset + t);

            if (attribute_byte & 0x40) // If flip sprite horizontally
            {
                pixel[7] = (pattern >> 14) & 3;
                pixel[6] = (pattern >> 6) & 3;
                pixel[5] = (pattern >> 12) & 3;
                pixel[4] = (pattern >> 4) & 3;
                pixel[3] = (pattern >> 10) & 3;
                pixel[2] = (pattern >> 2) & 3;
                pixel[1] = (pattern >> 8) & 3;
                pixel[0] = pattern & 3;
            }
            else
            {
                pixel[0] = (pattern >> 14) & 3;
                pixel[1] = (pattern >> 6) & 3;
                pixel[2] = (pattern >> 12) & 3;
                pixel[3] = (pattern >> 4) & 3;
                pixel[4] = (pattern >> 10) & 3;
                pixel[5] = (pattern >> 2) & 3;
                pixel[6] = (pattern >> 8) & 3;
                pixel[7] = pattern & 3;
            }

            // Check for sprite 0 hit
            if (i == 0 && status.sprite_zero_hit == 0)
            {
                for (int j = 0; j < 8; j++)
                {
                    const uint16_t screen_x = (uint16_t)sprite_x + j;
                    if (screen_x >= (SCANLINE_SIZE - 1) ||
                        (screen_x < 8 && (!mask.render_background_left || !mask.render_sprite_left)))
                    {
                        continue;
                    }

                    if (pixel[j] && ((ptr_scanline_meta[j] & 0x80) == 0))
                    {
                        status.sprite_zero_hit = true;
                        break;
                    }
                }
            }

            // Render sprite pixels on scanline buffer
            if (attribute_byte & 0x20) // Sprite Priorty : 1 - behind background | 0 - in front of background
            {
                for (int j = 0; j < 8; j++)
                {
                    const uint16_t screen_x = (uint16_t)sprite_x + j;
                    if (screen_x >= SCANLINE_SIZE || (screen_x < 8 && !mask.render_sprite_left))
                    {
                        continue;
                    }

                    if (pixel[j])
                    {
                        if (ptr_scanline_meta[j] & 0x80) ptr_buffer[j] = tile_palette[pixel[j]];
                        ptr_scanline_meta[j] |= 0x40;
                    }
                }
            }
            else
            {
                for (int j = 0; j < 8; j++)
                {
                    const uint16_t screen_x = (uint16_t)sprite_x + j;
                    if (screen_x >= SCANLINE_SIZE || (screen_x < 8 && !mask.render_sprite_left))
                    {
                        continue;
                    }

                    if (pixel[j] && ((ptr_scanline_meta[j] & 0x40) == 0))
                    {
                        ptr_buffer[j] = tile_palette[pixel[j]];;
                        ptr_scanline_meta[j] |= 0x40;
                    }
                }
            }
        }

        // If sprite overflow, break
        if (++sprite_count == 8) { status.sprite_overflow = true; break; }
    }

    ptr_buffer = buffer_offset;
}

inline bool Ppu2C02::spriteIntersectsScanline(const OAM* sprite_entry,
                                              uint8_t sprite_size,
                                              uint16_t current_scanline) const
{
    if (!sprite_entry)
    {
        return false;
    }

    const uint8_t sprite_y = sprite_entry->y + 1;
    return !((sprite_y > current_scanline) ||
             (sprite_y <= (current_scanline - sprite_size)) ||
             (sprite_y == 0) ||
             (sprite_y >= 240));
}

inline uint16_t Ppu2C02::spritePatternAddress(const OAM* sprite_entry,
                                              uint8_t sprite_size,
                                              uint16_t sprite_pattern_base) const
{
    if (!sprite_entry)
    {
        return 0x0000;
    }

    const uint8_t sprite_index = sprite_entry->index;
    if (sprite_size == 16)
    {
        return ((sprite_index & 0x01) << 12) | ((sprite_index & 0xFE) << 4);
    }

    return sprite_pattern_base + ((uint16_t)sprite_index << 4);
}

inline void Ppu2C02::emitMapperIrqA12Pulse()
{
    if (!cart || !(mask.render_background || mask.render_sprite))
    {
        return;
    }

    /**
     * 这里不把 PPU 重写成 dot-accurate，而是只补一条“足够轻量”的 A12 脉冲近似：
     *
     * 1. 先显式喂一次 A12=0 的地址，表示上一段 nametable/attribute 低电平窗口；
     * 2. 再按当前 BG/Sprite pattern table 选择，补一条本扫描线最关键的高电平取样；
     * 3. mapper004 内部再按 A12 0->1 上升沿去 clock IRQ counter。
     *
     * 这样做比“每扫描线直接减一次计数器”更接近 MMC3 的真实行为，
     * 同时不会把当前这套按扫描线渲染的 PPU 结构拖进高成本的逐点模拟。
     */
    cart->notifyPpuAddress(0x2000 | (v.reg & 0x0FFF));

    uint16_t pulse_addr = 0x0000;
    bool have_high_phase = false;
    const bool background_fetches_high = mask.render_background && control.background_table_addr;

    if (background_fetches_high)
    {
        pulse_addr = 0x1000;
        have_high_phase = true;
    }

    if (mask.render_sprite)
    {
        if (!control.sprite_size)
        {
            if (control.sprite_table_addr && !have_high_phase)
            {
                pulse_addr = 0x1FF0;
                have_high_phase = true;
            }
        }
        else
        {
            const uint8_t sprite_size = 16;
            uint8_t visible_sprite_count = 0;
            uint16_t sprite_high_addr = 0x0000;
            bool sprite_fetches_high = false;

            for (int i = 0; i < 64; i++)
            {
                if (!spriteIntersectsScanline(&sprite[i], sprite_size, scanline))
                {
                    continue;
                }

                visible_sprite_count++;
                if (!sprite_fetches_high)
                {
                    sprite_high_addr = spritePatternAddress(&sprite[i], sprite_size, 0);
                    sprite_fetches_high = (sprite_high_addr & 0x1000) != 0;
                }

                if (visible_sprite_count == 8)
                {
                    break;
                }
            }

            /**
             * 8x16 模式下，若本行实际命中的精灵不足 8 个，后续 dummy sprite fetch
             * 往往仍会带出 A12 高相位。这里保留一个轻量近似，避免少精灵行完全丢 IRQ。
             */
            if (!sprite_fetches_high && visible_sprite_count < 8)
            {
                sprite_high_addr = 0x1FE0;
                sprite_fetches_high = true;
            }

            if (sprite_fetches_high && !have_high_phase)
            {
                pulse_addr = sprite_high_addr;
                have_high_phase = true;
            }
        }
    }

    if (!have_high_phase)
    {
        return;
    }

    cart->notifyPpuAddress(pulse_addr);
}

void Ppu2C02::fakeSpriteHit(uint16_t current_scanline)
{
    scanline = current_scanline;
    emitMapperIrqA12Pulse();
    if (mask.render_background || mask.render_sprite) cart->ppuScanline();
    if (!mask.render_sprite || status.sprite_zero_hit) return;

    uint8_t sprite_size;
    uint16_t offset = (control.sprite_table_addr ? 0x1000 : 0);
    sprite_size = (control.sprite_size ? 16 : 8);

    uint8_t sprite_x, sprite_y;
    sprite_y = sprite[0].y + 1;
    // Check if sprite is in scanline
    if ((sprite_y > scanline) || (sprite_y <= (scanline - sprite_size)) 
        || (sprite_y == 0) || (sprite_y >= 240))
        return;

    int16_t y_offset;
    uint16_t tile_addr;

    sprite_x = sprite[0].x;
    uint8_t tile_index = sprite[0].index;
    uint8_t attribute_byte = sprite[0].attribute;

    tile_addr = (control.sprite_size) ? ((tile_index & 0x01) << 12) | ((tile_index & 0xFE) << 4) : offset + (tile_index << 4);
    uint8_t* ptr_tile = cart->ppuReadPtr(tile_addr);

    y_offset = scanline - sprite_y;
    if (y_offset > 7) y_offset += 8;

    if (attribute_byte & 0x80) // If flip sprite vertically
    {
        y_offset -= (control.sprite_size) ? 23 : 7;
        ptr_tile -= y_offset;
    }
    else ptr_tile += y_offset;

    // Draw to buffer
    uint16_t pattern = ((ptr_tile[8] & 0xAA) << 8) | ((ptr_tile[8] & 0x55) << 1)
                    | ((ptr_tile[0] & 0xAA) << 7) | (ptr_tile[0] & 0x55);
    if (pattern)
    {
        status.sprite_zero_hit = true;
        return;
        // uint8_t pixel[8];
        // if (attribute_byte & 0x40) // If flip sprite horizontally
        // {
        //     pixel[7] = (pattern >> 14) & 3;
        //     pixel[6] = (pattern >> 6) & 3;
        //     pixel[5] = (pattern >> 12) & 3;
        //     pixel[4] = (pattern >> 4) & 3;
        //     pixel[3] = (pattern >> 10) & 3;
        //     pixel[2] = (pattern >> 2) & 3;
        //     pixel[1] = (pattern >> 8) & 3;
        //     pixel[0] = pattern & 3;
        // }
        // else
        // {
        //     pixel[0] = (pattern >> 14) & 3;
        //     pixel[1] = (pattern >> 6) & 3;
        //     pixel[2] = (pattern >> 12) & 3;
        //     pixel[3] = (pattern >> 4) & 3;
        //     pixel[4] = (pattern >> 10) & 3;
        //     pixel[5] = (pattern >> 2) & 3;
        //     pixel[6] = (pattern >> 8) & 3;
        //     pixel[7] = pattern & 3;
        // }

        // for (int i = 0; i < 8; i++)
        // {
        //     if (pixel[i]) 
        //     {
        //         status.sprite_zero_hit = true;
        //         return;
        //     }
        // }
    }
}

inline void Ppu2C02::finishScanline()
{
    emitMapperIrqA12Pulse();
    if (mask.render_background || mask.render_sprite) 
        cart->ppuScanline();
    if (!ptr_display || display_rows == 0)
    {
        scanline_counter = 0;
        return;
    }

    uint32_t* display = (uint32_t*)(ptr_display + (scanline_counter * SCANLINE_SIZE));
    uint8_t* buffer = ptr_buffer;
    const uint16_t* palette = nes_palette[mask.emphasize];
    for (int i = 0, size = (SCANLINE_SIZE >> 1); i < size; i++)
    {
        const uint32_t color0 = palette[*buffer++];
        const uint32_t color1 = palette[*buffer++];
        display[i] = color0 | (color1 << 16);
    }
    scanline_counter++;

    /**
     * `display_rows` 由外层 video_out 根据当前可借到的共享缓冲能力决定，
     * 因此这里不再使用编译期固定的 `SCANLINES_PER_BUFFER`。
     *
     * 同时在帧尾也要把不足一个完整块的剩余行立即提交出去，
     * 否则当 `transfer_rows` 不是 240 的整除数时，最后几行会一直滞留在缓冲里。
     */
    if (scanline_counter >= display_rows || scanline >= 239)
    {
        bus->renderImage((uint16_t)(scanline - (scanline_counter - 1)), scanline_counter);
        scanline_counter = 0;
    }
}

void Ppu2C02::reset()
{
    status.reg = 0x00;
	mask.reg = 0x00;
	control.reg = 0x00;
	t.reg = 0x00;
	v.reg = 0x00;
	x = 0x00;
    w = 0x00;
    OAMADDR = 0x00;
    OAMDATA = 0x00;
	PPUDATA_buffer = 0x00;
    resetDisplayTransfer();
}

void Ppu2C02::connectCartridge(Cartridge* cartridge)
{
    cart = cartridge;
    setMirror((Cartridge::MIRROR)cart->hardware_mirror);
}

void Ppu2C02::setMirror(Cartridge::MIRROR mirror)
{
    switch (mirror)
    {
        case Cartridge::MIRROR::VERTICAL:
            ptr_nametable[0] = &nametable[0x0000];
            ptr_nametable[1] = &nametable[0x0400];
            ptr_nametable[2] = &nametable[0x0000];
            ptr_nametable[3] = &nametable[0x0400];
            break;

        case Cartridge::MIRROR::HORIZONTAL:
            ptr_nametable[0] = &nametable[0x0000];
            ptr_nametable[1] = &nametable[0x0000];
            ptr_nametable[2] = &nametable[0x0400];
            ptr_nametable[3] = &nametable[0x0400];
            break;

        case Cartridge::MIRROR::ONESCREEN_LOW:
            ptr_nametable[0] = ptr_nametable[1] = ptr_nametable[2] = ptr_nametable[3] = &nametable[0x0000];
            break;

        case Cartridge::MIRROR::ONESCREEN_HIGH:
            ptr_nametable[0] = ptr_nametable[1] = ptr_nametable[2] = ptr_nametable[3] = &nametable[0x0400];
            break;

        case Cartridge::MIRROR::FOURSCREEN:
            ptr_nametable[0] = &nametable[0x0000];
            ptr_nametable[1] = &nametable[0x0400];
            ptr_nametable[2] = &nametable[0x0800];
            ptr_nametable[3] = &nametable[0x0C00];
            break;

        case Cartridge::MIRROR::HARDWARE:
            break;
    }
}

Cartridge::MIRROR Ppu2C02::getMirror()
{
    if (ptr_nametable[0] == &nametable[0x0000] && 
        ptr_nametable[1] == &nametable[0x0400] &&
        ptr_nametable[2] == &nametable[0x0000] &&
        ptr_nametable[3] == &nametable[0x0400])
        return Cartridge::MIRROR::VERTICAL;

    if (ptr_nametable[0] == &nametable[0x0000] && 
        ptr_nametable[1] == &nametable[0x0000] &&
        ptr_nametable[2] == &nametable[0x0400] &&
        ptr_nametable[3] == &nametable[0x0400])
        return Cartridge::MIRROR::HORIZONTAL;

    if (ptr_nametable[0] == &nametable[0x0000] && 
        ptr_nametable[1] == &nametable[0x0000] &&
        ptr_nametable[2] == &nametable[0x0000] &&
        ptr_nametable[3] == &nametable[0x0000])
        return Cartridge::MIRROR::ONESCREEN_LOW;

    if (ptr_nametable[0] == &nametable[0x0400] && 
        ptr_nametable[1] == &nametable[0x0400] &&
        ptr_nametable[2] == &nametable[0x0400] &&
        ptr_nametable[3] == &nametable[0x0400])
        return Cartridge::MIRROR::ONESCREEN_HIGH;

    if (ptr_nametable[0] == &nametable[0x0000] &&
        ptr_nametable[1] == &nametable[0x0400] &&
        ptr_nametable[2] == &nametable[0x0800] &&
        ptr_nametable[3] == &nametable[0x0C00])
        return Cartridge::MIRROR::FOURSCREEN;

    return Cartridge::MIRROR::HORIZONTAL;
}

void Ppu2C02::setPalette(uint8_t palette)
{
    switch (palette)
    {
    case NTSC565:
        nes_palette = palette_NTSC565;
        break;

    case PAL565:
        nes_palette = palette_PAL565;
        break;

    case NTSC222:
        nes_palette = palette_NTSC222;
        break;
    
    case PAL222:
        nes_palette = palette_PAL222;
        break;
    }
}

void Ppu2C02::setDisplayTarget(uint16_t *display, uint16_t rows)
{
    ptr_display = display;
    display_rows = rows;
}

void Ppu2C02::resetDisplayTransfer()
{
    ptr_display = nullptr;
    display_rows = 0;
    scanline_counter = 0;
}

void Ppu2C02::dumpState(File& state)
{
    state.write((uint8_t*)scanline_buffer, sizeof(scanline_buffer));
    state.write((uint8_t*)scanline_metadata, sizeof(scanline_metadata));
    state.write(nametable, sizeof(nametable));
    for (int i = 0; i < 4; i++)
    {
        uint8_t map = (uint8_t)((ptr_nametable[i] - nametable) >> 10);
        state.write((uint8_t*)&map, sizeof(map));
    }
    state.write(palette_table, sizeof(palette_table));
    state.write((uint8_t*)&scanline_counter, sizeof(scanline_counter));

    state.write((uint8_t*)&control.reg, sizeof(control.reg));
    state.write((uint8_t*)&mask.reg, sizeof(mask.reg));
    state.write((uint8_t*)&status.reg, sizeof(status.reg));
    state.write((uint8_t*)&OAMADDR, sizeof(OAMADDR));
    state.write((uint8_t*)&OAMDATA, sizeof(OAMDATA));

    state.write((uint8_t*)sprite, sizeof(sprite));
    state.write((uint8_t*)&v.reg, sizeof(v.reg));
    state.write((uint8_t*)&t.reg, sizeof(t.reg));
    state.write((uint8_t*)&x, sizeof(x));
    state.write((uint8_t*)&w, sizeof(w));
    state.write((uint8_t*)&PPUDATA_buffer, sizeof(PPUDATA_buffer));
}

void Ppu2C02::loadState(File& state)
{
    state.read((uint8_t*)scanline_buffer, sizeof(scanline_buffer));
    state.read((uint8_t*)scanline_metadata, sizeof(scanline_metadata));
    state.read(nametable, sizeof(nametable));
    for (int i = 0; i < 4; i++)
    {
        uint8_t map = 0;
        state.read(&map, sizeof(map));
        ptr_nametable[i] = &nametable[(map & 0x03) << 10];
    }
    state.read(palette_table, sizeof(palette_table));
    state.read((uint8_t*)&scanline_counter, sizeof(scanline_counter));

    state.read((uint8_t*)&control.reg, sizeof(control.reg));
    state.read((uint8_t*)&mask.reg, sizeof(mask.reg));
    state.read((uint8_t*)&status.reg, sizeof(status.reg));
    state.read((uint8_t*)&OAMADDR, sizeof(OAMADDR));
    state.read((uint8_t*)&OAMDATA, sizeof(OAMDATA));

    state.read((uint8_t*)sprite, sizeof(sprite));
    state.read((uint8_t*)&v.reg, sizeof(v.reg));
    state.read((uint8_t*)&t.reg, sizeof(t.reg));
    state.read((uint8_t*)&x, sizeof(x));
    state.read((uint8_t*)&w, sizeof(w));
    state.read((uint8_t*)&PPUDATA_buffer, sizeof(PPUDATA_buffer));

    /**
     * 显示传输状态不属于可持久化的 PPU 逻辑状态。
     *
     * 旧实现里这部分是类内静态 `display_buffer`，loadState 后天然沿用当前内存；
     * 现在改成从 video_out 动态借缓冲后，必须明确把“本次显示会话的暂存指针”
     * 清空，等 Bus 在外层重新挂接新的共享双缓冲。
     */
    resetDisplayTransfer();
}
