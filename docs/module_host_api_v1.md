# module_host_api_v1

`module_host_api_v1` 是固件传给动态模块的宿主能力表。`nes.so` 不直接链接固件里的文件系统、显示、任务、堆、Lua 等符号，而是在 `module_create_v1()` 里保存这张函数表，并通过它调用宿主能力。

ABI 定义在 [include/module_abi.h](../include/module_abi.h)。

## 模块生命周期

动态模块需要导出这些符号：

```c
const module_manifest_t *module_query_v1(void);

int32_t module_create_v1(const module_host_api_v1 *host,
                         const module_open_info_t *info,
                         void **out_instance);

int32_t module_luaopen_v1(void *instance, lua_State *L);

void module_destroy_v1(void *instance);
```

加载流程：

1. 固件加载 `.so` 后调用 `module_query_v1()` 读取 manifest。
2. 固件检查 `manifest->magic`、`manifest->abi_version` 和 `manifest->min_host_version`。
3. 固件调用 `module_create_v1()`，把 `module_host_api_v1` 指针传给模块。
4. Lua `require()` 动态库时，固件调用 `module_luaopen_v1()`，模块把 Lua table 压到栈顶。
5. 模块卸载或 Lua 状态释放时，固件调用 `module_destroy_v1()`。

## 基本约定

- `host->abi_version` 必须等于 `MODULE_ABI_VERSION`。
- 所有 API 结构体都有 `size` 字段，宿主扩展 ABI 时应保持向后兼容。
- 函数指针允许为空，模块调用前需要判断；为空通常表示宿主不支持该能力。
- 返回值使用 `MODULE_OK` 表示成功，负数为 `module_error_t`。
- 模块内分配的内存应通过 `host->heap.*` 释放，避免跨 allocator。
- 宿主传入的 `module_host_api_v1` 指针应在模块实例生命周期内保持有效。

## 子 API

`module_host_api_v1` 包含这些能力组：

```c
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
```

### serial

串口输出，用于日志：

```text
write / print / println / flush
```

### sd 和 file

`sd` 负责挂载状态、路径操作和打开文件；`file` 负责对已打开文件句柄读写、seek、size 和 close。

NES 模块当前使用：

```text
sd.begin
sd.open
file.read
file.seek
file.position
file.size_bytes
file.available
file.close
```

### display

显示接口以 surface 为单位。模块先 `acquire()` 独占显示，再通过 RGB565 DMA 推屏接口提交像素，结束时 `release()`。

stream 接口是 TFT_eSPI 风格的分块推屏语义：

```text
start_write     ~= startWrite()
push_image_dma  ~= pushImageDMA()
end_write       ~= dmaWait() / endWrite()
fill_screen     ~= fillScreen()
```

也就是说，ABI 名称直接贴近 TFT_eSPI 的调用形态，但仍然通过 host surface 隔离宿主对象；模块不直接持有 TFT_eSPI 指针。

NES 模块当前使用：

```text
width
height
acquire
start_write
push_image_dma
end_write
release
```

`start_write / push_image_dma / end_write` 用于 DMA 分块提交；NES 会使用双槽缓冲，并按可配置的 `transfer_rows` 分块提交给 host，提交给 host 的像素块必须是连续 RGB565 数据。

### audio

音频流接口。当前 NES 模块保留 ABI，但尚未接入音频输出。

### gamepad

通用手柄事件接口。当前示例 app 在 Lua 侧读取 `gamepad` 模块并映射 NES bit mask，`nes.so` 本身只接收 `emu:set_input_mask(mask)` 下发的 8-bit NES 输入。

### time

时间和延时接口：

```text
millis
micros
delay
yield
```

NES core 用它做帧率控制、停止等待和任务让步。

### heap

模块内存分配接口：

```text
malloc
calloc
realloc
free
free_size
largest_free_block
```

`module_heap_caps_t` 用于声明内存能力，例如 `MODULE_HEAP_INTERNAL`、`MODULE_HEAP_PSRAM`、`MODULE_HEAP_DMA`、`MODULE_HEAP_8BIT`。

### task

任务接口：

```text
create
remove
yield
delay
```

NES 模块会创建独立任务运行 core。建议宿主把该任务栈放在内部 RAM；如果任务栈落在 PSRAM，NES 模块会在状态里报告 `task_stack_psram = true`。

### lua

Lua C API 的宿主转发表。动态模块不直接链接 Lua 符号，而是通过 `host->lua.*` 创建 table、读取参数、压入返回值和注册 C closure。

## NES 模块最低需求

要运行当前 `nes.so`，宿主至少需要提供：

```text
host.sd.begin / open
host.file.read / seek / position / size_bytes / available / close
host.display.width / height / acquire / push_image_dma / release
host.time.millis / micros / delay
host.task.create / remove / yield / delay
host.heap.malloc / calloc / free / free_size / largest_free_block
host.serial.print / println
host.lua.*
```

推荐同时实现：

```text
host.display.start_write / push_image_dma / end_write
```

这样 NES 画面可以走 TFT_eSPI 风格的双缓冲 DMA 分块提交，减少同步推屏开销。
