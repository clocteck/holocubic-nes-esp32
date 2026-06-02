#ifndef BUS_H
#define BUS_H

#include <Arduino.h>
#include <stdint.h>

#include "cartridge.h"
#include "cpu6502.h"
#include "ppu2C02.h"

namespace nes::video
{
class NesVideoOut;
}

class Bus
{
public:
    Bus();
    ~Bus();

public:
    Cpu6502 cpu;
    Ppu2C02 ppu;
    Cartridge *cart = nullptr;
    uint8_t RAM[2048];
    uint8_t controller = 0x00;

    void cpuWrite(uint16_t addr, uint8_t data);
    uint8_t cpuRead(uint16_t addr);
    void setPPUMirrorMode(Cartridge::MIRROR mirror);
    Cartridge::MIRROR getPPUMirrorMode();

    void insertCartridge(Cartridge *cartridge);
    void setVideoOut(nes::video::NesVideoOut *video_out);
    bool reset(uint32_t max_stage = 0);
    void clock();
    void IRQ();
    void NMI();
    void OAM_Write(uint8_t addr, uint8_t data);
    uint16_t ppu_scanline = 0;
    void renderImage(uint16_t scanline, uint16_t row_count);
    bool consumeRenderFailure(String *err = nullptr);

    void saveState();
    void loadState();

private:
    /**
     * @brief 借出下一块可写显示缓冲，并挂到 PPU 当前传输目标上。
     *
     * 这里把“向显示任务申请双缓冲 slot”的逻辑集中在 Bus，
     * 让 PPU 继续专注于像素生成，不直接知道 FreeRTOS 队列和显示线程细节。
     */
    bool prepareRenderBuffer();

    uint8_t controller_state = 0x00;
    uint8_t controller_strobe = 0x00;
    /**
     * @brief CPU 总线上的“最后一个可见字节”近似值。
     *
     * 说明：
     * 1. 过去未映射 CPU 读一律回 `0x00`，这会把不少 open-bus 依赖场景直接钉死；
     * 2. 这里先做一个轻量近似：每次 CPU 读/写都会刷新该值，未映射读时直接回它；
     * 3. 对 mapper 226 这类盗版 multicart 菜单来说，这比固定 `0x00` 更接近真机。
     */
    uint8_t m_cpu_open_bus = 0x00;
    uint16_t m_mapper226_trace_reads_left = 0;
    bool frame_latch = false;
    volatile bool m_render_failed = false;
    String m_render_error;
    nes::video::NesVideoOut *m_video_out = nullptr;
};

#endif
