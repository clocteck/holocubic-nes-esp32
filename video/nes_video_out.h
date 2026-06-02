#pragma once

#include <stdint.h>

#include "Arduino.h"
#include "nes_types.h"

namespace nes::video
{

class NesVideoOut
{
public:
    struct TransferChunk
    {
        uint16_t *pixels = nullptr;
        uint16_t capacity_rows = 0;
        uint8_t slot_index = 0;
    };

    void init();
    bool begin(const nes::VideoSpec &spec,
               int16_t target_x,
               int16_t target_y,
               BaseType_t task_core,
               UBaseType_t task_priority,
               String *err = nullptr);
    void end();
    bool acquireTransferChunk(TransferChunk *out_chunk, String *err = nullptr);
    bool submitTransferChunk(uint16_t start_row, uint16_t row_count, String *err = nullptr);
    void cancelTransferChunk();
    bool consumeFailure(String *err = nullptr);
    bool active() const;
    nes::VideoSpec spec() const;
    bool asyncSupported() const { return false; }
    bool asyncActive() const { return false; }
    uint8_t asyncSlots() const { return 0; }
    bool streamSupported() const { return m_stream_supported; }
    bool streamActive() const { return m_stream_active; }
    uint8_t streamSlots() const { return kDmaSlotCount; }
    uint8_t streamQueued() const { return m_stream_queued; }

private:
    static constexpr uint8_t kDmaSlotCount = 2;
    static constexpr uint16_t kDmaRows = 16;

    struct DmaSlot
    {
        module_display_chunk_t chunk = {};
        uint16_t *pixels = nullptr;
    };

    void setError(String *err, const char *text);
    void setFailure(const char *text);
    bool hasTftStreamApi(const module_host_api_v1 *host) const;
    bool allocDmaBuffers(const module_host_api_v1 *host, String *err);
    void freeDmaBuffers(const module_host_api_v1 *host);
    bool startWrite(String *err = nullptr);
    bool pushImageDMA(const module_display_chunk_t *chunk,
                      int16_t x,
                      int16_t y,
                      uint16_t w,
                      uint16_t h,
                      String *err = nullptr);
    bool dmaWait(String *err = nullptr);

    void *m_surface = nullptr;
    nes::VideoSpec m_spec = {};
    int16_t m_target_x = 0;
    int16_t m_target_y = 0;
    module_display_chunk_t m_chunk = {};
    bool m_has_chunk = false;
    uint8_t m_preparing_slot = 0;
    uint8_t m_next_slot = 0;
    volatile uint8_t m_failed = 0;
    char m_last_error[96] = {};

    DmaSlot m_slots[kDmaSlotCount] = {};
    bool m_stream_supported = false;
    bool m_stream_active = false;
    uint8_t m_stream_queued = 0;
};

} // namespace nes::video
