# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目定位

Clocteck Holocubic / cubic Lua 固件的 **NES 模拟器动态模块**，不是独立固件。产物是一个 ELF 共享对象 `nes.so`，由宿主固件的 ESP-ELFLoader 在运行时加载，Lua app 通过 `require("/sd/modules/nes.so")` 使用。

关键含义：

- 模块**不链接**宿主固件的任何符号（文件系统、显示、任务、堆、Lua 都不链接），全部能力经 `module_host_api_v1` 函数表调用（`include/module_abi.h`，说明见 `docs/module_host_api_v1.md`）。
- 目标芯片 `esp32s3`（PSRAM 地址判定 `0x3C000000..0x3E000000` 写死在 `runtime/nes_core_bridge.cpp` 与 `port/nes_port.cpp`）。
- NES core 参考 Anemoia-ESP32，GPL-3.0。

## 构建

无 flash/monitor 流程——本仓库不产生可烧写固件，调试靠宿主固件串口里 `[nes.so]` 前缀日志（`host->serial.println`）。仓库内也没有测试/lint 配置。

```bash
# 可选：让 CMake 自动找到与宿主固件配套的 module_abi.h
export CUBICLUA_ROOT=/path/to/cubic_arduino/cubic-develop

idf.py set-target esp32s3
idf.py build          # 产物 build/nes.so
```

不用 `CUBICLUA_ROOT` 时直接指定 ABI 目录：

```bash
idf.py -DMODULE_ABI_DIR=/path/to/src/dynmod build
```

`module_abi.h` 查找顺序（`main/CMakeLists.txt`）：`-DMODULE_ABI_DIR` → `$CUBICLUA_ROOT/src/dynmod` → 仓库内 `include/` → `../../../../src/dynmod`。

构建要点：

- `CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT=y` 必须开（`sdkconfig.defaults` 已带）。关掉时 `project_so(nes)` 不创建 target，也就没有 `build/nes.so`。已有旧 `sdkconfig` 时 defaults 不生效，需删掉或在 `idf.py menuconfig` 里确认一次。
- 根 `CMakeLists.txt` 用 `set(COMPONENTS main espressif__elf_loader lwip)` 裁剪组件；`esp-elf-loader` 1.3.x 的 `project_so()` 只直接收 C 源文件，所以 C++ core 先编进 `esp-idf/main/libmain.a`，再由 `ELF_LIBS` 链进 `nes.so`。新增 C++ 源码要加到 `main/CMakeLists.txt` 的 `NES_CORE_SRCS`，不要指望 `project_so` 自动发现。
- core 用 `-O3 -fPIC -fvisibility=hidden -fdata-sections -ffunction-sections` + C++17。因为默认隐藏符号，四个 ABI 入口必须带 `NES_MODULE_EXPORT`（`main/nes_module.c`）。
- ABI 校验：`host->abi_version` 高 16 位需等于模块的 `MODULE_ABI_VERSION`（当前 `0x00020002`）且不低于它。`module_abi.h` 与宿主固件不一致会加载失败或跑飞。

部署：`build/nes.so` → `/sd/modules/nes.so`，`examples/nes-gamepad.lua` → `/sd/apps/`，ROM 放 `/sd/nes/`。

## 分层与边界

```
Lua app (examples/nes-gamepad.lua)
  │ nes.create / emu:start / emu:set_input_mask（示例实际只用这些）
  │ emu:read_audio  ← 仅 "lua" 音频后端需要，示例里没实现，故示例无声
main/nes_module.c        ABI 入口 + Lua 绑定 + session 状态/选项解析（纯 C）
  │ nes_core_* (runtime/nes_core_bridge.h，C 接口)
runtime/nes_core_bridge  NesCoreRuntime：任务生命周期、帧节拍、init 分级、状态汇总
  │
core/  bus → cpu6502 / ppu2C02 / apu2A03 / cartridge → mappers/
  │ 只依赖 port/ 提供的 Arduino 兼容层
port/  Arduino.h(String/File/类型) + SD.h + nes_port.* → 全部转成 host API
video/ audio/  RGB565 DMA 分块推屏 / APU 采样输出
```

边界纪律：

- `core/` 里不要出现真的 Arduino 或 ESP-IDF 头文件、`malloc`、`Serial`。用 `port/nes_port.h`（`nes_port_malloc/millis/file_*/log`）和 `port/Arduino.h` 的 `String`/`File`。
- PPU 只管生成像素（写 `ptr_display`），"向显示层借/还双缓冲 slot" 的逻辑集中在 `Bus::prepareRenderBuffer()` / `Bus::renderImage()`；不要让 PPU 直接碰 `NesVideoOut`。
- 输入不在模块里读手柄：Lua app 读 gamepad 后映射成 8-bit NES mask，经 `emu:set_input_mask()` 原子写入 `m_input_mask`。

## 帧循环与性能约束

改动这块前先读 `runtime/nes_core_bridge.cpp::taskLoop()` 和 `core/bus.cpp::Bus::clock()`。设备实测约 50fps，仍在优化中。

- `Bus::clock()` 一次跑完整一帧：按每 3 条 scanline 一组（`cpu.clock(113/114/114)`）交替推进 CPU/PPU，避免 341 dots 不被 3 整除带来的计数开销；随后 scanline 240、`setVBlank()` + `cpu.clock(2501)`、`clearVBlank()`。
- **隔帧渲染**：`nes_config.h` 的 `FRAMESKIP`（默认定义为 1，`bus.cpp` 里是 `#ifdef` 判断，即默认开启）让 `frame_latch` 每帧取反；跳过的那帧走 `ppu.fakeSpriteHit()` 只维持 sprite-0 hit 时序，不产像素。要关掉必须 `#undef`／改成 `#if`，仅把值改成 0 无效。
- 帧节拍：`next_frame_due += frame_us` 累加式，落后超过 4 帧就重置基准；提前则 `nes_port_delay()`，落后则 `host->task.yield()`。
- 推屏走 TFT_eSPI 风格：`startWrite / setAddrWindow / pushPixelsDMA / endWrite`。`video/nes_video_out.cpp` 用 **2 个 DMA slot**（`kDmaSlotCount`），每块 `width * transfer_rows * 2` 字节，从 `MODULE_HEAP_INTERNAL|DMA|8BIT` 分配；`endWrite`（即 dmaWait）只在整帧末尾调用一次，中途靠 slot 轮换与 PPU 生成重叠。宿主缺任一 stream API 就整帧失败。
- `transfer_rows` 默认 16，是显存占用与 DMA 次数的主要权衡旋钮（Lua 侧 `transfer_rows` 或 `video.transfer_rows`，clamp 到 1..240）。
- 热路径函数标 `MOD_IRAM_ATTR`（`port/Arduino.h`：`section(".mod_iram"), noinline, used`），由宿主 loader 放进 IRAM。给 `bus.cpp`/`cartridge.cpp` 加新热函数时保持这个标注。
- `main/nes_shims.c` 自带 `memcpy/memset/memmove`（带 `no-tree-loop-distribute-patterns`，否则编译器会把循环重写成对自身的调用）。动态模块不能依赖宿主 libc 符号，新增字符串/内存函数需求要按同样方式自带。
- 任何要交给 host 的函数指针都得先过 `nes_port_exec_ptr()`（ELF 数据段地址 → 可执行地址），参考 `taskEntry`/`apuTaskEntry` 的用法。
- core 任务栈若落在 PSRAM，`taskLoop()` 直接拒跑并 park，通过 `emu:info().task_stack_psram` 上报——这是宿主固件配置问题，不要在模块侧绕。

## 内存策略

- mapper 缓冲：`mapperAllocHot()`/`mapperAllocHotZeroed()` 先内部 RAM、不够回退 PSRAM；冷数据 `mapperAllocPsram()`/`mapperAllocPsramZeroed()` 直接 PSRAM（`core/mapper.cpp`，失败时会打印 internal/psram 剩余量）。
- `Cartridge` 构造时尝试把整份 PRG/CHR 预载进 PSRAM；若预载后 mapper 分配失败，会 `releasePreloadedRom()` 再建一次 mapper，退回"ROM 文件常驻打开 + 按需读 SD"模式（`core/cartridge.cpp`）。调 mapper 内存时注意这条回退路径别踩坏。
- 模块自身结构体优先 `MODULE_HEAP_INTERNAL|8BIT`，失败退 `MODULE_HEAP_DEFAULT`。跨 allocator 释放是禁忌：模块分配的内存只能用 `host->heap.free`。

## 音频 / APU

- `core/apu2A03.*` 是完整 2A03（2×pulse + triangle + noise + DMC，含 envelope/sweep/length/linear counter 与高通滤波），采样经 `setAudioSink()` 回调交给 `audio/nes_audio_out.*`。
- APU 在**独立任务** `nes_apu` 上跑（`startApuTask()`：4096 栈，跑在与 core 相反的核，优先级 core-1，每轮 busy 调 256 次 `apu.clock()`）。它与 core 任务共享 `Bus`，改 APU/CPU 交互时要考虑这层并发。
- 后端在 `NesAudioOut::begin()` 里一次选定（之后不会降级）：宿主 `host->audio.begin/write/end` 齐全且 `begin` 成功 → `"host"`；否则若 `lua_fallback`（默认开）→ `"lua"`（模块内环形队列，Lua 侧 `emu:read_audio(bytes)` 取 PCM）；都不成立 → `"none"` + `audio_error`。默认 22050Hz / 16bit / mono / 音量 80%。
- 音频失败不杀模拟：`taskLoop` 每帧 `consumeFailure()`，出错就记 `audio_error`，**先 `stopApuTask()` + 摘 sink 再 `m_audio.end()`**（否则 APU 任务可能正卡在 `write()` 里，而这边把 host stream 关了/把队列 free 了）。
- Lua 队列是 SPSC 但 tail 有两个写者（消费者 `read()` 推进；队列满时生产者也推进 tail 丢旧数据），两侧都必须用 **CAS** 提交 tail，普通 store 会把对方的更新覆盖掉。`read()` 还有读者计数 + `m_closing`，`freeQueue()` 先关门再等在途读者退出（上限 ~200ms）才 free——`read()` 在 Lua 任务、`freeQueue()` 在 core 任务，否则是 UAF。
- **格式实际写死**：APU 只产 22050Hz（`SAMPLE_RATE`）/ `int16_t[AUDIO_BUFFER_SIZE=256]` 单声道，链路无重采样、无格式转换。`audio.rate/bits/channels` 为兼容仍接受但**被显式忽略**（`apply_options()` 丢弃，`NesAudioOut::begin()` 再把 spec 收敛回真实格式，`write()` 按 `frames * sizeof(int16_t)` 算长度）。2026-07-29 前 `channels=2` 会让 `write()` 按 `frames * channels * 2` 读单声道 256 采样缓冲 → 越界读 512 字节，已修。要真支持多格式得先在 `audio/nes_audio_out.*` 加转换。
- **`volume = 0` 就是静音**（2026-07-29 修：原来 `nes_core_bridge.cpp` 把 0 当「未设置」改回 80）。默认值只在 `nes_module.c::session_set_defaults()` 里设，runtime 不再覆盖显式的 0。彻底关音频（省掉 APU 任务和队列）用 `audio.enabled = false`。

## 配置项在哪

- 编译期：`nes_config.h`（`FRAMESKIP`、`NES_SCREEN_SWAP_BYTES`、`NES_OVERSCAN_CROP_LEFT=8`、上电 RAM 模式、mapper226 兼容/trace 开关，以及 `nes::config::kDefault*` 默认值）。`config.h` 是留给 core 的空壳兼容头，板级配置一律走 host API。
- 运行期：**只有 `nes.create{...}` 收选项**（`apply_options()` 只被 `l_create()` 调用；`emu:start()` 不读任何 Lua 参数，`emu:load()` 只取路径，给它们传选项表会被静默忽略）。`main/nes_module.c::apply_options()` 解析并 clamp——`fps`(1..60，模块默认 30)、`transfer_rows`、`task_stack`(4096..32768)、`task_priority`(1..10)、`task_core`、`autorun`/`run`、`video.{x,y,transfer_rows}`、`audio.{enabled,lua_fallback,rate,bits,channels,volume,queue_bytes}`。不给 `x` 时按屏宽自动居中。

## 常见任务

**加 mapper**：`core/mappers/mapperNNN.{h,cpp}` 实现 `MapperVTable`（函数指针表 + `state` 指针，不是 C++ 虚函数），在 `core/cartridge.h` include、`Cartridge::createMapperInstance()` 的 switch 里加 case，并加入 `main/CMakeLists.txt` 的 `NES_CORE_SRCS`。若需要 scanline/A12/cycle 回调，还要看 `bindMapperFeatureFlags()`（按 mapper 号开启回调，避免热路径上无谓的间接调用）。当前支持 0/1/2/3/4/7/15/69/226。

**排查启动失败**：`emu:info()` 的 `stage`（阶段码定义在 `nes_core_bridge.cpp` 的 `CoreStage`：20/21/22 video、30/31 cartridge、40/41/42 bus、50~52 帧内、99 error）配合 `last_error`。`emu:init(level)` 可分级初始化到指定阶段：1=仅 video，2=+cartridge，3=+bus 挂载，4..7=bus reset 的子阶段，用于定位是哪一步挂的。

## 文档同步约定

`README.md`（特性 / Lua API / 音频 / Host API 使用范围 / 当前限制）与 `docs/module_host_api_v1.md` 的 `audio` 一节已于 2026-07-29 按 APU 实现校准（此前两处都还写着"音频未接入、`nes.AUDIO == false`"）。改音频行为、Lua API 或 host API 使用范围时要同时更新这两处；最容易写漂的三点是 `nes.AUDIO`、`emu:read_audio()` 的存在与语义、以及后端探测顺序（host → lua）。
