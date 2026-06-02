#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if __has_include("module_abi.h")
#include "module_abi.h"
#else
#include "../include/module_abi.h"
#endif
#include "../runtime/nes_core_bridge.h"

#define NES_VERSION "0.1.0"
#define NES_FRAME_WIDTH 256
#define NES_FRAME_HEIGHT 240
#define NES_DEFAULT_FPS 30
#define NES_DEFAULT_TASK_STACK (12u * 1024u)
#define NES_DEFAULT_TASK_PRIORITY 3u
#define NES_DEFAULT_TASK_CORE 1

#define NES_PAD_A (1u << 0)
#define NES_PAD_B (1u << 1)
#define NES_PAD_SELECT (1u << 2)
#define NES_PAD_START (1u << 3)
#define NES_PAD_UP (1u << 4)
#define NES_PAD_DOWN (1u << 5)
#define NES_PAD_LEFT (1u << 6)
#define NES_PAD_RIGHT (1u << 7)

#define NES_MODULE_EXPORT __attribute__((visibility("default"), used))

typedef enum nes_state_t {
    NES_STATE_EMPTY = 0,
    NES_STATE_LOADED = 1,
    NES_STATE_RUNNING = 2,
    NES_STATE_PAUSED = 3,
    NES_STATE_STOPPING = 4,
    NES_STATE_ERROR = 5,
} nes_state_t;

typedef struct nes_rom_info_t {
    uint8_t valid;
    uint8_t nes2;
    uint8_t trainer;
    uint8_t four_screen;
    uint8_t vertical_mirror;
    uint8_t prg_banks;
    uint8_t chr_banks;
    uint8_t mapper;
    uint32_t prg_bytes;
    uint32_t chr_bytes;
    uint32_t prg_offset;
    uint32_t chr_offset;
    uint64_t file_size;
    char mirror[16];
} nes_rom_info_t;

typedef struct nes_module_instance_t nes_module_instance_t;

typedef struct nes_session_t {
    nes_module_instance_t *owner;
    nes_rom_info_t rom;
    nes_state_t state;
    uint8_t loaded;
    volatile uint8_t running;
    volatile uint8_t stop_requested;
    volatile uint8_t paused;
    char rom_path[MODULE_PATH_MAX];
    char last_error[96];
    void *core_runtime;
    uint32_t frames;
    uint32_t started_ms;
    uint32_t stopped_ms;
    uint32_t last_gamepad_mask;
    uint32_t last_nes_mask;
    uint32_t task_stack_ptr;
    uint32_t step_pending;
    uint32_t stage;
    uint8_t display_stream_supported;
    uint8_t display_stream_active;
    uint8_t display_stream_slots;
    uint8_t display_stream_queued;
    uint8_t display_async_supported;
    uint8_t display_async_active;
    uint8_t display_async_slots;
    uint8_t task_stack_psram;
    uint8_t autorun;
    uint8_t autorun_active;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t target_fps;
    uint32_t task_stack_bytes;
    uint32_t task_priority;
    int32_t task_core;
} nes_session_t;

struct nes_module_instance_t {
    const module_host_api_v1 *host;
    uint32_t created_ms;
    char path[MODULE_PATH_MAX];
    nes_session_t session;
};

static const module_host_api_v1 *s_host = NULL;

static const module_manifest_t s_manifest = {
    MODULE_MANIFEST_MAGIC,
    MODULE_ABI_VERSION,
    sizeof(module_manifest_t),
    "nes",
    NES_VERSION,
    "NES dynamic module MVP",
    0,
    MODULE_ABI_VERSION,
};

/**
 * @brief Copy a C string into a fixed buffer.
 */
static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/**
 * @brief Store a human-readable session error.
 */
static void set_error(nes_session_t *session, const char *err)
{
    if (session) {
        copy_text(session->last_error, sizeof(session->last_error), err ? err : "nes failed");
        session->state = NES_STATE_ERROR;
    }
}

/**
 * @brief Return the host API from a session or the global fallback.
 */
static const module_host_api_v1 *session_host(nes_session_t *session)
{
    if (session && session->owner && session->owner->host) {
        return session->owner->host;
    }
    return s_host;
}

/**
 * @brief Push `nil, err` in NodeMCU-style result form.
 */
static int push_error(lua_State *L, const module_host_api_v1 *host, const char *err)
{
    if (!host) {
        return 0;
    }
    host->lua.pushnil(L);
    host->lua.pushstring(L, err ? err : "nes failed");
    return 2;
}

/**
 * @brief Push boolean true in NodeMCU-style success form.
 */
static int push_true(lua_State *L, const module_host_api_v1 *host)
{
    host->lua.pushboolean(L, 1);
    return 1;
}

/**
 * @brief Set a string field in the Lua table at the top of the stack.
 */
static void set_string_field(lua_State *L, const module_host_api_v1 *host, const char *key, const char *value)
{
    host->lua.pushstring(L, value ? value : "");
    host->lua.setfield(L, -2, key);
}

/**
 * @brief Set an integer field in the Lua table at the top of the stack.
 */
static void set_integer_field(lua_State *L, const module_host_api_v1 *host, const char *key, int64_t value)
{
    host->lua.pushinteger(L, value);
    host->lua.setfield(L, -2, key);
}

/**
 * @brief Set a boolean field in the Lua table at the top of the stack.
 */
static void set_boolean_field(lua_State *L, const module_host_api_v1 *host, const char *key, int value)
{
    host->lua.pushboolean(L, value ? 1 : 0);
    host->lua.setfield(L, -2, key);
}

/**
 * @brief Attach a C closure with one lightuserdata upvalue.
 */
static void set_closure_field(lua_State *L,
                              const module_host_api_v1 *host,
                              const char *key,
                              module_lua_cfunction_t fn,
                              void *upvalue)
{
    host->lua.pushlightuserdata(L, upvalue);
    host->lua.pushcclosure(L, fn, 1);
    host->lua.setfield(L, -2, key);
}

/**
 * @brief Read a module instance pointer from a Lua closure upvalue.
 */
static nes_module_instance_t *instance_from_lua(lua_State *L)
{
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index) {
        return NULL;
    }
    return (nes_module_instance_t *)s_host->lua.touserdata(L, s_host->lua.upvalue_index(1));
}

/**
 * @brief Read a session pointer from a Lua closure upvalue.
 */
static nes_session_t *session_from_lua(lua_State *L)
{
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index) {
        return NULL;
    }
    return (nes_session_t *)s_host->lua.touserdata(L, s_host->lua.upvalue_index(1));
}

/**
 * @brief Reset runtime options to conservative defaults.
 */
static void session_set_defaults(nes_session_t *session)
{
    if (!session) {
        return;
    }
    session->state = NES_STATE_EMPTY;
    session->loaded = 0;
    session->running = 0;
    session->stop_requested = 0;
    session->paused = 0;
    session->core_runtime = NULL;
    session->frames = 0;
    session->started_ms = 0;
    session->stopped_ms = 0;
    session->last_gamepad_mask = 0;
    session->last_nes_mask = 0;
    session->task_stack_ptr = 0;
    session->step_pending = 0;
    session->stage = 0;
    session->task_stack_psram = 0;
    session->autorun = 0;
    session->autorun_active = 0;
    session->x = 32;
    session->y = 0;
    session->width = NES_FRAME_WIDTH;
    session->height = NES_FRAME_HEIGHT;
    session->target_fps = NES_DEFAULT_FPS;
    session->task_stack_bytes = NES_DEFAULT_TASK_STACK;
    session->task_priority = NES_DEFAULT_TASK_PRIORITY;
    session->task_core = NES_DEFAULT_TASK_CORE;
    session->rom_path[0] = '\0';
    session->last_error[0] = '\0';
    memset(&session->rom, 0, sizeof(session->rom));
}

static void session_sync_runtime(nes_session_t *session);
static void session_release_runtime(nes_session_t *session);

/**
 * @brief Read exactly len bytes from a host file handle.
 */
static int read_exact(const module_host_api_v1 *host, void *file, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        size_t got = 0;
        int32_t ret = host->file.read(file, dst + total, len - total, &got);
        if (ret != MODULE_OK || got == 0) {
            return 0;
        }
        total += got;
    }
    return 1;
}

/**
 * @brief Parse an iNES header from SD without keeping the ROM file open.
 */
static int parse_rom_header(const module_host_api_v1 *host,
                            const char *path,
                            nes_rom_info_t *out,
                            char *err,
                            size_t err_size)
{
    uint8_t header[16];
    void *file = NULL;
    int32_t ret = MODULE_OK;
    uint64_t file_size = 0;
    uint32_t expected_min_size = 0;

    if (!host || !path || !out) {
        copy_text(err, err_size, "rom path missing");
        return 0;
    }

    memset(out, 0, sizeof(*out));
    if (host->sd.begin && host->sd.begin() != MODULE_OK) {
        copy_text(err, err_size, "sd begin failed");
        return 0;
    }

    ret = host->sd.open(path, MODULE_FILE_READ, &file);
    if (ret != MODULE_OK || !file) {
        copy_text(err, err_size, "open rom failed");
        return 0;
    }

    if (host->file.size_bytes) {
        (void)host->file.size_bytes(file, &file_size);
    }

    if (!read_exact(host, file, header, sizeof(header))) {
        host->file.close(file);
        copy_text(err, err_size, "read rom header failed");
        return 0;
    }
    host->file.close(file);

    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        copy_text(err, err_size, "invalid iNES header");
        return 0;
    }

    out->nes2 = ((header[7] & 0x0C) == 0x08) ? 1 : 0;
    if (out->nes2) {
        copy_text(err, err_size, "NES 2.0 rom is not supported yet");
        return 0;
    }

    out->trainer = (header[6] & 0x04) ? 1 : 0;
    out->four_screen = (header[6] & 0x08) ? 1 : 0;
    out->vertical_mirror = (header[6] & 0x01) ? 1 : 0;
    out->mapper = (uint8_t)((header[6] >> 4) | (header[7] & 0xF0));
    out->prg_banks = header[4];
    out->chr_banks = header[5];
    out->prg_bytes = (uint32_t)out->prg_banks * 16384u;
    out->chr_bytes = (uint32_t)out->chr_banks * 8192u;
    out->prg_offset = 16u + (out->trainer ? 512u : 0u);
    out->chr_offset = out->prg_offset + out->prg_bytes;
    out->file_size = file_size;

    if (out->four_screen) {
        copy_text(out->mirror, sizeof(out->mirror), "four_screen");
    } else if (out->vertical_mirror) {
        copy_text(out->mirror, sizeof(out->mirror), "vertical");
    } else {
        copy_text(out->mirror, sizeof(out->mirror), "horizontal");
    }

    if (out->prg_banks == 0) {
        copy_text(err, err_size, "rom has no PRG banks");
        return 0;
    }

    expected_min_size = out->chr_offset + out->chr_bytes;
    if (file_size != 0 && file_size < expected_min_size) {
        copy_text(err, err_size, "rom file is shorter than header declares");
        return 0;
    }

    out->valid = 1;
    copy_text(err, err_size, "");
    return 1;
}

/**
 * @brief Push parsed ROM information as a Lua table.
 */
static void push_rom_info(lua_State *L, const module_host_api_v1 *host, const nes_rom_info_t *info)
{
    host->lua.createtable(L, 0, 20);
    if (!info) {
        set_boolean_field(L, host, "valid", 0);
        return;
    }

    set_boolean_field(L, host, "valid", info->valid);
    set_boolean_field(L, host, "nes2", info->nes2);
    set_boolean_field(L, host, "trainer", info->trainer);
    set_boolean_field(L, host, "four_screen", info->four_screen);
    set_boolean_field(L, host, "vertical_mirror", info->vertical_mirror);
    set_integer_field(L, host, "mapper", info->mapper);
    set_integer_field(L, host, "prg_banks", info->prg_banks);
    set_integer_field(L, host, "chr_banks", info->chr_banks);
    set_integer_field(L, host, "prg_bytes", info->prg_bytes);
    set_integer_field(L, host, "chr_bytes", info->chr_bytes);
    set_integer_field(L, host, "prg_offset", info->prg_offset);
    set_integer_field(L, host, "chr_offset", info->chr_offset);
    set_integer_field(L, host, "file_size", (int64_t)info->file_size);
    set_string_field(L, host, "mirror", info->mirror);
}

/**
 * @brief Read an integer option from a Lua table.
 */
static int read_table_integer(lua_State *L,
                              const module_host_api_v1 *host,
                              int table_index,
                              const char *key,
                              int64_t *out)
{
    int top = host->lua.gettop(L);
    int found = 0;
    host->lua.getfield(L, table_index, key);
    if (host->lua.isnumber(L, -1)) {
        *out = host->lua.tointeger(L, -1);
        found = 1;
    }
    host->lua.settop(L, top);
    return found;
}

/**
 * @brief Read a boolean option from a Lua table.
 */
static int read_table_boolean(lua_State *L,
                              const module_host_api_v1 *host,
                              int table_index,
                              const char *key,
                              int *out)
{
    int top = host->lua.gettop(L);
    int found = 0;
    host->lua.getfield(L, table_index, key);
    if (!host->lua.isnil(L, -1)) {
        *out = host->lua.toboolean(L, -1) ? 1 : 0;
        found = 1;
    }
    host->lua.settop(L, top);
    return found;
}

/**
 * @brief Read a string option from a Lua table into a fixed buffer.
 */
static int read_table_string(lua_State *L,
                             const module_host_api_v1 *host,
                             int table_index,
                             const char *key,
                             char *out,
                             size_t out_size)
{
    int top = host->lua.gettop(L);
    int found = 0;
    host->lua.getfield(L, table_index, key);
    if (host->lua.isstring(L, -1)) {
        copy_text(out, out_size, host->lua.tostring(L, -1));
        found = 1;
    }
    host->lua.settop(L, top);
    return found;
}

/**
 * @brief Clamp and apply a signed integer option.
 */
static void apply_u32_option(int64_t value, uint32_t min_value, uint32_t max_value, uint32_t *out)
{
    if (!out) {
        return;
    }
    if (value < (int64_t)min_value) {
        *out = min_value;
    } else if (value > (int64_t)max_value) {
        *out = max_value;
    } else {
        *out = (uint32_t)value;
    }
}

/**
 * @brief Apply Lua create/start options to the session.
 */
static void apply_options(lua_State *L, const module_host_api_v1 *host, nes_session_t *session, int table_index)
{
    int64_t n = 0;
    int top = 0;
    int keep_center = 1;
    int autorun = 0;

    if (!session || table_index <= 0 || !host->lua.istable(L, table_index)) {
        return;
    }

    if (read_table_integer(L, host, table_index, "fps", &n)) {
        apply_u32_option(n, 1, 60, &session->target_fps);
    }
    if (read_table_integer(L, host, table_index, "task_stack", &n)) {
        apply_u32_option(n, 4096, 32768, &session->task_stack_bytes);
    }
    if (read_table_integer(L, host, table_index, "task_priority", &n)) {
        apply_u32_option(n, 1, 10, &session->task_priority);
    }
    if (read_table_integer(L, host, table_index, "task_core", &n)) {
        session->task_core = (n < 0) ? -1 : ((n > 1) ? 1 : (int32_t)n);
    }

    (void)read_table_boolean(L, host, table_index, "center", &keep_center);
    if (read_table_boolean(L, host, table_index, "autorun", &autorun) ||
        read_table_boolean(L, host, table_index, "run", &autorun)) {
        session->autorun = autorun ? 1 : 0;
    }

    top = host->lua.gettop(L);
    host->lua.getfield(L, table_index, "video");
    if (host->lua.istable(L, -1)) {
        int video_index = host->lua.gettop(L);
        if (read_table_integer(L, host, video_index, "x", &n)) {
            session->x = (n < 0) ? 0 : (uint16_t)n;
            keep_center = 0;
        }
        if (read_table_integer(L, host, video_index, "y", &n)) {
            session->y = (n < 0) ? 0 : (uint16_t)n;
        }
    }
    host->lua.settop(L, top);

    if (read_table_integer(L, host, table_index, "x", &n)) {
        session->x = (n < 0) ? 0 : (uint16_t)n;
        keep_center = 0;
    }
    if (read_table_integer(L, host, table_index, "y", &n)) {
        session->y = (n < 0) ? 0 : (uint16_t)n;
    }

    if (keep_center && host->display.width) {
        int32_t screen_w = host->display.width();
        if (screen_w > NES_FRAME_WIDTH) {
            session->x = (uint16_t)((screen_w - NES_FRAME_WIDTH) / 2);
        }
    }
}

/**
 * @brief Load ROM metadata into a stopped session.
 */
static int session_load_rom(nes_session_t *session, const char *path)
{
    const module_host_api_v1 *host = session_host(session);
    nes_rom_info_t info;
    char err[96];

    if (!session || !host || !path || !path[0]) {
        set_error(session, "rom path missing");
        return 0;
    }
    session_sync_runtime(session);
    if (session->running) {
        set_error(session, "stop nes before loading another rom");
        return 0;
    }

    session_release_runtime(session);
    if (!parse_rom_header(host, path, &info, err, sizeof(err))) {
        set_error(session, err);
        return 0;
    }

    session->rom = info;
    session->loaded = 1;
    session->state = NES_STATE_LOADED;
    session->frames = 0;
    session->last_gamepad_mask = 0;
    session->last_nes_mask = 0;
    session->step_pending = 0;
    session->autorun_active = 0;
    session->last_error[0] = '\0';
    copy_text(session->rom_path, sizeof(session->rom_path), path);
    return 1;
}

/**
 * @brief Keep only the standard 8-bit NES pad mask bits.
 */
static uint32_t sanitize_nes_mask(uint32_t mask)
{
    return mask & (NES_PAD_A |
                   NES_PAD_B |
                   NES_PAD_SELECT |
                   NES_PAD_START |
                   NES_PAD_UP |
                   NES_PAD_DOWN |
                   NES_PAD_LEFT |
                   NES_PAD_RIGHT);
}

/**
 * @brief Copy status from the C++ core bridge back into the Lua-facing session.
 */
static void session_sync_runtime(nes_session_t *session)
{
    nes_core_status_t status;
    if (!session || !session->core_runtime) {
        return;
    }

    memset(&status, 0, sizeof(status));
    nes_core_status(session->core_runtime, &status);
    session->state = (nes_state_t)status.state;
    session->running = status.running;
    session->paused = status.paused;
    session->frames = status.frames;
    session->started_ms = status.started_ms;
    session->stopped_ms = status.stopped_ms;
    session->last_gamepad_mask = status.last_gamepad_mask;
    session->last_nes_mask = status.last_nes_mask;
    session->task_stack_ptr = status.task_stack_ptr;
    session->step_pending = status.step_pending;
    session->stage = status.stage;
    session->display_stream_supported = status.display_stream_supported;
    session->display_stream_active = status.display_stream_active;
    session->display_stream_slots = status.display_stream_slots;
    session->display_stream_queued = status.display_stream_queued;
    session->display_async_supported = status.display_async_supported;
    session->display_async_active = status.display_async_active;
    session->display_async_slots = status.display_async_slots;
    session->task_stack_psram = status.task_stack_psram;
    session->autorun_active = status.autorun;
    if (status.last_error[0]) {
        copy_text(session->last_error, sizeof(session->last_error), status.last_error);
    }
}

/**
 * @brief Release runtime resources owned by the C++ core bridge.
 */
static void session_release_runtime(nes_session_t *session)
{
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return;
    }
    if (session->core_runtime) {
        nes_core_destroy(session->core_runtime);
        session->core_runtime = NULL;
    }
}

/**
 * @brief Start a loaded session using the copied src/nes core.
 */
static int session_start(nes_session_t *session)
{
    const module_host_api_v1 *host = session_host(session);
    nes_core_options_t options;
    char err[96];

    if (!session || !host) {
        return 0;
    }
    if (!session->loaded) {
        set_error(session, "load a rom before start");
        return 0;
    }
    session_sync_runtime(session);
    if (session->running) {
        set_error(session, "nes already running");
        return 0;
    }

    if (!session->core_runtime) {
        session->core_runtime = nes_core_create(host);
    }
    if (!session->core_runtime) {
        set_error(session, "create nes core failed");
        return 0;
    }
    nes_core_set_input_mask(session->core_runtime, session->last_nes_mask);

    memset(&options, 0, sizeof(options));
    options.x = session->x;
    options.y = session->y;
    options.width = session->width;
    options.height = session->height;
    options.target_fps = session->target_fps;
    options.task_stack_bytes = session->task_stack_bytes;
    options.task_priority = session->task_priority;
    options.task_core = session->task_core;
    options.autorun = session->autorun ? 1 : 0;
    err[0] = '\0';

    if (!nes_core_start(session->core_runtime, session->rom_path, &options, err, sizeof(err))) {
        set_error(session, err[0] ? err : "start nes core failed");
        return 0;
    }
    nes_core_set_input_mask(session->core_runtime, session->last_nes_mask);

    session->state = NES_STATE_RUNNING;
    session->running = 1;
    session->paused = session->autorun ? 0 : 1;
    session->autorun_active = session->autorun ? 1 : 0;
    session->step_pending = 0;
    session->frames = 0;
    session->last_error[0] = '\0';
    session_sync_runtime(session);
    return 1;
}

/**
 * @brief Stop the copied src/nes core.
 */
static int session_stop(nes_session_t *session, uint32_t timeout_ms, int force)
{
    char err[96];
    if (!session) {
        return 0;
    }
    if (!session->core_runtime) {
        session->running = 0;
        session->paused = 0;
        session->autorun_active = 0;
        session->step_pending = 0;
        session->state = session->loaded ? NES_STATE_LOADED : NES_STATE_EMPTY;
        return 1;
    }

    err[0] = '\0';
    session->state = NES_STATE_STOPPING;
    if (!nes_core_stop(session->core_runtime, timeout_ms, force, err, sizeof(err))) {
        set_error(session, err[0] ? err : "nes stop failed");
        session_sync_runtime(session);
        return 0;
    }

    session_sync_runtime(session);
    if (session->state != NES_STATE_ERROR) {
        session->state = session->loaded ? NES_STATE_LOADED : NES_STATE_EMPTY;
        session->running = 0;
        session->paused = 0;
        session->autorun_active = 0;
        session->step_pending = 0;
    }
    return 1;
}

/**
 * @brief Return a stable state name for Lua.
 */
static const char *state_name(nes_state_t state)
{
    switch (state) {
    case NES_STATE_EMPTY:
        return "empty";
    case NES_STATE_LOADED:
        return "loaded";
    case NES_STATE_RUNNING:
        return "running";
    case NES_STATE_PAUSED:
        return "paused";
    case NES_STATE_STOPPING:
        return "stopping";
    case NES_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

/**
 * @brief Push a session status table.
 */
static void push_session_info(lua_State *L, const module_host_api_v1 *host, nes_session_t *session)
{
    host->lua.createtable(L, 0, 37);
    if (!session) {
        set_string_field(L, host, "state", "missing");
        return;
    }

    session_sync_runtime(session);
    set_string_field(L, host, "version", NES_VERSION);
    set_string_field(L, host, "state", state_name(session->state));
    set_string_field(L, host, "rom", session->rom_path);
    set_string_field(L, host, "last_error", session->last_error);
    set_boolean_field(L, host, "loaded", session->loaded);
    set_boolean_field(L, host, "running", session->running);
    set_boolean_field(L, host, "paused", session->paused);
    set_boolean_field(L, host, "autorun", session->autorun);
    set_boolean_field(L, host, "autorun_active", session->autorun_active);
    set_boolean_field(L, host, "emulated", 1);
    set_integer_field(L, host, "frames", session->frames);
    set_integer_field(L, host, "step_pending", session->step_pending);
    set_integer_field(L, host, "stage", session->stage);
    set_boolean_field(L, host, "display_stream_supported", session->display_stream_supported);
    set_boolean_field(L, host, "display_stream_active", session->display_stream_active);
    set_integer_field(L, host, "display_stream_slots", session->display_stream_slots);
    set_integer_field(L, host, "display_stream_queued", session->display_stream_queued);
    set_boolean_field(L, host, "display_async_supported", session->display_async_supported);
    set_boolean_field(L, host, "display_async_active", session->display_async_active);
    set_integer_field(L, host, "display_async_slots", session->display_async_slots);
    set_integer_field(L, host, "started_ms", session->started_ms);
    set_integer_field(L, host, "stopped_ms", session->stopped_ms);
    set_integer_field(L, host, "x", session->x);
    set_integer_field(L, host, "y", session->y);
    set_integer_field(L, host, "width", session->width);
    set_integer_field(L, host, "height", session->height);
    set_integer_field(L, host, "fps", session->target_fps);
    set_integer_field(L, host, "task_stack", session->task_stack_bytes);
    set_integer_field(L, host, "task_stack_ptr", session->task_stack_ptr);
    set_boolean_field(L, host, "task_stack_psram", session->task_stack_psram);
    set_integer_field(L, host, "task_priority", session->task_priority);
    set_integer_field(L, host, "task_core", session->task_core);
    set_integer_field(L, host, "gamepad_mask", session->last_gamepad_mask);
    set_integer_field(L, host, "pad_mask", session->last_nes_mask);
    set_integer_field(L, host, "heap_psram_free",
                      host->heap.free_size ? (int64_t)host->heap.free_size(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT) : 0);
    set_integer_field(L, host, "heap_internal_free",
                      host->heap.free_size ? (int64_t)host->heap.free_size(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT) : 0);
    set_integer_field(L, host, "display_width", host->display.width ? host->display.width() : 0);
    set_integer_field(L, host, "display_height", host->display.height ? host->display.height() : 0);

    push_rom_info(L, host, &session->rom);
    host->lua.setfield(L, -2, "rom_info");
}

/**
 * @brief Return the path argument from either dot-call or colon-call style.
 */
static const char *method_path_arg(lua_State *L, const module_host_api_v1 *host)
{
    if (host->lua.gettop(L) >= 2 && host->lua.istable(L, 1) && host->lua.isstring(L, 2)) {
        return host->lua.checkstring(L, 2);
    }
    return host->lua.checkstring(L, 1);
}

/**
 * @brief Lua API: nes.read_header(path) -> table | nil, err.
 */
static int l_read_header(lua_State *L)
{
    nes_module_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    nes_rom_info_t info;
    char err[96];
    const char *path = NULL;

    if (!host) {
        return 0;
    }

    path = host->lua.checkstring(L, 1);
    if (!parse_rom_header(host, path, &info, err, sizeof(err))) {
        return push_error(L, host, err);
    }
    push_rom_info(L, host, &info);
    return 1;
}

/**
 * @brief Lua API: nes.info() -> table.
 */
static int l_module_info(lua_State *L)
{
    nes_module_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    if (!inst || !host) {
        return push_error(L, host, "nes.info: instance missing");
    }

    push_session_info(L, host, &inst->session);
    set_string_field(L, host, "module_path", inst->path);
    set_integer_field(L, host, "created_ms", inst->created_ms);
    return 1;
}

/**
 * @brief Lua API: emu:load(path) -> true | nil, err.
 */
static int l_emu_load(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    const char *path = NULL;
    if (!session || !host) {
        return push_error(L, host, "nes.load: session missing");
    }

    path = method_path_arg(L, host);
    if (!session_load_rom(session, path)) {
        return push_error(L, host, session->last_error);
    }
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:start() -> true | nil, err.
 */
static int l_emu_start(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.start: session missing");
    }

    if (!session_start(session)) {
        return push_error(L, host, session->last_error);
    }
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:stop() -> true | nil, err.
 */
static int l_emu_stop(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.stop: session missing");
    }

    if (!session_stop(session, 2000, 0)) {
        return push_error(L, host, session->last_error);
    }
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:pause() -> true.
 */
static int l_emu_pause(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.pause: session missing");
    }

    if (session->core_runtime) {
        (void)nes_core_pause(session->core_runtime, 1);
    }
    session->paused = 1;
    session->autorun_active = 0;
    session_sync_runtime(session);
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:resume() -> true.
 */
static int l_emu_resume(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.resume: session missing");
    }

    if (session->core_runtime) {
        (void)nes_core_pause(session->core_runtime, 0);
    }
    session->paused = 0;
    session->autorun_active = 1;
    session_sync_runtime(session);
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:init() -> true | nil, err.
 */
static int l_emu_init(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    int argc = host ? host->lua.gettop(L) : 0;
    int arg = 1;
    int64_t level = 0;
    char err[96];

    if (!session || !host) {
        return push_error(L, host, "nes.init: session missing");
    }
    if (!session->core_runtime) {
        return push_error(L, host, "nes.init: start nes before init");
    }

    if (argc >= 2 && host->lua.istable(L, 1)) {
        arg = 2;
    }
    if (argc >= arg && host->lua.isnumber(L, arg)) {
        level = host->lua.tointeger(L, arg);
    }
    if (level < 0) {
        level = 0;
    } else if (level > 8) {
        level = 8;
    }

    err[0] = '\0';
    if (!nes_core_prepare(session->core_runtime, 3000, (uint32_t)level, err, sizeof(err))) {
        set_error(session, err[0] ? err : "nes init failed");
        session_sync_runtime(session);
        return push_error(L, host, session->last_error);
    }

    session_sync_runtime(session);
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:step([frames]) -> true | nil, err.
 */
static int l_emu_step(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    int argc = host ? host->lua.gettop(L) : 0;
    int arg = 1;
    int64_t frames = 1;
    char err[96];

    if (!session || !host) {
        return push_error(L, host, "nes.step: session missing");
    }
    if (!session->core_runtime) {
        return push_error(L, host, "nes.step: start nes before stepping");
    }

    if (argc >= 2 && host->lua.istable(L, 1)) {
        arg = 2;
    }
    if (argc >= arg && host->lua.isnumber(L, arg)) {
        frames = host->lua.tointeger(L, arg);
    }
    if (frames < 1) {
        frames = 1;
    } else if (frames > 120) {
        frames = 120;
    }

    err[0] = '\0';
    if (!nes_core_step(session->core_runtime, (uint32_t)frames, err, sizeof(err))) {
        set_error(session, err[0] ? err : "nes step failed");
        session_sync_runtime(session);
        return push_error(L, host, session->last_error);
    }

    session->autorun_active = 0;
    session->paused = 1;
    session_sync_runtime(session);
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:reset() -> true | nil, err.
 */
static int l_emu_reset(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.reset: session missing");
    }

    if (!session_stop(session, 2000, 0)) {
        return push_error(L, host, session->last_error);
    }
    if (!session_start(session)) {
        return push_error(L, host, session->last_error);
    }
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:info() -> table.
 */
static int l_emu_info(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    if (!session || !host) {
        return push_error(L, host, "nes.info: session missing");
    }

    push_session_info(L, host, session);
    return 1;
}

/**
 * @brief Lua API: emu:input() -> table.
 */
static int l_emu_input(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    uint32_t mask = 0;
    uint32_t nes_mask = 0;
    if (!session || !host) {
        return push_error(L, host, "nes.input: session missing");
    }

    if (session->core_runtime && nes_core_input(session->core_runtime, &mask, &nes_mask)) {
        /* core bridge already holds the Lua-provided NES pad mask */
    } else {
        mask = sanitize_nes_mask(session->last_nes_mask);
        nes_mask = mask;
    }
    session->last_gamepad_mask = mask;
    session->last_nes_mask = nes_mask;

    host->lua.createtable(L, 0, 14);
    set_integer_field(L, host, "gamepad_mask", mask);
    set_integer_field(L, host, "mask", nes_mask);
    set_boolean_field(L, host, "a", (nes_mask & NES_PAD_A) != 0);
    set_boolean_field(L, host, "b", (nes_mask & NES_PAD_B) != 0);
    set_boolean_field(L, host, "select", (nes_mask & NES_PAD_SELECT) != 0);
    set_boolean_field(L, host, "start", (nes_mask & NES_PAD_START) != 0);
    set_boolean_field(L, host, "up", (nes_mask & NES_PAD_UP) != 0);
    set_boolean_field(L, host, "down", (nes_mask & NES_PAD_DOWN) != 0);
    set_boolean_field(L, host, "left", (nes_mask & NES_PAD_LEFT) != 0);
    set_boolean_field(L, host, "right", (nes_mask & NES_PAD_RIGHT) != 0);
    return 1;
}

/**
 * @brief Lua API: emu:set_input_mask(mask) -> true.
 */
static int l_emu_set_input_mask(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);
    int argc = host ? host->lua.gettop(L) : 0;
    int arg = 1;
    int64_t raw = 0;
    uint32_t mask = 0;

    if (!session || !host) {
        return push_error(L, host, "nes.set_input_mask: session missing");
    }

    if (argc >= 2 && host->lua.istable(L, 1)) {
        arg = 2;
    }
    raw = host->lua.checkinteger(L, arg);
    if (raw < 0) {
        raw = 0;
    }
    mask = sanitize_nes_mask((uint32_t)raw);
    session->last_gamepad_mask = mask;
    session->last_nes_mask = mask;
    if (session->core_runtime) {
        nes_core_set_input_mask(session->core_runtime, mask);
    }
    return push_true(L, host);
}

/**
 * @brief Lua API: emu:clear_input() -> true.
 */
static int l_emu_clear_input(lua_State *L)
{
    nes_session_t *session = session_from_lua(L);
    const module_host_api_v1 *host = session_host(session);

    if (!session || !host) {
        return push_error(L, host, "nes.clear_input: session missing");
    }

    session->last_gamepad_mask = 0;
    session->last_nes_mask = 0;
    if (session->core_runtime) {
        nes_core_set_input_mask(session->core_runtime, 0);
    }
    return push_true(L, host);
}

/**
 * @brief Push an emulator object table backed by the given session.
 */
static void push_emu_object(lua_State *L, const module_host_api_v1 *host, nes_session_t *session)
{
    host->lua.createtable(L, 0, 20);
    set_string_field(L, host, "VERSION", NES_VERSION);
    set_integer_field(L, host, "WIDTH", NES_FRAME_WIDTH);
    set_integer_field(L, host, "HEIGHT", NES_FRAME_HEIGHT);
    set_closure_field(L, host, "load", l_emu_load, session);
    set_closure_field(L, host, "start", l_emu_start, session);
    set_closure_field(L, host, "stop", l_emu_stop, session);
    set_closure_field(L, host, "pause", l_emu_pause, session);
    set_closure_field(L, host, "resume", l_emu_resume, session);
    set_closure_field(L, host, "init", l_emu_init, session);
    set_closure_field(L, host, "step", l_emu_step, session);
    set_closure_field(L, host, "reset", l_emu_reset, session);
    set_closure_field(L, host, "info", l_emu_info, session);
    set_closure_field(L, host, "input", l_emu_input, session);
    set_closure_field(L, host, "set_input_mask", l_emu_set_input_mask, session);
    set_closure_field(L, host, "clear_input", l_emu_clear_input, session);
}

/**
 * @brief Lua API: nes.create([opts]) -> emu | nil, err.
 */
static int l_create(lua_State *L)
{
    nes_module_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    nes_session_t *session = inst ? &inst->session : NULL;
    char rom_path[MODULE_PATH_MAX];

    if (!inst || !host || !session) {
        return push_error(L, host, "nes.create: instance missing");
    }
    session_sync_runtime(session);
    if (session->running) {
        return push_error(L, host, "nes.create: stop current session first");
    }

    session_release_runtime(session);
    session_set_defaults(session);
    session->owner = inst;
    rom_path[0] = '\0';

    if (host->lua.gettop(L) >= 1 && host->lua.istable(L, 1)) {
        apply_options(L, host, session, 1);
        (void)read_table_string(L, host, 1, "rom", rom_path, sizeof(rom_path));
    }

    if (rom_path[0] && !session_load_rom(session, rom_path)) {
        return push_error(L, host, session->last_error);
    }

    push_emu_object(L, host, session);
    return 1;
}

/**
 * @brief Export module metadata to ESP-ELFLoader.
 */
NES_MODULE_EXPORT const module_manifest_t *module_query_v1(void)
{
    return &s_manifest;
}

/**
 * @brief Create one module instance for a Lua state.
 */
NES_MODULE_EXPORT int32_t module_create_v1(const module_host_api_v1 *host,
                                           const module_open_info_t *info,
                                           void **out_instance)
{
    nes_module_instance_t *inst = NULL;
    if (!host || !out_instance || host->abi_version != MODULE_ABI_VERSION) {
        return MODULE_ERR_INVALID_ARG;
    }

    *out_instance = NULL;
    s_host = host;
    inst = (nes_module_instance_t *)host->heap.calloc(1,
                                                      sizeof(nes_module_instance_t),
                                                      MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!inst) {
        inst = (nes_module_instance_t *)host->heap.calloc(1, sizeof(nes_module_instance_t), MODULE_HEAP_DEFAULT);
    }
    if (!inst) {
        return MODULE_ERR_NO_MEMORY;
    }

    inst->host = host;
    inst->created_ms = host->time.millis ? host->time.millis() : 0;
    copy_text(inst->path, sizeof(inst->path), info ? info->path : "");
    inst->session.owner = inst;
    session_set_defaults(&inst->session);
    inst->session.owner = inst;

    if (host->serial.println) {
        host->serial.println("[nes.so] create");
    }

    *out_instance = inst;
    return MODULE_OK;
}

/**
 * @brief Open the Lua-facing `nes` table.
 */
NES_MODULE_EXPORT int32_t module_luaopen_v1(void *instance, lua_State *L)
{
    nes_module_instance_t *inst = (nes_module_instance_t *)instance;
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    if (!inst || !host || !L) {
        return MODULE_ERR_INVALID_ARG;
    }

    host->lua.createtable(L, 0, 24);
    set_string_field(L, host, "VERSION", NES_VERSION);
    set_boolean_field(L, host, "AUDIO", 0);
    set_boolean_field(L, host, "EMULATOR_CORE", 1);
    set_integer_field(L, host, "WIDTH", NES_FRAME_WIDTH);
    set_integer_field(L, host, "HEIGHT", NES_FRAME_HEIGHT);
    set_integer_field(L, host, "PAD_A", NES_PAD_A);
    set_integer_field(L, host, "PAD_B", NES_PAD_B);
    set_integer_field(L, host, "PAD_SELECT", NES_PAD_SELECT);
    set_integer_field(L, host, "PAD_START", NES_PAD_START);
    set_integer_field(L, host, "PAD_UP", NES_PAD_UP);
    set_integer_field(L, host, "PAD_DOWN", NES_PAD_DOWN);
    set_integer_field(L, host, "PAD_LEFT", NES_PAD_LEFT);
    set_integer_field(L, host, "PAD_RIGHT", NES_PAD_RIGHT);
    set_closure_field(L, host, "create", l_create, inst);
    set_closure_field(L, host, "read_header", l_read_header, inst);
    set_closure_field(L, host, "info", l_module_info, inst);
    return MODULE_OK;
}

/**
 * @brief Destroy a module instance and synchronously stop its task.
 */
NES_MODULE_EXPORT void module_destroy_v1(void *instance)
{
    nes_module_instance_t *inst = (nes_module_instance_t *)instance;
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    if (!inst || !host) {
        return;
    }

    (void)session_stop(&inst->session, 3000, 1);
    session_release_runtime(&inst->session);
    if (host->serial.println) {
        host->serial.println("[nes.so] destroy");
    }
    host->heap.free(inst);
}
