#include "runtime/nes_core_bridge.h"

#include <string.h>

#include "Arduino.h"
#include "core/bus.h"
#include "core/cartridge.h"
#include "video/nes_video_out.h"
#include "nes_port.h"

namespace
{

enum CoreState
{
    CoreEmpty = 0,
    CoreLoaded = 1,
    CoreRunning = 2,
    CorePaused = 3,
    CoreStopping = 4,
    CoreError = 5,
};

enum CoreStage
{
    StageIdle = 0,
    StageTaskStart = 10,
    StageInitBegin = 20,
    StageVideoBegin = 21,
    StageVideoReady = 22,
    StageCartridgeBegin = 30,
    StageCartridgeReady = 31,
    StageBusBegin = 40,
    StageBusResetBegin = 41,
    StageBusReady = 42,
    StageFrameBegin = 50,
    StageFrameClockDone = 51,
    StageFrameDone = 52,
    StageStopping = 90,
    StageError = 99,
};

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0)
    {
        return;
    }
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t sanitize_nes_mask(uint32_t mask)
{
    return mask & 0xFFu;
}

static bool ptr_in_psram(const void *ptr)
{
    const uintptr_t addr = (uintptr_t)ptr;
    return addr >= 0x3C000000u && addr < 0x3E000000u;
}

class NesCoreRuntime
{
public:
    explicit NesCoreRuntime(const module_host_api_v1 *host)
        : m_host(host)
    {
        nes_port_set_host(host);
    }

    ~NesCoreRuntime()
    {
        stop(3000, true, nullptr, 0);
    }

    bool start(const char *rom_path, const nes_core_options_t *options, char *err, size_t err_size)
    {
        if (!m_host || !rom_path || !rom_path[0])
        {
            setError("rom path missing", err, err_size);
            return false;
        }
        if (m_task || m_running)
        {
            setError("nes already running", err, err_size);
            return false;
        }

        m_options = options ? *options : nes_core_options_t{};
        if (m_options.width == 0)
        {
            m_options.width = 256;
        }
        if (m_options.height == 0)
        {
            m_options.height = 240;
        }
        if (m_options.target_fps == 0)
        {
            m_options.target_fps = 60;
        }
        if (m_options.task_stack_bytes == 0)
        {
            m_options.task_stack_bytes = 16 * 1024;
        }
        if (m_options.task_priority == 0)
        {
            m_options.task_priority = 3;
        }

        copy_text(m_rom_path, sizeof(m_rom_path), rom_path);
        m_stop_requested = false;
        m_autorun = m_options.autorun != 0;
        m_paused = !m_autorun;
        m_running = false;
        m_task_exited = false;
        m_prepare_requested = false;
        m_prepare_result = 0;
        m_prepare_level = 0;
        m_step_frames = 0;
        m_frames = 0;
        m_started_ms = 0;
        m_stopped_ms = 0;
        m_last_error[0] = '\0';
        m_state = CoreLoaded;
        m_stage = StageIdle;

        void *task = nullptr;
        auto entry = reinterpret_cast<void (*)(void *)>(nes_port_exec_ptr(reinterpret_cast<const void *>(taskEntry)));
        const int32_t ret = m_host->task.create ? m_host->task.create("nes_core",
                                                                      entry,
                                                                      this,
                                                                      m_options.task_stack_bytes,
                                                                      m_options.task_priority,
                                                                      m_options.task_core,
                                                                      &task)
                                                : MODULE_ERR_UNSUPPORTED;
        if (ret != MODULE_OK || !task)
        {
            setError("create nes task failed", err, err_size);
            return false;
        }
        m_task = task;
        return true;
    }

    bool stop(uint32_t timeout_ms, bool force, char *err, size_t err_size)
    {
        if (!m_task && !m_running)
        {
            releaseCoreObjects();
            if (m_state != CoreError)
            {
                m_state = m_loaded ? CoreLoaded : CoreEmpty;
            }
            return true;
        }

        m_stop_requested = true;
        m_state = CoreStopping;
        setStage(StageStopping, "[nes.so] stop requested");
        const uint32_t start_ms = nes_port_millis();
        while (m_task && !m_task_exited && (nes_port_millis() - start_ms) < timeout_ms)
        {
            nes_port_delay(10);
        }

        if (m_task)
        {
            if (!m_task_exited && !force)
            {
                setError("nes stop timeout", err, err_size);
                return false;
            }
            if (m_host && m_host->task.remove)
            {
                m_host->task.remove(m_task);
            }
            m_task = nullptr;
            m_task_exited = false;
            m_running = false;
            m_stop_requested = false;
            m_autorun = false;
            m_paused = false;
            m_step_frames = 0;
            m_prepare_requested = false;
            m_prepare_result = 0;
            m_prepare_level = 0;
            releaseCoreObjects();
        }

        if (m_state != CoreError)
        {
            m_state = m_loaded ? CoreLoaded : CoreEmpty;
        }
        return true;
    }

    void pause(bool paused)
    {
        m_autorun = !paused;
        m_paused = paused;
        if (m_running)
        {
            m_state = paused ? CorePaused : CoreRunning;
        }
    }

    bool step(uint32_t frames, char *err, size_t err_size)
    {
        if (!m_task)
        {
            setError("nes is not running", err, err_size);
            return false;
        }
        if (frames == 0)
        {
            frames = 1;
        }
        if (frames > 120)
        {
            frames = 120;
        }
        m_autorun = false;
        m_paused = true;
        m_step_frames += frames;
        if (m_step_frames > 120)
        {
            m_step_frames = 120;
        }
        return true;
    }

    bool prepare(uint32_t timeout_ms, uint32_t level, char *err, size_t err_size)
    {
        if (!m_task)
        {
            setError("nes is not running", err, err_size);
            return false;
        }
        if (m_bus)
        {
            return true;
        }

        m_prepare_level = level;
        m_prepare_result = 0;
        m_prepare_requested = true;
        const uint32_t start_ms = nes_port_millis();
        while (m_prepare_requested && m_task && !m_task_exited &&
               (nes_port_millis() - start_ms) < timeout_ms)
        {
            nes_port_delay(10);
        }
        if (m_prepare_requested)
        {
            setError("nes init timeout", err, err_size);
            m_prepare_requested = false;
            return false;
        }
        if (m_prepare_result != 1)
        {
            if (err && err_size > 0)
            {
                copy_text(err, err_size, m_last_error[0] ? m_last_error : "nes init failed");
            }
            return false;
        }
        return true;
    }

    void input(uint32_t *out_gamepad_mask, uint32_t *out_nes_mask)
    {
        pollInput();
        if (out_gamepad_mask)
        {
            *out_gamepad_mask = m_last_gamepad_mask;
        }
        if (out_nes_mask)
        {
            *out_nes_mask = m_last_nes_mask;
        }
    }

    void setInputMask(uint32_t mask)
    {
        __atomic_store_n(&m_input_mask, sanitize_nes_mask(mask), __ATOMIC_RELEASE);
    }

    void status(nes_core_status_t *out)
    {
        if (!out)
        {
            return;
        }
        const uint32_t input_mask = __atomic_load_n(&m_input_mask, __ATOMIC_ACQUIRE);
        memset(out, 0, sizeof(*out));
        out->state = m_state;
        out->running = m_running ? 1 : 0;
        out->paused = m_paused ? 1 : 0;
        out->loaded = m_loaded ? 1 : 0;
        out->mapper = m_mapper_id;
        out->frames = m_frames;
        out->started_ms = m_started_ms;
        out->stopped_ms = m_stopped_ms;
        out->last_gamepad_mask = sanitize_nes_mask(input_mask);
        out->last_nes_mask = sanitize_nes_mask(input_mask);
        out->task_stack_ptr = m_task_stack_ptr;
        out->step_pending = m_step_frames;
        out->stage = m_stage;
        out->display_stream_supported = m_video.streamSupported() ? 1 : 0;
        out->display_stream_active = m_video.streamActive() ? 1 : 0;
        out->display_stream_slots = m_video.streamSlots();
        out->display_stream_queued = m_video.streamQueued();
        out->display_async_supported = m_video.asyncSupported() ? 1 : 0;
        out->display_async_active = m_video.asyncActive() ? 1 : 0;
        out->display_async_slots = m_video.asyncSlots();
        out->task_stack_psram = m_task_stack_psram ? 1 : 0;
        out->autorun = m_autorun ? 1 : 0;
        copy_text(out->last_error, sizeof(out->last_error), m_last_error);
    }

private:
    static void taskEntry(void *arg)
    {
        NesCoreRuntime *self = static_cast<NesCoreRuntime *>(arg);
        if (self)
        {
            self->taskLoop();
        }
    }

    void taskLoop()
    {
        while (!m_task)
        {
            nes_port_delay(1);
        }
        void *self_task = m_task;
        uint8_t stack_probe = 0;
        m_task_stack_ptr = (uint32_t)(uintptr_t)&stack_probe;
        m_task_stack_psram = ptr_in_psram(&stack_probe);
        m_running = true;
        m_state = CoreRunning;
        m_started_ms = nes_port_millis();
        setStage(StageTaskStart, "[nes.so] task start");

        if (m_task_stack_psram)
        {
            setError("nes task stack is in PSRAM; rebuild host with internal dynmod task stack", nullptr, 0);
            m_running = false;
            m_stop_requested = false;
            m_autorun = false;
            m_paused = false;
            m_step_frames = 0;
            m_prepare_requested = false;
            m_prepare_result = 2;
            m_prepare_level = 0;
            m_stopped_ms = nes_port_millis();
            m_task_exited = true;
            parkCurrentTask();
        }

        m_state = m_paused ? CorePaused : CoreRunning;
        const uint32_t frame_us = m_options.target_fps > 0 ? (1000000u / m_options.target_fps) : 16666u;
        while (!m_stop_requested)
        {
            if (m_prepare_requested)
            {
                m_prepare_result = createCoreObjects() ? 1 : 2;
                m_prepare_level = 0;
                m_prepare_requested = false;
                m_paused = true;
                m_state = m_prepare_result == 1 ? CorePaused : CoreError;
                continue;
            }

            const bool single_step = m_step_frames > 0;
            if (m_paused && !single_step)
            {
                m_state = CorePaused;
                nes_port_delay(20);
                continue;
            }

            if (!m_bus && !createCoreObjects())
            {
                m_prepare_level = 0;
                m_running = false;
                m_autorun = false;
                m_paused = true;
                m_step_frames = 0;
                break;
            }
            m_prepare_level = 0;

            const uint64_t frame_start = nes_port_micros();
            setStage(StageFrameBegin, "[nes.so] frame begin");
            pollInput();
            if (m_bus)
            {
                m_bus->controller = (uint8_t)m_last_nes_mask;
                m_bus->clock();
                setStage(StageFrameClockDone, "[nes.so] frame clock done");
                String render_err;
                if (m_bus->consumeRenderFailure(&render_err))
                {
                    setError(render_err.length() > 0 ? render_err.c_str() : "nes display push failed", nullptr, 0);
                    break;
                }
            }
            m_frames++;
            if (single_step && m_step_frames > 0)
            {
                m_step_frames--;
            }
            if (!m_autorun && m_step_frames == 0)
            {
                m_paused = true;
                m_state = CorePaused;
            }
            else
            {
                m_state = CoreRunning;
            }
            setStage(StageFrameDone, "[nes.so] frame done");

            const uint64_t elapsed = nes_port_micros() - frame_start;
            if (elapsed < frame_us)
            {
                const uint32_t sleep_us = frame_us - (uint32_t)elapsed;
                nes_port_delay(sleep_us / 1000u);
            }
            else if (m_host && m_host->task.yield)
            {
                m_host->task.yield();
            }
        }

        releaseCoreObjects();
        m_running = false;
        m_stop_requested = false;
        m_autorun = false;
        m_paused = false;
        m_step_frames = 0;
        m_prepare_requested = false;
        m_prepare_result = 0;
        m_prepare_level = 0;
        m_stopped_ms = nes_port_millis();
        if (m_state != CoreError)
        {
            m_state = m_loaded ? CoreLoaded : CoreEmpty;
        }
        m_task_exited = true;

        (void)self_task;
        parkCurrentTask();
    }

    bool createCoreObjects()
    {
        nes_port_set_host(m_host);
        setStage(StageInitBegin, "[nes.so] init begin");
        releaseCoreObjects();

        m_video.init();
        nes::VideoSpec spec = {};
        spec.width = m_options.width;
        spec.height = m_options.height;
        spec.center_on_screen = false;
        spec.transfer_rows = nes::config::kDefaultTransferRows;

        String err;
        setStage(StageVideoBegin, "[nes.so] video begin");
        if (!m_video.begin(spec,
                           (int16_t)m_options.x,
                           (int16_t)m_options.y,
                           (BaseType_t)m_options.task_core,
                           (UBaseType_t)m_options.task_priority,
                           &err))
        {
            setError(err.length() > 0 ? err.c_str() : "nes display begin failed", nullptr, 0);
            return false;
        }
        setStage(StageVideoReady, "[nes.so] video ready");
        if (m_prepare_level == 1)
        {
            m_loaded = true;
            return true;
        }

        setStage(StageCartridgeBegin, "[nes.so] cartridge begin");
        m_cartridge = new Cartridge(m_rom_path);
        if (!m_cartridge || !m_cartridge->ready())
        {
            const char *msg = m_cartridge ? m_cartridge->lastError().c_str() : "alloc cartridge failed";
            setError(msg && msg[0] ? msg : "load rom failed", nullptr, 0);
            releaseCoreObjects();
            return false;
        }
        m_mapper_id = m_cartridge->mapperId();
        setStage(StageCartridgeReady, "[nes.so] cartridge ready");
        if (m_prepare_level == 2)
        {
            m_loaded = true;
            return true;
        }

        setStage(StageBusBegin, "[nes.so] bus begin");
        m_bus = new Bus();
        if (!m_bus)
        {
            setError("alloc bus failed", nullptr, 0);
            releaseCoreObjects();
            return false;
        }

        m_bus->setVideoOut(&m_video);
        m_bus->insertCartridge(m_cartridge);
        if (m_prepare_level == 3)
        {
            m_loaded = true;
            setStage(StageBusReady, "[nes.so] bus attached");
            return true;
        }

        setStage(StageBusResetBegin, "[nes.so] bus reset begin");
        const uint32_t reset_level = (m_prepare_level >= 4 && m_prepare_level <= 7) ? (m_prepare_level - 3) : 0;
        if (!m_bus->reset(reset_level))
        {
            setError("nes bus reset failed", nullptr, 0);
            releaseCoreObjects();
            return false;
        }
        m_loaded = true;
        setStage(StageBusReady, "[nes.so] bus ready");
        return true;
    }

    void releaseCoreObjects()
    {
        if (m_bus)
        {
            delete m_bus;
            m_bus = nullptr;
        }
        if (m_cartridge)
        {
            delete m_cartridge;
            m_cartridge = nullptr;
        }
        m_video.end();
    }

    void pollInput()
    {
        const uint32_t mask = __atomic_load_n(&m_input_mask, __ATOMIC_ACQUIRE);
        m_last_gamepad_mask = mask;
        m_last_nes_mask = sanitize_nes_mask(mask);
    }

    void setError(const char *text, char *err, size_t err_size)
    {
        copy_text(m_last_error, sizeof(m_last_error), text ? text : "nes failed");
        if (err && err_size > 0)
        {
            copy_text(err, err_size, m_last_error);
        }
        m_stage = StageError;
        m_state = CoreError;
    }

    void setStage(uint32_t stage, const char *text)
    {
        m_stage = stage;
        if (stage == StageFrameBegin || stage == StageFrameClockDone || stage == StageFrameDone)
        {
            return;
        }
        if (m_host && m_host->serial.println && text)
        {
            m_host->serial.println(text);
        }
    }

    void parkCurrentTask()
    {
        for (;;)
        {
            nes_port_delay(1000);
        }
    }

    const module_host_api_v1 *m_host = nullptr;
    nes_core_options_t m_options = {};
    char m_rom_path[MODULE_PATH_MAX] = {};
    char m_last_error[96] = {};

    Bus *m_bus = nullptr;
    Cartridge *m_cartridge = nullptr;
    nes::video::NesVideoOut m_video;
    void *m_task = nullptr;

    volatile bool m_running = false;
    volatile bool m_stop_requested = false;
    volatile bool m_paused = false;
    volatile bool m_autorun = false;
    volatile bool m_task_exited = false;
    volatile bool m_prepare_requested = false;
    volatile uint32_t m_prepare_result = 0;
    volatile uint32_t m_prepare_level = 0;
    bool m_loaded = false;
    uint8_t m_mapper_id = 0;
    int32_t m_state = CoreEmpty;
    uint32_t m_frames = 0;
    uint32_t m_started_ms = 0;
    uint32_t m_stopped_ms = 0;
    uint32_t m_input_mask = 0;
    uint32_t m_last_gamepad_mask = 0;
    uint32_t m_last_nes_mask = 0;
    uint32_t m_task_stack_ptr = 0;
    volatile uint32_t m_step_frames = 0;
    volatile uint32_t m_stage = StageIdle;
    bool m_task_stack_psram = false;
};

} // namespace

extern "C" void *nes_core_create(const module_host_api_v1 *host)
{
    nes_port_set_host(host);
    return new NesCoreRuntime(host);
}

extern "C" void nes_core_destroy(void *runtime)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    delete core;
}

extern "C" int nes_core_start(void *runtime,
                              const char *rom_path,
                              const nes_core_options_t *options,
                              char *err,
                              size_t err_size)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    return core && core->start(rom_path, options, err, err_size) ? 1 : 0;
}

extern "C" int nes_core_stop(void *runtime, uint32_t timeout_ms, int force, char *err, size_t err_size)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    return !core || core->stop(timeout_ms, force != 0, err, err_size) ? 1 : 0;
}

extern "C" int nes_core_pause(void *runtime, int paused)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    if (!core)
    {
        return 0;
    }
    core->pause(paused != 0);
    return 1;
}

extern "C" int nes_core_prepare(void *runtime, uint32_t timeout_ms, uint32_t level, char *err, size_t err_size)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    return core && core->prepare(timeout_ms, level, err, err_size) ? 1 : 0;
}

extern "C" int nes_core_step(void *runtime, uint32_t frames, char *err, size_t err_size)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    return core && core->step(frames, err, err_size) ? 1 : 0;
}

extern "C" void nes_core_set_input_mask(void *runtime, uint32_t mask)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    if (core)
    {
        core->setInputMask(mask);
    }
}

extern "C" int nes_core_input(void *runtime, uint32_t *out_gamepad_mask, uint32_t *out_nes_mask)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    if (!core)
    {
        return 0;
    }
    core->input(out_gamepad_mask, out_nes_mask);
    return 1;
}

extern "C" void nes_core_status(void *runtime, nes_core_status_t *out)
{
    NesCoreRuntime *core = static_cast<NesCoreRuntime *>(runtime);
    if (core)
    {
        core->status(out);
    }
    else if (out)
    {
        memset(out, 0, sizeof(*out));
    }
}
