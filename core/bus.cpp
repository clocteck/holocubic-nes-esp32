#include "bus.h"

#include <stdio.h>
#include <string.h>

#include "../nes_config.h"
#include "video/nes_video_out.h"
#include "nes_port.h"
#include "power_on_ram.h"

Bus::Bus()
{
    nes::power_on::fillVolatileRam(RAM, sizeof(RAM), 0x43505552u);
    m_cpu_open_bus = 0x00;
    m_mapper226_trace_reads_left = NES_MAPPER226_TRACE_MAX_UNMAPPED_READS;
    cpu.connectBus(this);
    cpu.apu.connectBus(this);
    ppu.connectBus(this);
}

Bus::~Bus()
{
}

IRAM_ATTR void Bus::cpuWrite(uint16_t addr, uint8_t data)
{
    if (cart && cart->cpuWrite(addr, data)) {}
    else if ((addr & 0xE000) == 0x0000)
    {
        RAM[addr & 0x07FF] = data;
    }
    else if ((addr & 0xE000) == 0x2000)
    {
        ppu.cpuWrite(addr & 0x0007, data);
    }
    else if ((addr & 0xF000) == 0x4000 && (addr <= 0x4013 || addr == 0x4015 || addr == 0x4017))
    {
        cpu.apuWrite(addr, data);
    }
    else if (addr == 0x4014)
    {
        cpu.OAM_DMA(data);
    }
    else if (addr == 0x4016)
    {
        controller_strobe = data & 1;
        if (controller_strobe)
        {
            controller_state = controller;
        }
    }

    /**
     * 这里把 CPU 最近一次放到数据总线上的值记下来，后面未映射读时
     * 直接回它，作为 open bus 的一阶近似。
     */
    m_cpu_open_bus = data;
}

IRAM_ATTR uint8_t Bus::cpuRead(uint16_t addr)
{
    uint8_t data = m_cpu_open_bus;
    bool handled = false;

    if (cart && cart->cpuRead(addr, data))
    {
        handled = true;
    }
    else if ((addr & 0xE000) == 0x0000)
    {
        data = RAM[addr & 0x07FF];
        handled = true;
    }
    else if ((addr & 0xE000) == 0x2000)
    {
        data = ppu.cpuRead(addr & 0x0007);
        handled = true;
    }
    else if (addr == 0x4015)
    {
        data = cpu.apu.cpuRead(addr);
        handled = true;
    }
    else if (addr == 0x4016)
    {
        uint8_t value = controller_state & 1;
        if (!controller_strobe)
            controller_state >>= 1;
        data = value | 0x40;
        handled = true;
    }

#if NES_MAPPER226_TRACE_UNMAPPED_READS
    if (!handled &&
        cart &&
        cart->mapperId() == 226 &&
        addr >= 0x6000 &&
        addr <= 0x7FFF &&
        m_mapper226_trace_reads_left > 0)
    {
        LOGF("[nes][m226] read open addr=%04X data=%02X left=%u\n",
             addr,
             data,
             (unsigned)(m_mapper226_trace_reads_left - 1U));
        m_mapper226_trace_reads_left--;
    }
#endif

    m_cpu_open_bus = data;
    return data;
}

bool Bus::reset(uint32_t max_stage)
{
    nes_port_log("[nes.so] bus reset enter");
    m_render_failed = false;
    if (m_render_error.length() > 0)
    {
        m_render_error.remove(0);
    }
    m_mapper226_trace_reads_left = NES_MAPPER226_TRACE_MAX_UNMAPPED_READS;
    nes_port_log("[nes.so] bus reset clear done");

    /**
     * 这里明确把 `reset()` 视作 soft reset：
     *
     * 1. mapper 寄存器、CPU/PPU 内部状态需要复位；
     * 2. 但 2 KiB CPU RAM 不应该顺手清掉；
     * 3. 上电 RAM 的随机/固定填充只发生在 `Bus` 构造这一层。
     *
     * 这样 mapper 226 这类依赖“power-on RAM 未定义、soft reset 保留 RAM”的
     * multicart 菜单，行为才更接近真机。
     */
    if (cart)
    {
        nes_port_log("[nes.so] bus reset cart begin");
        cart->reset();
        nes_port_log("[nes.so] bus reset cart done");
    }
    if (max_stage == 1)
    {
        return true;
    }

    nes_port_log("[nes.so] bus reset cpu begin");
    cpu.reset();
    nes_port_log("[nes.so] bus reset cpu done");
    if (max_stage == 2)
    {
        return true;
    }

    nes_port_log("[nes.so] bus reset apu begin");
    cpu.apu.reset();
    nes_port_log("[nes.so] bus reset apu done");
    if (max_stage == 3)
    {
        return true;
    }

    nes_port_log("[nes.so] bus reset ppu begin");
    ppu.reset();
    nes_port_log("[nes.so] bus reset ppu done");
    if (max_stage == 4)
    {
        return true;
    }

    nes_port_log("[nes.so] bus reset render begin");
    const bool ok = prepareRenderBuffer();
    nes_port_log("[nes.so] bus reset render done");
    return ok;
}

IRAM_ATTR void Bus::clock()
{
    // 1 frame == 341 dots * 261 scanlines
    // Visible scanlines 0-239
    
    // Rendering 3 scanlines at a time because 1 CPU clock == 3 PPU clocks
    // and there's only 341 ppu clocks (dots) in a scanline, which is not divisible by 3.
    // Using a counter/for loop with += 341 & -= 3 is too big of a performance hit.
    // 1 scanline == ~113.67 CPU clocks, so for every 3 scanlines, two scanlines will have an extra CPU clock
    if (!frame_latch)
    {
        for (ppu_scanline = 0; ppu_scanline < 240; ppu_scanline += 3)
        {
            cpu.clock(113);
            ppu.renderScanline(ppu_scanline);

            cpu.clock(114);
            ppu.renderScanline(ppu_scanline + 1);

            cpu.clock(114);
            ppu.renderScanline(ppu_scanline + 2);
        }
    }
    else
    {
        for (ppu_scanline = 0; ppu_scanline < 240; ppu_scanline += 3)
        {
            cpu.clock(113);
            ppu.fakeSpriteHit(ppu_scanline);

            cpu.clock(114);
            ppu.fakeSpriteHit(ppu_scanline + 1);

            cpu.clock(114);
            ppu.fakeSpriteHit(ppu_scanline + 2);
        }
    }

    // Setup for the next frame
    // Same reason as scanlines 0-239, 2/3 of scanlines will have an extra CPU clock. 
    // Scanline 240
    cpu.clock(113);

    // Scanline 241-261
    ppu.setVBlank();
    cpu.clock(2501);

    ppu.clearVBlank();
    cpu.clock(114);

#ifdef FRAMESKIP
    frame_latch = !frame_latch;
#endif
}

IRAM_ATTR void Bus::setPPUMirrorMode(Cartridge::MIRROR mirror)
{
    ppu.setMirror(mirror);
}

Cartridge::MIRROR Bus::getPPUMirrorMode()
{
    return ppu.getMirror();
}

IRAM_ATTR void Bus::OAM_Write(uint8_t addr, uint8_t data)
{
    ppu.ptr_sprite[addr] = data;
}

void Bus::insertCartridge(Cartridge* cartridge)
{
    cart = cartridge;
    cpu.connectCartridge(cartridge);
    ppu.connectCartridge(cartridge);
    if (cart)
    {
        cart->connectBus(this);
    }
}

void Bus::setVideoOut(nes::video::NesVideoOut *video_out)
{
    m_video_out = video_out;
}

bool Bus::prepareRenderBuffer()
{
    if (!m_video_out || !m_video_out->active())
    {
        ppu.setDisplayTarget(nullptr, 0);
        m_render_failed = true;
        m_render_error = "nes video output inactive";
        return false;
    }

    nes::video::NesVideoOut::TransferChunk chunk = {};
    String err;
    if (!m_video_out->acquireTransferChunk(&chunk, &err) ||
        !chunk.pixels ||
        chunk.capacity_rows == 0)
    {
        ppu.setDisplayTarget(nullptr, 0);
        m_render_failed = true;
        m_render_error = err.length() > 0 ? err : String("acquire nes render chunk failed");
        return false;
    }

    ppu.setDisplayTarget(chunk.pixels, chunk.capacity_rows);
    return true;
}

IRAM_ATTR void Bus::renderImage(uint16_t scanline, uint16_t row_count)
{
    if (m_render_failed)
    {
        return;
    }

    String err;
    if (!m_video_out || !m_video_out->submitTransferChunk(scanline, row_count, &err))
    {
        if (m_video_out)
        {
            m_video_out->cancelTransferChunk();
        }

        m_render_failed = true;
        m_render_error = err.length() > 0 ? err : String("submit nes render chunk failed");
        ppu.setDisplayTarget(nullptr, 0);
        return;
    }

    (void)prepareRenderBuffer();
}

bool Bus::consumeRenderFailure(String *err)
{
    if (m_video_out)
    {
        String async_err;
        if (m_video_out->consumeFailure(&async_err))
        {
            m_render_failed = true;
            m_render_error = async_err.length() > 0 ? async_err : String("nes async display failed");
        }
    }

    if (!m_render_failed)
    {
        return false;
    }

    if (m_render_error.length() == 0)
    {
        m_render_error = "nes display push failed";
    }

    if (err)
    {
        *err = m_render_error;
    }

    m_render_failed = false;
    return true;
}

IRAM_ATTR void Bus::IRQ()
{
    cpu.IRQ();
}

IRAM_ATTR void Bus::NMI()
{
    cpu.NMI();
}

void Bus::saveState()
{
    // Save-state persistence is intentionally outside the first dynmod core port.
}

void Bus::loadState()
{
    // Save-state persistence is intentionally outside the first dynmod core port.
}
