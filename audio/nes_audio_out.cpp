#include "audio/nes_audio_out.h"

#include <string.h>

#include "nes_port.h"

namespace
{
static size_t clamp_queue_bytes(size_t value)
{
    if (value < 4096)
    {
        return 4096;
    }
    if (value > 131072)
    {
        return 131072;
    }
    return value & ~(size_t)1;
}

static size_t align_down(size_t value, size_t align)
{
    return align > 1 ? (value - (value % align)) : value;
}
} // namespace

namespace nes::audio
{

const char *NesAudioOut::backendName() const
{
    switch (m_backend)
    {
    case Backend::HostAudio:
        return "host";
    case Backend::LuaQueue:
        return "lua";
    default:
        return "none";
    }
}

bool NesAudioOut::begin(const AudioSpec &spec, String *err)
{
    end();
    m_host = nes_port_host();
    m_spec = spec;
    m_requested = spec.enabled;
    m_failed = false;
    m_error = "";
    m_dropped_bytes = 0;

    if (!spec.enabled)
    {
        return true;
    }

    String host_err;
    if (beginHostAudio(spec, &host_err))
    {
        return true;
    }

    if (spec.lua_fallback && beginLuaQueue(spec, err))
    {
        return true;
    }

    if (err && err->length() == 0)
    {
        *err = host_err.length() > 0 ? host_err : "nes audio unavailable";
    }
    return false;
}

void NesAudioOut::end()
{
    if (m_backend == Backend::HostAudio && m_host && m_host->audio.end && m_stream)
    {
        (void)m_host->audio.end(m_stream);
    }

    freeQueue();
    m_stream = nullptr;
    m_backend = Backend::None;
}

bool NesAudioOut::write(const int16_t *samples, size_t frames)
{
    if (!samples || frames == 0 || m_backend == Backend::None)
    {
        return true;
    }

    const size_t bytes = frames * (size_t)m_spec.channels * sizeof(int16_t);
    return writeBytes(samples, bytes);
}

size_t NesAudioOut::read(uint8_t *dst, size_t max_bytes)
{
    if (!dst || max_bytes == 0 || m_backend != Backend::LuaQueue || !m_queue || m_queue_capacity == 0)
    {
        return 0;
    }

    const size_t head = __atomic_load_n(&m_queue_head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
    size_t available = (head + m_queue_capacity - tail) % m_queue_capacity;
    if (available == 0)
    {
        return 0;
    }
    if (max_bytes > available)
    {
        max_bytes = available;
    }
    const size_t frame_bytes = ((m_spec.channels == 0) ? 1u : (size_t)m_spec.channels) * sizeof(int16_t);
    max_bytes = align_down(max_bytes, frame_bytes);
    if (max_bytes == 0)
    {
        return 0;
    }

    const size_t first = (tail + max_bytes <= m_queue_capacity) ? max_bytes : (m_queue_capacity - tail);
    memcpy(dst, m_queue + tail, first);
    if (first < max_bytes)
    {
        memcpy(dst + first, m_queue, max_bytes - first);
    }
    tail = (tail + max_bytes) % m_queue_capacity;
    __atomic_store_n(&m_queue_tail, tail, __ATOMIC_RELEASE);
    return max_bytes;
}

size_t NesAudioOut::queuedBytes() const
{
    if (m_backend != Backend::LuaQueue || !m_queue || m_queue_capacity == 0)
    {
        return 0;
    }
    const size_t head = __atomic_load_n(&m_queue_head, __ATOMIC_ACQUIRE);
    const size_t tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
    return (head + m_queue_capacity - tail) % m_queue_capacity;
}

bool NesAudioOut::consumeFailure(String *err)
{
    if (!m_failed)
    {
        return false;
    }
    if (err)
    {
        *err = m_error;
    }
    m_failed = false;
    return true;
}

bool NesAudioOut::beginHostAudio(const AudioSpec &spec, String *err)
{
    if (!m_host || !m_host->audio.begin || !m_host->audio.write || !m_host->audio.end)
    {
        if (err)
        {
            *err = "host audio api missing";
        }
        return false;
    }

    module_audio_desc_t desc = {};
    desc.size = sizeof(desc);
    desc.sample_rate = spec.sample_rate;
    desc.bits_per_sample = spec.bits_per_sample;
    desc.channels = spec.channels;
    desc.flags = 0;

    void *stream = nullptr;
    const int32_t ret = m_host->audio.begin(&desc, &stream);
    if (ret != MODULE_OK || !stream)
    {
        if (err)
        {
            *err = "host audio unsupported";
        }
        return false;
    }

    m_stream = stream;
    m_backend = Backend::HostAudio;
    return true;
}

bool NesAudioOut::beginLuaQueue(const AudioSpec &spec, String *err)
{
    if (!m_host || !m_host->heap.malloc)
    {
        if (err)
        {
            *err = "lua audio queue heap missing";
        }
        return false;
    }

    const size_t capacity = clamp_queue_bytes(spec.queue_bytes);
    m_queue = (uint8_t *)m_host->heap.malloc(capacity, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
    if (!m_queue)
    {
        m_queue = (uint8_t *)m_host->heap.malloc(capacity, MODULE_HEAP_DEFAULT);
    }
    if (!m_queue)
    {
        if (err)
        {
            *err = "alloc lua audio queue failed";
        }
        return false;
    }

    m_queue_capacity = capacity;
    m_queue_head = 0;
    m_queue_tail = 0;
    m_backend = Backend::LuaQueue;
    return true;
}

bool NesAudioOut::writeBytes(const void *data, size_t bytes)
{
    if (!data || bytes == 0)
    {
        return true;
    }

    if (m_backend == Backend::HostAudio)
    {
        size_t offset = 0;
        while (offset < bytes)
        {
            size_t written = 0;
            const int32_t ret = m_host->audio.write(m_stream,
                                                    (const uint8_t *)data + offset,
                                                    bytes - offset,
                                                    &written);
            if (ret != MODULE_OK || written == 0)
            {
                setFailure("host audio write failed");
                return false;
            }
            offset += written;
        }
        return true;
    }

    if (m_backend == Backend::LuaQueue)
    {
        return enqueueBytes((const uint8_t *)data, bytes);
    }

    return true;
}

bool NesAudioOut::enqueueBytes(const uint8_t *data, size_t bytes)
{
    if (!m_queue || m_queue_capacity == 0)
    {
        return false;
    }

    const size_t frame_bytes = ((m_spec.channels == 0) ? 1u : (size_t)m_spec.channels) * sizeof(int16_t);
    bytes = align_down(bytes, frame_bytes);
    if (bytes == 0)
    {
        return true;
    }

    if (bytes >= m_queue_capacity)
    {
        const size_t keep = align_down(m_queue_capacity - frame_bytes, frame_bytes);
        data += bytes - keep;
        m_dropped_bytes += (uint32_t)(bytes - keep);
        bytes = keep;
    }

    size_t head = 0;
    size_t tail = 0;
    size_t used = 0;
    size_t free_bytes = 0;
    for (uint8_t wait_ms = 0; wait_ms < 20; ++wait_ms)
    {
        head = __atomic_load_n(&m_queue_head, __ATOMIC_ACQUIRE);
        tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
        used = (head + m_queue_capacity - tail) % m_queue_capacity;
        free_bytes = m_queue_capacity - used - 1;
        if (free_bytes >= bytes)
        {
            break;
        }
        if (m_host && m_host->task.delay)
        {
            m_host->task.delay(1);
        }
        else if (m_host && m_host->time.delay)
        {
            m_host->time.delay(1);
        }
        else if (m_host && m_host->task.yield)
        {
            m_host->task.yield();
        }
    }

    head = __atomic_load_n(&m_queue_head, __ATOMIC_ACQUIRE);
    tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
    used = (head + m_queue_capacity - tail) % m_queue_capacity;
    free_bytes = m_queue_capacity - used - 1;
    if (free_bytes < bytes)
    {
        size_t drop = align_down(bytes - free_bytes + frame_bytes - 1, frame_bytes);
        if (drop > used)
        {
            drop = align_down(used, frame_bytes);
        }
        tail = (tail + drop) % m_queue_capacity;
        __atomic_store_n(&m_queue_tail, tail, __ATOMIC_RELEASE);
        m_dropped_bytes += (uint32_t)drop;
    }

    const size_t first = (head + bytes <= m_queue_capacity) ? bytes : (m_queue_capacity - head);
    memcpy(m_queue + head, data, first);
    if (first < bytes)
    {
        memcpy(m_queue, data + first, bytes - first);
    }
    head = (head + bytes) % m_queue_capacity;
    __atomic_store_n(&m_queue_head, head, __ATOMIC_RELEASE);
    return true;
}

void NesAudioOut::setFailure(const char *text)
{
    m_failed = true;
    m_error = text ? text : "nes audio failed";
}

void NesAudioOut::freeQueue()
{
    if (m_queue && m_host && m_host->heap.free)
    {
        m_host->heap.free(m_queue);
    }
    m_queue = nullptr;
    m_queue_capacity = 0;
    m_queue_head = 0;
    m_queue_tail = 0;
}

} // namespace nes::audio
