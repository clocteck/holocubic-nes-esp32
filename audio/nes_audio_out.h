#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

#include "module_abi.h"

namespace nes::audio
{

struct AudioSpec
{
    uint32_t sample_rate = 22050;
    uint16_t bits_per_sample = 16;
    uint16_t channels = 1;
    bool enabled = true;
    bool lua_fallback = true;
    size_t queue_bytes = 32768;
};

class NesAudioOut
{
public:
    bool begin(const AudioSpec &spec, String *err = nullptr);
    void end();

    bool active() const { return m_backend != Backend::None; }
    bool requested() const { return m_requested; }
    const char *backendName() const;
    bool write(const int16_t *samples, size_t frames);
    size_t read(uint8_t *dst, size_t max_bytes);
    size_t queuedBytes() const;
    uint32_t droppedBytes() const { return m_dropped_bytes; }
    bool consumeFailure(String *err = nullptr);

private:
    enum class Backend
    {
        None,
        HostAudio,
        LuaQueue,
    };

    bool beginHostAudio(const AudioSpec &spec, String *err);
    bool beginLuaQueue(const AudioSpec &spec, String *err);
    bool writeBytes(const void *data, size_t bytes);
    bool enqueueBytes(const uint8_t *data, size_t bytes);
    void setFailure(const char *text);
    void freeQueue();

    const module_host_api_v1 *m_host = nullptr;
    void *m_stream = nullptr;
    uint8_t *m_queue = nullptr;
    size_t m_queue_capacity = 0;
    volatile size_t m_queue_head = 0;
    volatile size_t m_queue_tail = 0;
    uint32_t m_dropped_bytes = 0;
    Backend m_backend = Backend::None;
    AudioSpec m_spec = {};
    bool m_requested = false;
    bool m_failed = false;
    String m_error;
};

} // namespace nes::audio
