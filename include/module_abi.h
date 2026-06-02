#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;

typedef int (*module_lua_cfunction_t)(lua_State *L);

#define MODULE_ABI_VERSION 0x00010000u
#define MODULE_MANIFEST_MAGIC 0x414D4F44u /* "AMOD" */
#define MODULE_NAME_MAX 32u
#define MODULE_PATH_MAX 160u

#define MODULE_SYMBOL_QUERY_V1 "module_query_v1"
#define MODULE_SYMBOL_CREATE_V1 "module_create_v1"
#define MODULE_SYMBOL_LUAOPEN_V1 "module_luaopen_v1"
#define MODULE_SYMBOL_DESTROY_V1 "module_destroy_v1"

typedef enum module_error_t {
    MODULE_OK = 0,
    MODULE_ERR_FAILED = -1,
    MODULE_ERR_INVALID_ARG = -2,
    MODULE_ERR_NO_MEMORY = -3,
    MODULE_ERR_NOT_FOUND = -4,
    MODULE_ERR_UNSUPPORTED = -5,
    MODULE_ERR_BUSY = -6,
    MODULE_ERR_IO = -7,
    MODULE_ERR_BAD_STATE = -8,
    MODULE_ERR_VERSION = -9,
} module_error_t;

typedef enum module_heap_caps_t {
    MODULE_HEAP_DEFAULT = 0,
    MODULE_HEAP_INTERNAL = 1u << 0,
    MODULE_HEAP_PSRAM = 1u << 1,
    MODULE_HEAP_DMA = 1u << 2,
    MODULE_HEAP_EXEC = 1u << 3,
    MODULE_HEAP_8BIT = 1u << 4,
    MODULE_HEAP_32BIT = 1u << 5,
} module_heap_caps_t;

typedef enum module_file_mode_t {
    MODULE_FILE_READ = 1u << 0,
    MODULE_FILE_WRITE = 1u << 1,
    MODULE_FILE_APPEND = 1u << 2,
    MODULE_FILE_CREATE = 1u << 3,
    MODULE_FILE_TRUNC = 1u << 4,
} module_file_mode_t;

typedef enum module_seek_mode_t {
    MODULE_SEEK_SET = 0,
    MODULE_SEEK_CUR = 1,
    MODULE_SEEK_END = 2,
} module_seek_mode_t;

typedef enum module_pixel_format_t {
    MODULE_PIXEL_RGB565 = 1,
} module_pixel_format_t;

typedef enum module_gamepad_button_t {
    MODULE_GAMEPAD_A = 1u << 0,
    MODULE_GAMEPAD_B = 1u << 1,
    MODULE_GAMEPAD_SELECT = 1u << 2,
    MODULE_GAMEPAD_START = 1u << 3,
    MODULE_GAMEPAD_UP = 1u << 4,
    MODULE_GAMEPAD_DOWN = 1u << 5,
    MODULE_GAMEPAD_LEFT = 1u << 6,
    MODULE_GAMEPAD_RIGHT = 1u << 7,
    MODULE_GAMEPAD_X = 1u << 8,
    MODULE_GAMEPAD_Y = 1u << 9,
    MODULE_GAMEPAD_L = 1u << 10,
    MODULE_GAMEPAD_R = 1u << 11,
    MODULE_GAMEPAD_HOME = 1u << 12,
    MODULE_GAMEPAD_MENU = 1u << 13,
} module_gamepad_button_t;

typedef enum module_input_event_type_t {
    MODULE_INPUT_NONE = 0,
    MODULE_INPUT_GAMEPAD = 1,
    MODULE_INPUT_KEY = 2,
    MODULE_INPUT_TOUCH = 3,
} module_input_event_type_t;

typedef struct module_manifest_t {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t size;
    const char *name;
    const char *version;
    const char *description;
    uint32_t flags;
    uint32_t min_host_version;
} module_manifest_t;

typedef struct module_open_info_t {
    uint32_t size;
    const char *name;
    const char *path;
    const char *app_dir;
    const char *data_dir;
} module_open_info_t;

typedef struct module_file_stat_t {
    uint32_t size;
    uint8_t is_directory;
    uint8_t reserved[3];
    uint64_t file_size;
    uint64_t modified_time;
} module_file_stat_t;

typedef struct module_display_desc_t {
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint32_t pixel_format;
    uint32_t flags;
} module_display_desc_t;

typedef struct module_display_caps_t {
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint32_t pixel_formats;
    uint16_t max_borrow_rows;
    uint16_t reserved;
} module_display_caps_t;

typedef struct module_display_chunk_t {
    uint32_t size;
    void *pixels;
    uint16_t rows;
    uint16_t width;
    uint32_t pitch_bytes;
    uint32_t pixel_format;
} module_display_chunk_t;

typedef struct module_gamepad_event_t {
    uint32_t size;
    uint32_t type;
    uint8_t player;
    uint8_t pressed;
    uint16_t reserved;
    uint32_t mask;
    uint32_t changed;
} module_gamepad_event_t;

typedef struct module_audio_desc_t {
    uint32_t size;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t flags;
} module_audio_desc_t;

typedef struct module_serial_api_t {
    uint32_t size;
    int32_t (*write)(const void *data, size_t len);
    int32_t (*print)(const char *text);
    int32_t (*println)(const char *text);
    void (*flush)(void);
} module_serial_api_t;

typedef struct module_sd_api_t {
    uint32_t size;
    int32_t (*begin)(void);
    int32_t (*mounted)(void);
    const char *(*mount_point)(void);
    int32_t (*exists)(const char *path);
    int32_t (*mkdir)(const char *path);
    int32_t (*remove)(const char *path);
    int32_t (*rename)(const char *from, const char *to);
    int32_t (*open)(const char *path, uint32_t mode, void **out_file);
} module_sd_api_t;

typedef struct module_file_api_t {
    uint32_t size;
    int32_t (*close)(void *file);
    int32_t (*available)(void *file, size_t *out_available);
    int32_t (*read)(void *file, void *buf, size_t len, size_t *out_read);
    int32_t (*write)(void *file, const void *buf, size_t len, size_t *out_written);
    int32_t (*seek)(void *file, int64_t offset, uint32_t mode);
    int32_t (*position)(void *file, uint64_t *out_pos);
    int32_t (*size_bytes)(void *file, uint64_t *out_size);
    int32_t (*flush)(void *file);
    int32_t (*is_directory)(void *file, int32_t *out_is_directory);
} module_file_api_t;

typedef struct module_display_api_t {
    uint32_t size;
    int32_t (*width)(void);
    int32_t (*height)(void);
    int32_t (*get_caps)(module_display_caps_t *out_caps);
    int32_t (*acquire)(const char *owner, const module_display_desc_t *desc, void **out_surface);
    int32_t (*release)(void *surface);
    int32_t (*borrow_rows)(void *surface, uint16_t min_rows, module_display_chunk_t *out_chunk);
    int32_t (*submit_rows)(void *surface, const module_display_chunk_t *chunk,
                           int16_t x, int16_t y, uint16_t w, uint16_t h);
    int32_t (*draw_rgb565)(void *surface, int16_t x, int16_t y,
                           const uint16_t *pixels, uint16_t w, uint16_t h,
                           uint32_t pitch_bytes);
    int32_t (*fill_rgb565)(void *surface, uint16_t color);
    int32_t (*borrow_rows_slot)(void *surface, uint8_t slot_index, uint16_t min_rows,
                                module_display_chunk_t *out_chunk);
    int32_t (*begin_stream)(void *surface);
    int32_t (*queue_rgb565)(void *surface, const module_display_chunk_t *chunk,
                            int16_t x, int16_t y, uint16_t w, uint16_t h);
    int32_t (*end_stream)(void *surface);
} module_display_api_t;

typedef struct module_audio_api_t {
    uint32_t size;
    int32_t (*begin)(const module_audio_desc_t *desc, void **out_stream);
    int32_t (*write)(void *stream, const void *samples, size_t bytes, size_t *out_written);
    int32_t (*available)(void *stream, size_t *out_bytes);
    int32_t (*end)(void *stream);
} module_audio_api_t;

typedef struct module_gamepad_api_t {
    uint32_t size;
    int32_t (*poll_event)(module_gamepad_event_t *out_event, uint32_t timeout_ms);
    int32_t (*set_focus)(const char *owner, int32_t enabled);
} module_gamepad_api_t;

typedef struct module_time_api_t {
    uint32_t size;
    uint32_t (*millis)(void);
    uint64_t (*micros)(void);
    void (*delay)(uint32_t ms);
    void (*yield)(void);
} module_time_api_t;

typedef struct module_heap_api_t {
    uint32_t size;
    void *(*malloc)(size_t size, uint32_t caps);
    void *(*calloc)(size_t n, size_t size, uint32_t caps);
    void *(*realloc)(void *ptr, size_t size, uint32_t caps);
    void (*free)(void *ptr);
    size_t (*free_size)(uint32_t caps);
    size_t (*largest_free_block)(uint32_t caps);
} module_heap_api_t;

typedef struct module_task_api_t {
    uint32_t size;
    int32_t (*create)(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_bytes, uint32_t priority, int32_t core,
                      void **out_task);
    void (*remove)(void *task);
    void (*yield)(void);
    void (*delay)(uint32_t ms);
} module_task_api_t;

typedef struct module_lua_api_t {
    uint32_t size;
    int (*gettop)(lua_State *L);
    void (*settop)(lua_State *L, int idx);
    int (*type)(lua_State *L, int idx);
    int (*istable)(lua_State *L, int idx);
    int (*isnil)(lua_State *L, int idx);
    int (*isnumber)(lua_State *L, int idx);
    int (*isstring)(lua_State *L, int idx);
    int (*toboolean)(lua_State *L, int idx);
    int64_t (*tointeger)(lua_State *L, int idx);
    double (*tonumber)(lua_State *L, int idx);
    const char *(*tostring)(lua_State *L, int idx);
    int64_t (*checkinteger)(lua_State *L, int idx);
    double (*checknumber)(lua_State *L, int idx);
    const char *(*checkstring)(lua_State *L, int idx);
    void *(*touserdata)(lua_State *L, int idx);
    void (*pushnil)(lua_State *L);
    void (*pushboolean)(lua_State *L, int value);
    void (*pushinteger)(lua_State *L, int64_t value);
    void (*pushnumber)(lua_State *L, double value);
    void (*pushstring)(lua_State *L, const char *text);
    void (*pushlightuserdata)(lua_State *L, void *ptr);
    void (*pushcfunction)(lua_State *L, module_lua_cfunction_t fn);
    void (*pushcclosure)(lua_State *L, module_lua_cfunction_t fn, int nup);
    void (*pushvalue)(lua_State *L, int idx);
    void (*newtable)(lua_State *L);
    void (*createtable)(lua_State *L, int narr, int nrec);
    void (*getfield)(lua_State *L, int idx, const char *key);
    void (*setfield)(lua_State *L, int idx, const char *key);
    void (*getglobal)(lua_State *L, const char *name);
    void (*setglobal)(lua_State *L, const char *name);
    int (*registry_ref)(lua_State *L);
    void (*registry_unref)(lua_State *L, int ref);
    void (*registry_rawgeti)(lua_State *L, int ref);
    int (*upvalue_index)(int n);
    int (*error)(lua_State *L, const char *msg);
} module_lua_api_t;

typedef struct module_host_api_v1 {
    uint32_t abi_version;
    uint32_t size;
    module_serial_api_t serial;
    module_sd_api_t sd;
    module_file_api_t file;
    module_display_api_t display;
    module_audio_api_t audio;
    module_gamepad_api_t gamepad;
    module_time_api_t time;
    module_heap_api_t heap;
    module_task_api_t task;
    module_lua_api_t lua;
} module_host_api_v1;

typedef const module_manifest_t *(*module_query_v1_fn)(void);
typedef int32_t (*module_create_v1_fn)(const module_host_api_v1 *host,
                                       const module_open_info_t *info,
                                       void **out_instance);
typedef int32_t (*module_luaopen_v1_fn)(void *instance, lua_State *L);
typedef void (*module_destroy_v1_fn)(void *instance);

#ifdef __cplusplus
}
#endif
