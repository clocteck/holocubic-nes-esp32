# Holocubic NES Dynamic Module

这是一个给 Clocteck Holocubic / cubic Lua 固件使用的 NES 动态模块。模块会被 ESP-ELFLoader 加载为 `nes.so`，Lua app 通过 `require("/sd/modules/nes.so")` 调用它。
只能在Clocteck Holocubic 设备上运行，可以参考视频 https://www.bilibili.com/video/BV1Jv5v6TED9。


## 特性

- 以 `module_host_api_v1` 作为唯一宿主 ABI。
- 通过 ESP-ELFLoader 构建和加载 `.so`。
- 示例 Lua app 支持从 `/sd/nes` 扫描 ROM，并使用 Xbox BLE 手柄映射 NES 输入。
- 在设备上可以跑到50帧左右
- 当前支持 mapper：`0, 1, 2, 3, 4, 7, 15, 69, 226`。

## 目录结构

```text
.
├── core/                  NES CPU/PPU/APU/cartridge/mapper 核心
├── main/                  ESP-ELFLoader 模块入口和 ESP-IDF component 配置
├── port/                  面向 NES core 的 Arduino 兼容接口
├── runtime/               C 接口到 C++ NES core 的运行时桥接
├── video/                 RGB565 显示输出封装
├── docs/
│   └── module_host_api_v1.md
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


`module_abi.h` 查找顺序：

1. CMake 参数 `-DMODULE_ABI_DIR=/path/to/src/dynmod`；
2. 环境变量 `CUBICLUA_ROOT` 指向的 `$CUBICLUA_ROOT/src/dynmod/module_abi.h`；
3. 仓库内 `include/module_abi.h`；
4. 原工程内嵌路径 `../../../../src/dynmod/module_abi.h`。

## 编译

PowerShell 示例：

```powershell
Set-Location "E:\cubicsrc\开源APP\nes"
$env:CUBICLUA_ROOT="E:\cubicsrc\cubic_lua\cubic_arduino\cubic-develop"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

如果不使用 `CUBICLUA_ROOT`，直接传入 ABI 目录：

```powershell
Set-Location "E:\cubicsrc\开源APP\nes"
idf.py "-DMODULE_ABI_DIR=E:\cubicsrc\cubic_lua\cubic_arduino\cubic-develop\src\dynmod" build
```

需要在 menuconfig 中确认 ESP-ELFLoader 的 shared object 动态加载选项已启用：

```text
CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT=y
```

仓库的 `sdkconfig.defaults` 已默认启用这个选项；如果本地已有旧 `sdkconfig`，仍需要在 menuconfig 中确认一次。如果这个选项关闭，`project_so(nes)` 不会创建 `so` target，也不会生成 `build/nes.so`。

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
host.display.width / height / acquire / start_write / push_image_dma / end_write / release
host.time.millis / micros / delay
host.task.create / remove / yield / delay
host.heap.malloc / calloc / free / free_size / largest_free_block
host.serial.print / println
host.lua.*
```

输入由 Lua app 读取手柄状态后映射成 NES 8-bit mask，再通过 `emu:set_input_mask(mask)` 下发给 core。

其中 `start_write / push_image_dma / end_write` 是 TFT_eSPI 风格的 DMA 分块推屏语义。`module_host_api_v1` 的完整说明见 [docs/module_host_api_v1.md](docs/module_host_api_v1.md)。

## 当前限制

- 音频暂未接入，`nes.AUDIO == false`。
- 当前 `.so` 内只开放单个 NES 会话实例。
- NES 2.0 ROM 暂不支持。
- 性能和兼容性取决于宿主固件的显示 DMA、任务栈内存位置和 PSRAM/内部 RAM 分配策略。
- `module_abi.h` 必须和宿主固件完全匹配，否则可能加载失败或运行异常。

## 致谢

感谢 [Anemoia-ESP32](https://github.com/Shim06/Anemoia-ESP32) 对 ESP32 NES core、mapper 支持和性能优化方向的启发。本项目将相关思路整理为 Holocubic/cubic Lua 固件可动态加载的 `nes.so` 模块形态。本项目的 NES CPU、PPU、mapper 和渲染链路参考了 Anemoia-ESP32 ，本项目也采用 GPL-3.0 协议，见 [LICENSE](LICENSE)。
