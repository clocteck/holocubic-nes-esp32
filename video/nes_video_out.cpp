#include "video/nes_video_out.h"

#include <stddef.h>
#include <string.h>

namespace nes::video
{

uint16_t NesVideoOut::sanitizeTransferRows(uint16_t transfer_rows, uint16_t frame_height) const
{
    const uint16_t safe_height = frame_height == 0 ? nes::config::kDefaultFrameHeight : frame_height;
    uint16_t rows = transfer_rows;

    if (transfer_rows == 0)
    {
        rows = nes::config::kDefaultTransferRows;
    }

    if (rows > safe_height)
    {
        return safe_height;
    }

    if (rows == 0)
    {
        return 1;
    }

    return rows;
}

void NesVideoOut::init()
{
}

void NesVideoOut::setError(String *err, const char *text)
{
    if (err)
    {
        *err = text ? text : "nes display failed";
    }
}

void NesVideoOut::setFailure(const char *text)
{
    m_failed = true;
    const char *msg = text ? text : "nes display failed";
    size_t i = 0;
    while (msg[i] && i + 1 < sizeof(m_last_error))
    {
        m_last_error[i] = msg[i];
        ++i;
    }
    m_last_error[i] = '\0';
}

bool NesVideoOut::hasTftStreamApi(const module_host_api_v1 *host) const
{
    if (!host)
    {
        return false;
    }
    const size_t need = offsetof(module_display_api_t, fill_screen) + sizeof(host->display.fill_screen);
    return host->display.size >= need &&
           host->display.start_write &&
           host->display.push_image_dma &&
           host->display.end_write;
}

bool NesVideoOut::allocDmaBuffers(const module_host_api_v1 *host, String *err)
{
    if (!host || !host->heap.malloc)
    {
        setError(err, "display heap api unavailable");
        return false;
    }

    const uint16_t width = m_spec.width ? m_spec.width : 256;
    const uint16_t rows = sanitizeTransferRows(m_spec.transfer_rows, m_spec.height);
    const size_t pixels = (size_t)width * (size_t)rows;
    const size_t bytes = pixels * sizeof(uint16_t);

    for (uint8_t i = 0; i < kDmaSlotCount; ++i)
    {
        uint16_t *buf = (uint16_t *)host->heap.malloc(bytes,
                                                      MODULE_HEAP_INTERNAL |
                                                          MODULE_HEAP_DMA |
                                                          MODULE_HEAP_8BIT);
        if (!buf)
        {
            freeDmaBuffers(host);
            setError(err, "alloc display dma buffer failed");
            return false;
        }
        memset(buf, 0, bytes);

        m_slots[i].pixels = buf;
        memset(&m_slots[i].chunk, 0, sizeof(m_slots[i].chunk));
        m_slots[i].chunk.size = sizeof(m_slots[i].chunk);
        m_slots[i].chunk.pixels = buf;
        m_slots[i].chunk.rows = rows;
        m_slots[i].chunk.width = width;
        m_slots[i].chunk.pitch_bytes = (uint32_t)width * sizeof(uint16_t);
        m_slots[i].chunk.pixel_format = MODULE_PIXEL_RGB565;
    }
    return true;
}

void NesVideoOut::freeDmaBuffers(const module_host_api_v1 *host)
{
    for (uint8_t i = 0; i < kDmaSlotCount; ++i)
    {
        if (m_slots[i].pixels && host && host->heap.free)
        {
            host->heap.free(m_slots[i].pixels);
        }
        m_slots[i].pixels = nullptr;
        memset(&m_slots[i].chunk, 0, sizeof(m_slots[i].chunk));
    }
}

bool NesVideoOut::startWrite(String *err)
{
    const module_host_api_v1 *host = nes_port_host();
    if (m_stream_active)
    {
        return true;
    }
    if (!m_surface || !host || !hasTftStreamApi(host))
    {
        setError(err, "display stream api unavailable");
        return false;
    }
    if (host->display.start_write(m_surface) != MODULE_OK)
    {
        setError(err, "begin display stream failed");
        return false;
    }
    m_stream_active = true;
    return true;
}

bool NesVideoOut::pushImageDMA(const module_display_chunk_t *chunk,
                               int16_t x,
                               int16_t y,
                               uint16_t w,
                               uint16_t h,
                               String *err)
{
    const module_host_api_v1 *host = nes_port_host();
    if (!m_surface || !host || !chunk || !chunk->pixels)
    {
        setError(err, "display push without pixels");
        return false;
    }

    if (m_stream_supported)
    {
        if (!startWrite(err))
        {
            m_stream_supported = false;
        }
        else
        {
            const int32_t ret = host->display.push_image_dma(m_surface,
                                                             x,
                                                             y,
                                                             w,
                                                             h,
                                                             (const uint16_t *)chunk->pixels);
            if (ret != MODULE_OK)
            {
                (void)dmaWait(nullptr);
                setError(err, "push display dma failed");
                return false;
            }
            m_stream_queued++;
            return true;
        }
    }

    if (!host->display.push_image_dma)
    {
        setError(err, "display push api unavailable");
        return false;
    }
    const int32_t ret = host->display.push_image_dma(m_surface,
                                                     x,
                                                     y,
                                                     w,
                                                     h,
                                                     (const uint16_t *)chunk->pixels);
    if (ret != MODULE_OK)
    {
        setError(err, "display push failed");
        return false;
    }
    return true;
}

bool NesVideoOut::dmaWait(String *err)
{
    const module_host_api_v1 *host = nes_port_host();
    if (!m_stream_active)
    {
        m_stream_queued = 0;
        return true;
    }
    if (!host || !host->display.end_write)
    {
        m_stream_active = false;
        m_stream_queued = 0;
        setError(err, "display stream api unavailable");
        return false;
    }

    const int32_t ret = host->display.end_write(m_surface);
    m_stream_active = false;
    m_stream_queued = 0;
    if (ret != MODULE_OK)
    {
        setError(err, "end display stream failed");
        return false;
    }
    return true;
}

bool NesVideoOut::begin(const nes::VideoSpec &spec,
                        int16_t target_x,
                        int16_t target_y,
                        BaseType_t task_core,
                        UBaseType_t task_priority,
                        String *err)
{
    (void)task_core;
    (void)task_priority;

    const module_host_api_v1 *host = nes_port_host();
    if (!host || !host->display.acquire)
    {
        setError(err, "display api unavailable");
        return false;
    }

    end();

    m_spec = spec;
    if (m_spec.width == 0)
    {
        m_spec.width = 256;
    }
    if (m_spec.height == 0)
    {
        m_spec.height = 240;
    }
    m_spec.transfer_rows = sanitizeTransferRows(spec.transfer_rows,
                                               m_spec.height);
    m_target_x = target_x;
    m_target_y = target_y;
    m_has_chunk = false;
    m_preparing_slot = 0;
    m_next_slot = 0;
    m_failed = false;
    m_last_error[0] = '\0';
    m_stream_supported = hasTftStreamApi(host);
    m_stream_active = false;
    m_stream_queued = 0;

    if (!allocDmaBuffers(host, err))
    {
        return false;
    }

    module_display_desc_t desc = {};
    desc.size = sizeof(desc);
    desc.width = m_spec.width;
    desc.height = m_spec.height;
    desc.pixel_format = MODULE_PIXEL_RGB565;

    if (host->display.acquire("nes", &desc, &m_surface) != MODULE_OK || !m_surface)
    {
        freeDmaBuffers(host);
        setError(err, "display busy");
        return false;
    }

    return true;
}

void NesVideoOut::end()
{
    const module_host_api_v1 *host = nes_port_host();
    (void)dmaWait(nullptr);
    if (m_surface && host && host->display.release)
    {
        (void)host->display.release(m_surface);
    }
    m_surface = nullptr;
    m_has_chunk = false;
    m_preparing_slot = 0;
    m_next_slot = 0;
    m_failed = false;
    m_last_error[0] = '\0';
    m_stream_supported = false;
    m_stream_active = false;
    m_stream_queued = 0;
    memset(&m_chunk, 0, sizeof(m_chunk));
    freeDmaBuffers(host);
}

bool NesVideoOut::acquireTransferChunk(TransferChunk *out_chunk, String *err)
{
    if (!out_chunk)
    {
        setError(err, "display chunk missing");
        return false;
    }
    *out_chunk = TransferChunk{};
    if (!m_surface)
    {
        setError(err, "display inactive");
        return false;
    }
    if (m_stream_queued >= kDmaSlotCount && !dmaWait(err))
    {
        return false;
    }

    m_preparing_slot = m_next_slot;
    DmaSlot *slot = &m_slots[m_preparing_slot];
    if (!slot->pixels || slot->chunk.rows == 0)
    {
        setError(err, "display dma buffer missing");
        return false;
    }

    m_chunk = slot->chunk;
    m_has_chunk = true;
    out_chunk->pixels = slot->pixels;
    out_chunk->capacity_rows = slot->chunk.rows;
    out_chunk->slot_index = m_preparing_slot;
    return true;
}

bool NesVideoOut::submitTransferChunk(uint16_t start_row, uint16_t row_count, String *err)
{
    if (!m_surface || !m_has_chunk)
    {
        setError(err, "display submit without chunk");
        return false;
    }
    if (row_count == 0)
    {
        m_has_chunk = false;
        return true;
    }
    if (row_count > m_chunk.rows)
    {
        setError(err, "display rows exceed dma buffer");
        return false;
    }

    m_chunk.width = m_spec.width;
    m_chunk.pitch_bytes = (uint32_t)m_spec.width * sizeof(uint16_t);
    m_chunk.pixel_format = MODULE_PIXEL_RGB565;

    const bool frame_end = ((uint32_t)start_row + (uint32_t)row_count) >= (uint32_t)m_spec.height;
    if (!pushImageDMA(&m_chunk,
                      m_target_x,
                      (int16_t)(m_target_y + start_row),
                      m_spec.width,
                      row_count,
                      err))
    {
        m_has_chunk = false;
        return false;
    }

    m_next_slot = (uint8_t)((m_preparing_slot + 1u) % kDmaSlotCount);
    m_has_chunk = false;
    if ((m_stream_supported && (frame_end || m_stream_queued >= kDmaSlotCount)) &&
        !dmaWait(err))
    {
        return false;
    }
    return true;
}

void NesVideoOut::cancelTransferChunk()
{
    m_has_chunk = false;
}

bool NesVideoOut::consumeFailure(String *err)
{
    if (!m_failed)
    {
        return false;
    }
    setError(err, m_last_error[0] ? m_last_error : "nes display failed");
    m_failed = false;
    m_last_error[0] = '\0';
    return true;
}

bool NesVideoOut::active() const
{
    return m_surface != nullptr;
}

nes::VideoSpec NesVideoOut::spec() const
{
    return m_spec;
}

} // namespace nes::video
