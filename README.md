# Holocubic NES Dynamic Module

这是一个给 Clocteck Holocubic / cubic Lua 固件使用的 NES 动态模块。模块会被 ESP-ELFLoader 加载为 `nes.so`，Lua app 通过 `require("/sd/modules/nes.so")` 调用它。

本项目的 NES CPU、PPU、mapper 和渲染链路参考了 [Shim06/Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32)。Anemoia-ESP32 在 GitHub 上标注为 GNU GPL v3.0，本项目也采用 GPL-3.0 协议，见 [LICENSE](LICENSE)。

项目不包含任何商业游戏 ROM。
cubic Lua 说明在另外项目中可以参考

## 特性

- 以 `module_host_api_v1` 作为唯一宿主 ABI。
- 通过 ESP-ELFLoader 构建和加载 `.so`。
- Lua 侧提供 NodeMCU 风格 API：`nes.create()`、`emu:start()`、`emu:stop()`、`emu:set_input_mask()` 等。
- 显示输出使用 RGB565 分块 DMA/stream 推屏。
- 示例 Lua app 支持从 `/sd/nes` 扫描 ROM，并使用 Xbox BLE 手柄映射 NES 输入。

当前支持 mapper：`0, 1, 2, 3, 4, 7, 15, 69, 226`。

## 目录结构

```text
.
├── core/                  NES CPU/PPU/APU/cartridge/mapper 核心
├── main/                  ESP-ELFLoader 模块入口和 ESP-IDF component 配置
├── port/                  面向 NES core 的 Arduino 兼容 shim，底层转发到 host API
├── runtime/               C 接口到 C++ NES core 的运行时桥接
├── video/                 RGB565 显示输出封装
├── examples/
│   └── nes-gamepad.lua    Holocubic Lua 示例 app
├── include/
│   └── module_abi.h       动态模块 host ABI
├── config.h
├── debug.h
├── nes_config.h           NES 运行配置
├── nes_types.h
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## 外部依赖

构建需要：

- ESP-IDF，并已设置 `IDF_PATH`；
- `espressif/elf_loader` 组件，`main/idf_component.yml` 会声明该依赖；
- 与目标固件一致的 `module_abi.h`，仓库内已带一份当前 ABI；
- 目标固件需要实现动态模块加载、Lua `require()` 动态库加载、SD、文件、显示、任务、堆、时间和串口 host API。

`module_abi.h` 查找顺序：

1. 仓库内 `include/module_abi.h`；
2. 环境变量 `CUBICLUA_ROOT` 指向的 `$CUBICLUA_ROOT/src/dynmod/module_abi.h`；
3. 原工程内嵌路径 `../../../../src/dynmod/module_abi.h`；
4. CMake 参数 `-DMODULE_ABI_DIR=/path/to/src/dynmod`。

## 编译

PowerShell 示例：

```powershell
$env:CUBICLUA_ROOT="E:\path\to\cubic-develop"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

如果不使用 `CUBICLUA_ROOT`，直接传入 ABI 目录：

```powershell
idf.py -DMODULE_ABI_DIR="E:\path\to\cubic-develop\src\dynmod" build
```

需要在 menuconfig 中启用 ESP-ELFLoader 的 shared object 动态加载选项：

```text
CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT=y
```

成功后产物位于：

```text
build/nes.so
```

说明：`esp-elf-loader` 1.3.x 的 `project_so()` 默认只直接收集 C 源文件。本项目的 C++ NES core 会先编译进 `esp-idf/main/libmain.a`，再由根目录 `CMakeLists.txt` 通过 `ELF_LIBS` 链接进 `nes.so`。

## 部署和运行

把文件复制到 SD 卡：

```text
/sd/modules/nes.so
/sd/apps/nes-gamepad.lua
/sd/nes/your_game.nes
```

最小 Lua 用法：

```lua
local nes = require("/sd/modules/nes.so")

local header, err = nes.read_header("/sd/nes/demo.nes")
if not header then
  print(err)
  return
end

local emu, err = nes.create({
  rom = "/sd/nes/demo.nes",
  fps = 60,
  autorun = true,
  video = { x = 32, y = 0 },
})
if not emu then
  print(err)
  return
end

emu:start()
emu:set_input_mask(nes.PAD_START)
```

示例 app：

```lua
dofile("/sd/apps/nes-gamepad.lua")
```

示例 app 默认：

- 从 `/sd/nes` 扫描 `.nes` 文件；
- 从 `/sd/modules/nes.so` 加载模块；
- 使用 Xbox BLE 手柄；
- `HOME` 长按从游戏返回选择页，再长按退出 app。

## Lua API

模块级：

```lua
nes.VERSION
nes.WIDTH
nes.HEIGHT
nes.AUDIO
nes.EMULATOR_CORE
nes.PAD_A
nes.PAD_B
nes.PAD_SELECT
nes.PAD_START
nes.PAD_UP
nes.PAD_DOWN
nes.PAD_LEFT
nes.PAD_RIGHT
nes.read_header(path)
nes.create([opts])
nes.info()
```

`emu` 对象：

```lua
emu:load(path)
emu:start()
emu:stop()
emu:pause()
emu:resume()
emu:reset()
emu:init([level])
emu:step([frames])
emu:info()
emu:input()
emu:set_input_mask(mask)
emu:clear_input()
```

NES 手柄 bit mask：

```text
bit 0  A
bit 1  B
bit 2  SELECT
bit 3  START
bit 4  UP
bit 5  DOWN
bit 6  LEFT
bit 7  RIGHT
```

## Host API 使用范围

`nes.so` 当前使用这些 host API：

```text
host.sd.begin / open
host.file.read / seek / position / size_bytes / available / close
host.display.width / height / acquire / begin_stream / queue_rgb565 / end_stream / draw_rgb565 / release
host.time.millis / micros / delay
host.task.create / remove / yield / delay
host.heap.malloc / calloc / free / free_size / largest_free_block
host.serial.print / println
host.lua.*
```

输入由 Lua app 读取手柄状态后映射成 NES 8-bit mask，再通过 `emu:set_input_mask(mask)` 下发给 core。

## 当前限制

- 音频暂未接入，`nes.AUDIO == false`。
- 当前 `.so` 内只开放单个 NES 会话实例。
- NES 2.0 ROM 暂不支持。
- 性能和兼容性取决于宿主固件的显示 DMA、任务栈内存位置和 PSRAM/内部 RAM 分配策略。
- `module_abi.h` 必须和宿主固件完全匹配，否则可能加载失败或运行异常。

## 致谢

感谢 [Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) 对 ESP32 NES core、mapper 支持和性能优化方向的启发。本项目将相关思路整理为 Holocubic/cubic Lua 固件可动态加载的 `nes.so` 模块形态。
