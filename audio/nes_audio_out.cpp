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
    /**
     * APU 只会产出 22050Hz / int16 / 单声道，链路里没有任何重采样或格式转换。
     * 这里把 spec 收敛到真实格式，避免：
     *   - channels=2 让 write() 按 frames*channels*2 读单声道缓冲（越界读）；
     *   - bits=8 让宿主按 8 位错解 16 位数据；
     *   - rate!=22050 让宿主按错误采样率播放（音调错）。
     * 真要支持多格式得先在本文件加转换，届时再放开这三个字段。
     */
    m_spec.sample_rate = kSourceSampleRate;
    m_spec.bits_per_sample = 16;
    m_spec.channels = 1;
    m_requested = spec.enabled;
    m_failed = false;
    m_error = "";
    m_dropped_bytes = 0;

    if (!spec.enabled)
    {
        return true;
    }

    /**
     * 注意后端要收 m_spec（归一化后的）而不是入参 spec：否则宿主的
     * module_audio_desc_t 里还写着用户传的 8bit / 双声道 / 别的采样率，
     * 而 write() 送的是 22050Hz/16bit/mono，宿主会按错的格式解码。
     */
    String host_err;
    if (beginHostAudio(m_spec, &host_err))
    {
        return true;
    }

    if (m_spec.lua_fallback && beginLuaQueue(m_spec, err))
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

    /**
     * `frames` 是 APU 交出的 int16 单声道采样个数，缓冲区就只有这么大。
     * 不能乘 m_spec.channels——那样 channels=2 时会多读一倍，越界读 APU
     * 的静态 audio_buffer。begin() 已把 channels 收敛到 1，这里再显式按
     * 源布局计算，双重保险。
     */
    const size_t bytes = frames * sizeof(int16_t);
    return writeBytes(samples, bytes);
}

size_t NesAudioOut::read(uint8_t *dst, size_t max_bytes)
{
    if (!dst || max_bytes == 0)
    {
        return 0;
    }

    /**
     * read() 在 Lua 任务上跑，freeQueue() 在 core 任务上跑（显示失败会让
     * core 任务直接进 releaseCoreObjects）。先登记为活跃读者再复查指针，
     * 否则「查完 m_queue 非空 → 被 free → memcpy」就是 UAF。
     */
    __atomic_add_fetch(&m_readers, 1, __ATOMIC_ACQ_REL);
    size_t copied = 0;
    if (!__atomic_load_n(&m_closing, __ATOMIC_ACQUIRE) &&
        m_backend == Backend::LuaQueue &&
        m_queue &&
        m_queue_capacity != 0)
    {
        copied = readLocked(dst, max_bytes);
    }
    __atomic_sub_fetch(&m_readers, 1, __ATOMIC_ACQ_REL);
    return copied;
}

size_t NesAudioOut::readLocked(uint8_t *dst, size_t max_bytes)
{
    const size_t head = __atomic_load_n(&m_queue_head, __ATOMIC_ACQUIRE);
    const size_t tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
    size_t available = (head + m_queue_capacity - tail) % m_queue_capacity;
    if (available == 0)
    {
        return 0;
    }
    if (max_bytes > available)
    {
        max_bytes = available;
    }
    max_bytes = align_down(max_bytes, kFrameBytes);
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

    /**
     * 队列满时生产者也会推进 tail 丢弃旧数据，所以 tail 有两个写者。
     * 用 CAS 提交：失败说明刚复制的区间已被生产者丢弃/覆盖，本次读作废
     * （宁可返回 0 让调用方重试，也不把撕裂的 PCM 交出去）。
     */
    size_t expected = tail;
    const size_t next_tail = (tail + max_bytes) % m_queue_capacity;
    if (!__atomic_compare_exchange_n(&m_queue_tail,
                                     &expected,
                                     next_tail,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
    {
        return 0;
    }
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

    const size_t frame_bytes = kFrameBytes;
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
    /**
     * 等满了还是没空间，就丢最旧的数据（保低延迟）。但 tail 同时也被消费者
     * 的 read() 推进，普通 store 会把消费者刚提交的 tail 覆盖回旧位置，
     * 让已消费的数据重新出现。这里用 CAS 提交，失败就重新取快照：
     * 消费者若已经腾出空间，free_bytes 够了直接退出。
     */
    for (uint8_t attempt = 0; attempt < 8; ++attempt)
    {
        tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
        used = (head + m_queue_capacity - tail) % m_queue_capacity;
        free_bytes = m_queue_capacity - used - 1;
        if (free_bytes >= bytes)
        {
            break;
        }
        size_t drop = align_down(bytes - free_bytes + frame_bytes - 1, frame_bytes);
        if (drop > used)
        {
            drop = align_down(used, frame_bytes);
        }
        if (drop == 0)
        {
            break;
        }
        size_t expected = tail;
        const size_t next_tail = (tail + drop) % m_queue_capacity;
        if (__atomic_compare_exchange_n(&m_queue_tail,
                                       &expected,
                                       next_tail,
                                       false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE))
        {
            m_dropped_bytes += (uint32_t)drop;
            break;
        }
    }

    tail = __atomic_load_n(&m_queue_tail, __ATOMIC_ACQUIRE);
    used = (head + m_queue_capacity - tail) % m_queue_capacity;
    free_bytes = m_queue_capacity - used - 1;
    if (free_bytes < bytes)
    {
        // 抢不到空间就丢这批新数据，绝不越过 tail 写坏消费者正在读的区间。
        m_dropped_bytes += (uint32_t)bytes;
        return true;
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
    /**
     * 先关门再等在途读者退出，才能安全 free。read() 里的读者计数保证
     * 不会出现「读者已通过指针检查、缓冲被释放」的 UAF。
     */
    __atomic_store_n(&m_closing, true, __ATOMIC_RELEASE);
    for (uint32_t spin = 0; spin < 200; ++spin)
    {
        if (__atomic_load_n(&m_readers, __ATOMIC_ACQUIRE) == 0)
        {
            break;
        }
        if (m_host && m_host->task.delay)
        {
            m_host->task.delay(1);
        }
        else if (m_host && m_host->task.yield)
        {
            m_host->task.yield();
        }
        else
        {
            break;
        }
    }

    if (m_queue && m_host && m_host->heap.free)
    {
        m_host->heap.free(m_queue);
    }
    m_queue = nullptr;
    m_queue_capacity = 0;
    m_queue_head = 0;
    m_queue_tail = 0;
    __atomic_store_n(&m_closing, false, __ATOMIC_RELEASE);
}

} // namespace nes::audio
