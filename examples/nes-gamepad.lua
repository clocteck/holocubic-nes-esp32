if _G.NES_GAME_APP and _G.NES_GAME_APP.shutdown then
  pcall(function()
    _G.NES_GAME_APP.shutdown()
  end)
end

NES_GAME_APP = {}
local APP = NES_GAME_APP

-- 这个入口文件固定服务 Xbox BLE 手柄，不再保留网页手柄/双模式切换分支。
APP.VERSION = "2026-06-01-nes-gamepad-dynmod-v1"
APP.MODULE_PATH = "/sd/modules/nes.so"
APP.nes_mod = nil
APP.nes_emu = nil
APP.nes_load_error = nil

-- 固件里可能仍注册了内置 `nes` 全局；本 app 只使用这里的局部适配器。
local function create_dynmod_nes_adapter()
  local adapter = {
    PLAYER_1 = 1,
    PLAYER_2 = 2,
    BTN_A = 0x01,
    BTN_B = 0x02,
    BTN_SELECT = 0x04,
    BTN_START = 0x08,
    BTN_UP = 0x10,
    BTN_DOWN = 0x20,
    BTN_LEFT = 0x40,
    BTN_RIGHT = 0x80,
    input = {},
  }

  -- 模块加载成功后用 `.so` 导出的 PAD 常量覆盖本地默认值，避免两边位图漂移。
  local function apply_module_constants(mod)
    if type(mod) ~= "table" then
      return
    end

    adapter.BTN_A = tonumber(mod.PAD_A) or adapter.BTN_A
    adapter.BTN_B = tonumber(mod.PAD_B) or adapter.BTN_B
    adapter.BTN_SELECT = tonumber(mod.PAD_SELECT) or adapter.BTN_SELECT
    adapter.BTN_START = tonumber(mod.PAD_START) or adapter.BTN_START
    adapter.BTN_UP = tonumber(mod.PAD_UP) or adapter.BTN_UP
    adapter.BTN_DOWN = tonumber(mod.PAD_DOWN) or adapter.BTN_DOWN
    adapter.BTN_LEFT = tonumber(mod.PAD_LEFT) or adapter.BTN_LEFT
    adapter.BTN_RIGHT = tonumber(mod.PAD_RIGHT) or adapter.BTN_RIGHT
  end

  -- 只从固定路径显式加载动态模块，不回退到固件内置 `nes` 全局。
  local function require_module()
    if APP.nes_mod then
      return APP.nes_mod
    end

    local ok, mod = pcall(require, APP.MODULE_PATH)
    if not ok or type(mod) ~= "table" then
      APP.nes_load_error = "require " .. tostring(APP.MODULE_PATH) .. " failed: " .. tostring(mod)
      return nil, APP.nes_load_error
    end

    APP.nes_mod = mod
    APP.nes_load_error = nil
    apply_module_constants(mod)
    print("[nes_game] dynmod loaded", APP.MODULE_PATH, "VERSION=" .. tostring(mod.VERSION))
    return mod
  end

  -- 把 `.so` 的 emu:info() 映射成旧 app 需要的 nes.state() 形状。
  function adapter.state()
    local emu = APP.nes_emu
    if emu and emu.info then
      local ok, info = pcall(function()
        return emu:info()
      end)
      if ok and type(info) == "table" then
        return {
          running = info.running and true or false,
          loaded = info.loaded and true or false,
          paused = info.paused and true or false,
          rendered_frames = tonumber(info.frames) or 0,
          target_fps = tonumber(info.fps) or 0,
          rom_path = tostring(info.rom or ""),
          last_error = tostring(info.last_error or ""),
          gamepad_mask = tonumber(info.gamepad_mask) or 0,
          pad_mask = tonumber(info.pad_mask) or 0,
          display_stream_supported = info.display_stream_supported and true or false,
          display_stream_active = info.display_stream_active and true or false,
          display_stream_slots = tonumber(info.display_stream_slots) or 0,
          display_stream_queued = tonumber(info.display_stream_queued) or 0,
          display_async_supported = info.display_async_supported and true or false,
          display_async_active = info.display_async_active and true or false,
          display_async_slots = tonumber(info.display_async_slots) or 0,
          dynmod = true,
        }
      end
    end

    return {
      running = false,
      loaded = false,
      paused = false,
      rendered_frames = 0,
      target_fps = 0,
      last_error = APP.nes_load_error,
      dynmod = true,
    }
  end

  -- 保持旧的 nes.start(path, opts) 入口，底层实际创建并启动 `.so` emu 对象。
  function adapter.start(path, options)
    local mod, load_err = require_module()
    if not mod then
      return nil, load_err
    end

    if APP.nes_emu and APP.nes_emu.stop then
      pcall(function()
        APP.nes_emu:stop()
      end)
      APP.nes_emu = nil
    end

    if type(mod.create) ~= "function" then
      return nil, "nes dynmod missing create()"
    end

    local fps = tonumber(options and (options.target_fps or options.fps)) or 60
    local opts = {
      rom = path,
      fps = fps,
      autorun = true,
      video = { x = 32, y = 0 },
      task_stack = 12288,
      task_priority = 3,
      task_core = 1,
    }

    local ok_create, emu, create_err = pcall(function()
      return mod.create(opts)
    end)
    if not ok_create then
      return nil, emu
    end
    if not emu then
      return nil, create_err or "nes dynmod create failed"
    end

    APP.nes_emu = emu
    if type(emu.start) ~= "function" then
      APP.nes_emu = nil
      return nil, "nes dynmod missing emu:start()"
    end

    local ok_start_call, ok_start, start_err = pcall(function()
      return emu:start()
    end)
    if not ok_start_call or not ok_start then
      APP.nes_emu = nil
      pcall(function()
        emu:stop()
      end)
      return nil, (ok_start_call and start_err or ok_start) or "nes dynmod start failed"
    end

    return true
  end

  -- 停止 `.so` 后台任务并释放当前 emu 引用，返回选择页时会走这里。
  function adapter.stop()
    local emu = APP.nes_emu
    APP.nes_emu = nil
    if not emu or not emu.stop then
      return true
    end

    local ok, result, err = pcall(function()
      return emu:stop()
    end)
    if not ok then
      return nil, result
    end
    if result == nil and err ~= nil then
      return nil, err
    end
    return result ~= nil and result or true
  end

  -- Lua 负责把手柄状态映射成 NES 位图，`.so` 只接收最终 mask。
  function adapter.input.set_mask(_player, mask)
    local emu = APP.nes_emu
    if not emu then
      return true
    end
    if type(emu.set_input_mask) ~= "function" then
      return nil, "nes dynmod missing emu:set_input_mask()"
    end
    return emu:set_input_mask(tonumber(mask) or 0)
  end

  function adapter.input.clear(_player)
    local emu = APP.nes_emu
    if not emu then
      return true
    end
    if type(emu.clear_input) == "function" then
      return emu:clear_input()
    end
    if type(emu.set_input_mask) == "function" then
      return emu:set_input_mask(0)
    end
    return nil, "nes dynmod missing input clear api"
  end

  return adapter
end

local nes = create_dynmod_nes_adapter()
APP.PLAYER = nes.PLAYER_1
APP.ROM_DIR = "/sd/nes"
APP.PREFERRED_ROM_PATH = rawget(_G, "NES_ROM_PATH")

APP.ui = {}
APP.roms = {}
APP.rom_meta_cache = {}
APP.selected_index = 0
APP.rom_path = nil
APP.screen_mode = "boot"
APP.is_shutting_down = false
APP.repeat_left = 0
APP.repeat_right = 0
APP.rom_font_handle = 0
APP.rom_font_path = nil
APP.rom_font_attempted = false
APP.last_open_error_path = nil
APP.last_open_error_text = nil
APP.local_mask = 0
APP.applied_mask = -1
APP.last_status = "booting"
APP.gamepad_connected = false
APP.gamepad_started = false
APP.gamepad_prev_buttons = 0
APP.gamepad_prev_nav = 0
APP.GAMEPAD_AXIS_THRESHOLD = 0.46
APP.GAMEPAD_NAV_LEFT = (1 << 0)
APP.GAMEPAD_NAV_RIGHT = (1 << 1)
APP.GAMEPAD_NAV_CONFIRM = (1 << 2)
APP.GAMEPAD_NAV_BACK = (1 << 3)
APP.GAMEPAD_NAV_RESCAN = (1 << 4)

local MAIN_STYLE = LV_PART_MAIN | LV_STATE_DEFAULT

-- 统一把文本转成字符串，避免 nil 文本把 UI 或日志更新打断。
local function text_or(value, fallback)
  if value == nil or value == "" then
    return fallback or ""
  end
  return tostring(value)
end

-- 从路径里提取最后一段文件名，避免右侧信息区展示整串长路径时过度拥挤。
local function basename_from_path(path)
  local s = tostring(path or "")
  local name = s:match("([^/]+)$")
  return text_or(name, s)
end

-- 右侧信息面板空间固定，因此这里统一把超长字符串裁成可控长度，避免文字压到下一个区域。
local function clip_text(text, max_len)
  local s = tostring(text or "")
  local limit = tonumber(max_len) or 0
  if limit <= 0 or #s <= limit then
    return s
  end

  if limit <= 3 then
    return s:sub(1, limit)
  end

  return s:sub(1, limit - 3) .. "..."
end

-- 错误信息只在右下角一小块展示，因此统一压成短文本，避免多行错误把布局撑乱。
local function compact_error_text(text)
  local s = tostring(text or "")
  if s == "" then
    return ""
  end

  s = s:gsub("[\r\n]+", " / ")
  return clip_text(s, 42)
end

-- 选择框里主要展示 ROM 名称；这里去掉 `.nes` 后缀，让可见字符尽量留给真正的标题。
local function display_rom_name(name)
  local s = tostring(name or "")
  s = s:gsub("%.nes$", "")
  s = s:gsub("%.NES$", "")
  if s == "" then
    return "-"
  end
  return s
end

-- ROM 文件名可能包含中文，优先加载外部字库；若加载失败，则退回内置 18 号字体继续工作。
local function ensure_rom_font_loaded()
  if type(APP.rom_font_handle) == "number" and APP.rom_font_handle > 0 then
    return APP.rom_font_handle
  end
  if APP.rom_font_attempted then
    return 0
  end

  if not lv_font_load then
    return 0
  end

  APP.rom_font_attempted = true
  local candidates = {
    { tag = "sd_apps_font", path = "/sd/apps/font/16chinese.bin" },
  }

  local chosen_handle = 0
  local chosen_path = nil

  for i = 1, #candidates do
    local item = candidates[i]
    local ok, handle = pcall(function()
      return lv_font_load(item.path)
    end)
    if ok and type(handle) == "number" and handle > 0 then
      if chosen_handle <= 0 then
        chosen_handle = handle
        chosen_path = item.path
      elseif lv_font_free then
        pcall(function()
          lv_font_free(handle)
        end)
      end
    end
  end

  if chosen_handle > 0 then
    APP.rom_font_handle = chosen_handle
    APP.rom_font_path = chosen_path
    return chosen_handle
  end

  print("[nes_game] lv_font_load failed", "/sd/apps/font/16chinese.bin")
  APP.rom_font_handle = 0
  APP.rom_font_path = nil
  return 0
end

-- 选择框里的标题统一通过这里拿字体引用，避免每个标签都重复写回退逻辑。
local function get_rom_font_ref()
  local handle = ensure_rom_font_loaded()
  if handle and handle > 0 then
    return handle
  end
  return 16
end

-- `lv_clear()` 会把底层 font handle 一并清掉；这里单独重置 Lua 侧缓存，避免继续拿旧句柄复用。
local function reset_rom_font_state()
  APP.rom_font_handle = 0
  APP.rom_font_path = nil
  APP.rom_font_attempted = false
end

-- 字体句柄是 UI 侧资源，脚本退出时主动释放，避免多次热重载后资源一直累积。
local function free_rom_font()
  local handle = tonumber(APP.rom_font_handle) or 0
  if handle > 0 and lv_font_free then
    pcall(function()
      lv_font_free(handle)
    end)
  end
  reset_rom_font_state()
end

-- 目录扫描同样做保护，避免 SD 未挂载时把脚本直接打崩。
local function safe_listdir(path)
  if not path or path == "" or not file or not file.listdir then
    return {}
  end

  local ok, entries = pcall(function()
    return file.listdir(path)
  end)

  if not ok or type(entries) ~= "table" then
    return {}
  end

  return entries
end

-- 统一识别 `.nes` 后缀，大小写都接受，方便兼容不同拷贝来源的 ROM 文件名。
local function is_nes_rom_name(name)
  local s = tostring(name or "")
  return s:lower():match("%.nes$") ~= nil
end

-- 从目录项里补齐完整路径，优先使用底层已经给出的 `entry.path`。
local function entry_to_path(dir_path, entry)
  if not entry then
    return nil
  end

  if entry.path and entry.path ~= "" then
    return entry.path
  end

  local name = tostring(entry.name or "")
  if name == "" then
    return nil
  end

  return tostring(dir_path or "") .. "/" .. name
end

-- 只从 `/sd/nes` 扫描 ROM 列表，并按文件名排序，保证左右切换时顺序稳定。
local function scan_roms_in_dir(dir_path)
  local entries = safe_listdir(dir_path)
  local roms = {}

  for _, entry in ipairs(entries) do
    if entry and (not entry.is_dir) and is_nes_rom_name(entry.name) then
      local path = entry_to_path(dir_path, entry)
      if path and path ~= "" then
        roms[#roms + 1] = {
          name = tostring(entry.name or ""),
          path = path,
        }
      end
    end
  end

  table.sort(roms, function(a, b)
    return a.name:lower() < b.name:lower()
  end)

  return roms
end

-- 按完整路径匹配当前 ROM，便于切回选择页后仍然停在刚才那一项。
local function find_rom_index_by_path(roms, rom_path)
  local target = tostring(rom_path or "")
  if target == "" then
    return nil
  end

  for index, item in ipairs(roms or {}) do
    if item and item.path == target then
      return index
    end
  end

  return nil
end

-- 统一把 ROM 下标收敛到合法范围，避免目录内容变化后旧索引越界。
local function clamp_rom_index(roms, index)
  local count = #(roms or {})
  if count <= 0 then
    return 0
  end

  local value = tonumber(index) or 1
  value = math.floor(value)
  if value < 1 then
    value = 1
  elseif value > count then
    value = count
  end

  return value
end

-- 环形移动选择项，左右切换时无须特殊判断头尾。
local function wrap_rom_index(index, count)
  local value = tonumber(index) or 1
  value = math.floor(value)
  if count <= 0 then
    return 0
  end

  while value < 1 do
    value = value + count
  end

  while value > count do
    value = value - count
  end

  return value
end

-- 读取当前选中的 ROM 记录，供 UI 和启动逻辑共用。
local function get_selected_rom()
  local index = tonumber(APP.selected_index) or 0
  if index < 1 or index > #APP.roms then
    return nil
  end
  return APP.roms[index]
end

-- 重新扫描 `/sd/nes`，并尽量保留当前或显式指定的选中项。
local function refresh_rom_catalog(preferred_path)
  local roms = scan_roms_in_dir(APP.ROM_DIR)
  APP.roms = roms

  if #roms <= 0 then
    APP.selected_index = 0
    APP.rom_path = nil
    return false
  end

  local wanted_path = preferred_path
  if (not wanted_path or wanted_path == "") and APP.rom_path and APP.rom_path ~= "" then
    wanted_path = APP.rom_path
  end
  if (not wanted_path or wanted_path == "") and APP.PREFERRED_ROM_PATH and APP.PREFERRED_ROM_PATH ~= "" then
    wanted_path = APP.PREFERRED_ROM_PATH
  end

  local selected = find_rom_index_by_path(roms, wanted_path)
  if not selected then
    selected = clamp_rom_index(roms, APP.selected_index)
  end
  if selected <= 0 then
    selected = 1
  end

  APP.selected_index = selected
  APP.rom_path = roms[selected].path
  return true
end

-- 左右切换 ROM 时统一更新选中项，保持 `APP.rom_path` 和索引同步。
local function move_selected_rom(step)
  local count = #APP.roms
  if count <= 0 then
    return false
  end

  APP.selected_index = wrap_rom_index((APP.selected_index or 1) + (step or 0), count)
  local rom = get_selected_rom()
  APP.rom_path = rom and rom.path or nil
  return rom ~= nil
end

-- 把 mapper id 解释成尽量简洁的说明，右侧信息区只保留最关键的类型信息。
local function mapper_text(mapper_id)
  local known = {
    [0] = "Mapper 0  NROM",
    [1] = "Mapper 1  MMC1",
    [2] = "Mapper 2  UxROM",
    [3] = "Mapper 3  CNROM",
  }

  if known[mapper_id] then
    return known[mapper_id]
  end

  return "Mapper " .. tostring(mapper_id)
end

-- 镜像模式这里只保留短标签，方便在窄右栏里稳定展示。
local function mirroring_text(mode)
  local known = {
    [0] = "Mirror H",
    [1] = "Mirror V",
    [2] = "Mirror S-L",
    [3] = "Mirror S-U",
    [4] = "Mirror 4SCR",
  }

  return known[mode] or "Mirror -"
end

-- 与核心里的 iNES 解析保持同一套规则，这样选择页看到的 mapper 能尽量和真正启动结果一致。
local function read_rom_meta(path)
  local meta = {
    path = text_or(path, ""),
    mapper_id = nil,
    mapper_text = "-",
    mirror_text = "-",
    prg_text = "-",
    chr_text = "-",
    error = nil,
  }

  if meta.path == "" then
    meta.error = "rom path missing"
    return meta
  end

  if not file or not file.open then
    meta.error = "file api unavailable"
    return meta
  end

  local fd = file.open(meta.path, "r")
  if not fd then
    meta.error = "open rom failed"
    return meta
  end

  local header = fd:read(16)
  pcall(function()
    fd:close()
  end)

  if type(header) ~= "string" or #header < 16 then
    meta.error = "rom header unreadable"
    return meta
  end

  local b = {string.byte(header, 1, 16)}
  if b[1] ~= 0x4E or b[2] ~= 0x45 or b[3] ~= 0x53 or b[4] ~= 0x1A then
    meta.error = "invalid iNES header"
    return meta
  end

  local flags6 = b[7] or 0
  local flags7 = b[8] or 0

  if (flags7 & 0x0C) == 0x08 then
    meta.error = "nes2.0 unsupported"
    return meta
  end

  local zero_padding = (b[13] or 0) == 0 and
    (b[14] or 0) == 0 and
    (b[15] or 0) == 0 and
    (b[16] or 0) == 0

  local mapper_upper = flags7 & 0xF0
  if (flags7 & 0x0C) == 0 and not zero_padding then
    mapper_upper = 0
  end

  local mapper_id = ((flags6 >> 4) | mapper_upper)
  local mirror_mode = 0
  if (flags6 & 0x08) ~= 0 then
    mirror_mode = 4
  elseif (flags6 & 0x01) ~= 0 then
    mirror_mode = 1
  else
    mirror_mode = 0
  end

  meta.mapper_id = mapper_id
  meta.mapper_text = mapper_text(mapper_id)
  meta.mirror_text = mirroring_text(mirror_mode)
  meta.prg_text = tostring(b[5] or 0) .. "x16K PRG"
  meta.chr_text = tostring(b[6] or 0) .. "x8K CHR"
  return meta
end

-- 选择页会频繁左右切换，因此把 ROM 头解析结果缓存起来，避免每次切换都重新打开文件。
local function get_rom_meta(path)
  local rom_path = tostring(path or "")
  if rom_path == "" then
    return read_rom_meta(rom_path)
  end

  local cached = APP.rom_meta_cache[rom_path]
  if cached then
    return cached
  end

  local meta = read_rom_meta(rom_path)
  APP.rom_meta_cache[rom_path] = meta
  return meta
end

-- 统一把输入 mask 约束在 0..255，避免手柄回调传入异常值时污染底层位图。
local function sanitize_mask(value)
  local n = 0

  if type(value) == "number" then
    n = value
  else
    local text = tostring(value or "")
    n = tonumber(text) or 0
  end

  n = math.floor(n)
  if n < 0 then
    n = 0
  elseif n > 255 then
    n = 255
  end

  return n
end

-- 统一设置纯色面板样式，避免在多个对象上反复散落同类样式代码。
local function apply_panel_style(obj, color)
  if not obj then
    return
  end

  lv_obj_set_style_bg_color(obj, color, MAIN_STYLE)
  lv_obj_set_style_bg_opa(obj, 255, MAIN_STYLE)
  lv_obj_set_style_border_width(obj, 0, MAIN_STYLE)
  lv_obj_set_style_radius(obj, 0, MAIN_STYLE)
  lv_obj_set_style_pad_all(obj, 0, MAIN_STYLE)
end

-- 统一做卡片式深色面板样式；支持轻微渐变和阴影，让层级更清楚但仍保持克制。
local function apply_card_style(obj, bg_color, border_color, radius, grad_color, shadow_color)
  if not obj then
    return
  end

  apply_panel_style(obj, bg_color)
  lv_obj_set_style_radius(obj, radius or 16, MAIN_STYLE)
  lv_obj_set_style_border_width(obj, 1, MAIN_STYLE)
  lv_obj_set_style_border_color(obj, border_color or 0x23476C, MAIN_STYLE)
  lv_obj_set_style_border_opa(obj, 255, MAIN_STYLE)
  lv_obj_set_style_bg_grad_color(obj, grad_color or bg_color, MAIN_STYLE)
  lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, MAIN_STYLE)
  if shadow_color then
    lv_obj_set_style_shadow_width(obj, 18, MAIN_STYLE)
    lv_obj_set_style_shadow_ofs_x(obj, 0, MAIN_STYLE)
    lv_obj_set_style_shadow_ofs_y(obj, 4, MAIN_STYLE)
    lv_obj_set_style_shadow_spread(obj, 0, MAIN_STYLE)
    lv_obj_set_style_shadow_color(obj, shadow_color, MAIN_STYLE)
    lv_obj_set_style_shadow_opa(obj, 96, MAIN_STYLE)
  else
    lv_obj_set_style_shadow_width(obj, 0, MAIN_STYLE)
    lv_obj_set_style_shadow_opa(obj, 0, MAIN_STYLE)
  end
  pcall(function()
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE)
  end)
end

-- 统一创建带位置和基础排版的标签，后面更新时只需要改文本即可。
local function create_label(parent, text, font_ref, color, align, x, y, width)
  local id = lv_label_create(parent)
  lv_label_set_text(id, text_or(text, ""))
  lv_obj_set_style_text_font(id, font_ref, MAIN_STYLE)
  lv_obj_set_style_text_color(id, color, MAIN_STYLE)
  lv_obj_set_style_text_opa(id, 255, MAIN_STYLE)
  if width and width > 0 then
    pcall(function()
      lv_obj_set_width(id, width)
    end)
  end
  lv_obj_align(id, align, x or 0, y or 0)
  return id
end

-- 胶囊徽标统一收口在这里，便于顶部连接态和索引角标共用同一套样式。
local function create_badge(parent, text, font_ref, text_color, bg_color, border_color, align, x, y, width, height)
  local box = lv_obj_create(parent)
  lv_obj_set_size(box, width or 64, height or 20)
  lv_obj_align(box, align or LV_ALIGN_CENTER, x or 0, y or 0)
  apply_card_style(
    box,
    bg_color or 0x152435,
    border_color or 0x37597C,
    math.floor((height or 20) / 2),
    bg_color or 0x152435
  )

  local label = create_label(
    box,
    text,
    font_ref or 12,
    text_color or 0xD8F1FF,
    LV_ALIGN_CENTER,
    0,
    0,
    math.max((width or 64) - 8, 24)
  )
  pcall(function()
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, MAIN_STYLE)
  end)
  return box, label
end

-- 右侧信息区和底部控制区都用这个小节卡片，布局关系更稳定，也便于后续扩展。
local function create_section_card(parent, x, y, width, height, title, bg_color, border_color, grad_color)
  local card = lv_obj_create(parent)
  lv_obj_set_pos(card, x or 0, y or 0)
  lv_obj_set_size(card, width or 80, height or 40)
  apply_card_style(card, bg_color or 0x101826, border_color or 0x2C4662, 14, grad_color or bg_color)

  local title_id = nil
  if title and title ~= "" then
    title_id = create_label(card, title, 12, 0x7CC5FF, LV_ALIGN_TOP_LEFT, 10, 8, (width or 80) - 20)
  end
  return card, title_id
end

-- 标签排版统一走这里，避免多处直接操作 style 时忘记做容错保护。
local function set_label_text_align(id, align)
  if not id then
    return
  end

  pcall(function()
    lv_obj_set_style_text_align(id, align, MAIN_STYLE)
  end)
end

-- 长文本模式根据不同标签分别设置，既保证文件名可读，也避免超宽文本撑坏布局。
local function set_label_long_mode(id, mode)
  if not id then
    return
  end

  pcall(function()
    lv_label_set_long_mode(id, mode)
  end)
end

-- 文本更新统一经由这里，避免 nil 文本和空对象导致重复的防御代码。
local function set_label_text(id, text)
  if id then
    lv_label_set_text(id, text_or(text, ""))
  end
end

-- 少量动态状态会切换高亮色，这里统一封装，避免直接散落 style 调用。
local function set_label_color(id, color)
  if not id then
    return
  end

  pcall(function()
    lv_obj_set_style_text_color(id, color, MAIN_STYLE)
  end)
end

-- 统一读取当前 gamepad 快照；若模块不存在或调用失败，则返回一份可安全使用的空状态。
local function get_gamepad_state()
  if not gamepad or not gamepad.state then
    return {
      started = false,
      connected = false,
      connecting = false,
      buttons_mask = 0,
      lx = 0,
      ly = 0,
      address = nil,
      last_address = nil,
    }
  end

  local ok, state = pcall(function()
    return gamepad.state()
  end)

  if ok and type(state) == "table" then
    return state
  end

  return {
    started = false,
    connected = false,
    connecting = false,
    buttons_mask = 0,
    lx = 0,
    ly = 0,
    address = nil,
    last_address = nil,
  }
end

-- 生成手柄状态的短文本，供选择页右栏和底部提示复用。
local function build_gamepad_panel_text()
  local state = get_gamepad_state()
  APP.gamepad_started = state.started and true or false
  APP.gamepad_connected = state.connected and true or false

  if not gamepad or not gamepad.start then
    return false, "controller missing"
  end

  if state.connected then
    local addr = text_or(state.address, state.last_address)
    if addr ~= "" then
      return false, "linked / " .. clip_text(addr, 14)
    end
    return false, "Xbox linked"
  end

  if state.connecting then
    return false, "pairing..."
  end

  if state.started then
    return false, "hold pair / Y"
  end

  return false, "turn on controller"
end

-- 底部提示固定围绕手柄配对与打开 ROM 的操作，不再混入网页入口说明。
local function build_network_hint_text()
  local state = get_gamepad_state()
  if state.connected then
    return "D-PAD / LS  SELECT"
  end
  if state.connecting then
    return "PAIRING XBOX BLE"
  end
  return "POWER ON PAD  HOLD PAIR"
end

-- 左侧主卡片底部只保留一句短提示，按连接/错误状态切换，避免主视觉被大段说明文字打散。
local function build_rom_focus_text(has_rom, has_error)
  if not has_rom then
    return "COPY .NES TO /SD/NES", 0xFFCB86
  end
  if has_error then
    return "CHECK RIGHT SIDE", 0xFFB773
  end

  local state = get_gamepad_state()
  if state.connected then
    return "A OR MENU TO LOAD", 0x86D5FF
  end
  if state.connecting then
    return "PAIRING XBOX", 0xFFD595
  end
  if state.started then
    return "PAIR CONTROLLER", 0xA8BECE
  end
  return "TURN ON CONTROLLER", 0x8FA7BE
end

local function refresh_control_hint()
  local hint1 = build_network_hint_text()
  local state = get_gamepad_state()
  local badge = "XBOX BLE"
  local hint2 = "Y RESCAN  LOCAL KEYS OK"
  local badge_text = 0xB7E6FF
  local badge_bg = 0x10273A
  local badge_border = 0x3B6790

  if state.connected then
    badge = "PLAYER 1"
    hint2 = "A / MENU OPEN  XBOX EXIT"
    badge_text = 0xDEFFF1
    badge_bg = 0x173126
    badge_border = 0x2D7B58
  elseif state.connecting then
    badge = "PAIRING"
    hint2 = "KEEP CONTROLLER AWAKE"
    badge_text = 0xFFF0CB
    badge_bg = 0x37260D
    badge_border = 0x9F6E1E
  end

  set_label_text(APP.ui.header_badge, badge)
  set_label_color(APP.ui.header_badge, badge_text)
  if APP.ui.header_badge_box then
    lv_obj_set_style_bg_color(APP.ui.header_badge_box, badge_bg, MAIN_STYLE)
    lv_obj_set_style_bg_grad_color(APP.ui.header_badge_box, badge_bg, MAIN_STYLE)
    lv_obj_set_style_border_color(APP.ui.header_badge_box, badge_border, MAIN_STYLE)
  end
  return hint1, hint2
end

-- 重新布局成“纯黑背景 + 左侧主 ROM 卡片 + 右侧极简信息列”的选择页，优先消除重叠。
local function build_ui()
  if lv_clear then
    pcall(function()
      lv_clear()
    end)
    reset_rom_font_state()
  end

  local root = lv_scr_act()
  local rom_font = get_rom_font_ref()
  APP.ui.root = root

  APP.ui.bg = lv_obj_create(root)
  lv_obj_set_pos(APP.ui.bg, 0, 0)
  lv_obj_set_size(APP.ui.bg, 320, 240)
  apply_panel_style(APP.ui.bg, 0x000000)
  lv_obj_set_style_bg_grad_color(APP.ui.bg, 0x000000, MAIN_STYLE)
  lv_obj_set_style_bg_grad_dir(APP.ui.bg, LV_GRAD_DIR_VER, MAIN_STYLE)

  APP.ui.header = lv_obj_create(root)
  lv_obj_set_pos(APP.ui.header, 8, 8)
  lv_obj_set_size(APP.ui.header, 304, 30)
  apply_card_style(APP.ui.header, 0x070707, 0x1E1E1E, 14, 0x070707)

  APP.ui.header_title = create_label(APP.ui.header,
    "NES GAMEPAD",
    14,
    0xF2F2F2,
    LV_ALIGN_LEFT_MID,
    14,
    0,
    160)

  APP.ui.header_badge_box, APP.ui.header_badge = create_badge(
    APP.ui.header,
    "XBOX BLE",
    10,
    0xA9D8FF,
    0x101010,
    0x2F6FA8,
    LV_ALIGN_RIGHT_MID,
    -12,
    0,
    86,
    20
  )

  APP.ui.left_panel = lv_obj_create(root)
  lv_obj_set_pos(APP.ui.left_panel, 8, 46)
  lv_obj_set_size(APP.ui.left_panel, 184, 186)
  apply_card_style(APP.ui.left_panel, 0x050505, 0x1A1A1A, 18, 0x050505)

  APP.ui.right_panel = lv_obj_create(root)
  lv_obj_set_pos(APP.ui.right_panel, 200, 46)
  lv_obj_set_size(APP.ui.right_panel, 112, 186)
  apply_card_style(APP.ui.right_panel, 0x050505, 0x1A1A1A, 18, 0x050505)

  APP.ui.rom_title = create_label(APP.ui.left_panel, "CARTRIDGE", 10, 0x7AA6D1, LV_ALIGN_TOP_LEFT, 14, 12, 92)
  APP.ui.rom_index_box, APP.ui.rom_index = create_badge(
    APP.ui.left_panel,
    "0 / 0",
    10,
    0xE6F2FF,
    0x101010,
    0x3B5875,
    LV_ALIGN_TOP_RIGHT,
    -12,
    10,
    54,
    20
  )

  APP.ui.rom_box = lv_obj_create(APP.ui.left_panel)
  lv_obj_set_pos(APP.ui.rom_box, 12, 36)
  lv_obj_set_size(APP.ui.rom_box, 160, 138)
  apply_card_style(APP.ui.rom_box, 0x0A0A0A, 0x2B6DA5, 18, 0x0A0A0A)
  lv_obj_set_style_outline_width(APP.ui.rom_box, 1, MAIN_STYLE)
  lv_obj_set_style_outline_color(APP.ui.rom_box, 0x14314C, MAIN_STYLE)
  lv_obj_set_style_outline_pad(APP.ui.rom_box, 1, MAIN_STYLE)

  APP.ui.rom_arrow_left = create_label(APP.ui.rom_box, "<", 12, 0x6FA9DD, LV_ALIGN_LEFT_MID, 12, -16, 10)
  APP.ui.rom_arrow_right = create_label(APP.ui.rom_box, ">", 12, 0x6FA9DD, LV_ALIGN_RIGHT_MID, -12, -16, 10)

  APP.ui.rom_name = create_label(APP.ui.rom_box, "-", rom_font, 0xF6F1E6, LV_ALIGN_CENTER, 0, -16, 128)
  set_label_text_align(APP.ui.rom_name, LV_TEXT_ALIGN_CENTER)
  set_label_long_mode(APP.ui.rom_name, LV_LABEL_LONG_SCROLL_CIRCULAR)

  APP.ui.rom_state = create_label(APP.ui.rom_box, "A OR MENU TO LOAD", 10, 0x86D5FF, LV_ALIGN_BOTTOM_MID, 0, -14, 128)
  set_label_text_align(APP.ui.rom_state, LV_TEXT_ALIGN_CENTER)
  set_label_long_mode(APP.ui.rom_state, LV_LABEL_LONG_CLIP)

  APP.ui.status_card, APP.ui.info_title = create_section_card(
    APP.ui.right_panel,
    8,
    8,
    96,
    34,
    nil,
    0x0A0A0A,
    0x202020,
    0x0A0A0A
  )
  APP.ui.status = create_label(APP.ui.status_card, "booting", 10, 0xFFFFFF, LV_ALIGN_CENTER, 0, 0, 84)
  set_label_text_align(APP.ui.status, LV_TEXT_ALIGN_CENTER)
  set_label_long_mode(APP.ui.status, LV_LABEL_LONG_CLIP)

  APP.ui.meta_card, APP.ui.meta_title = create_section_card(
    APP.ui.right_panel,
    8,
    50,
    96,
    70,
    nil,
    0x0A0A0A,
    0x202020,
    0x0A0A0A
  )
  APP.ui.mapper = create_label(APP.ui.meta_card, "-", 10, 0xF2EEE3, LV_ALIGN_TOP_LEFT, 10, 10, 76)
  set_label_long_mode(APP.ui.mapper, LV_LABEL_LONG_CLIP)
  APP.ui.mirror = create_label(APP.ui.meta_card, "-", 10, 0x93B8D8, LV_ALIGN_TOP_LEFT, 10, 26, 76)
  set_label_long_mode(APP.ui.mirror, LV_LABEL_LONG_CLIP)
  APP.ui.banks = create_label(APP.ui.meta_card, "-", 10, 0x7594B2, LV_ALIGN_TOP_LEFT, 10, 42, 76)
  set_label_long_mode(APP.ui.banks, LV_LABEL_LONG_WRAP)

  APP.ui.message_card, APP.ui.error_title = create_section_card(
    APP.ui.right_panel,
    8,
    128,
    96,
    50,
    nil,
    0x0A0A0A,
    0x202020,
    0x0A0A0A
  )
  APP.ui.error_title = create_label(APP.ui.message_card, "", 10, 0xFFB773, LV_ALIGN_TOP_LEFT, 10, 8, 76)
  APP.ui.error = create_label(APP.ui.message_card, "", 10, 0xDCE8F3, LV_ALIGN_TOP_LEFT, 10, 22, 76)
  set_label_long_mode(APP.ui.error, LV_LABEL_LONG_WRAP)

  refresh_control_hint()
end

-- 顶部说明在新布局里已经删掉，这里保留空实现，方便继续复用原有启动/返回调用点。
local function set_tip(text)
  return text
end

-- 状态只保留一行短文本，同时继续打串口，方便屏幕与日志保持一致。
local function set_status(text)
  APP.last_status = text_or(text, "")
  set_label_text(APP.ui.status, clip_text(APP.last_status, 18))
  print("[nes_game]", APP.last_status)
end

-- 错误/手柄状态都走这里展示；默认标题仍为 ERROR，便于调用点不改时保持旧行为。
local function set_error_text(text, title)
  local s = compact_error_text(text)
  if s == "" then
    set_label_text(APP.ui.error_title, "")
    set_label_text(APP.ui.error, "")
    return
  end

  if title == false or title == "" then
    set_label_text(APP.ui.error_title, "")
  else
    set_label_text(APP.ui.error_title, text_or(title, "ERROR"))
  end
  set_label_text(APP.ui.error, s)
  if title == "ERROR" then
    set_label_color(APP.ui.error_title, 0xFFB773)
  elseif title and title ~= false and title ~= "" then
    set_label_color(APP.ui.error_title, 0x7CC5FF)
  end
end

-- 选择页刷新统一落在这里：左边聚焦当前 ROM，右边展示状态、ROM 信息和错误/手柄提示。
local function render_selector_ui(status_text, detail_text)
  APP.screen_mode = "select"

  if #APP.roms <= 0 then
    set_status(status_text or "NO ROM")
    set_label_text(APP.ui.rom_index, "0 / 0")
    set_label_text(APP.ui.rom_name, "NO ROM")
    set_label_text(APP.ui.rom_state, "COPY .NES TO /SD/NES")
    set_label_color(APP.ui.rom_state, 0xFFCB86)
    set_label_text(APP.ui.mapper, "-")
    set_label_text(APP.ui.mirror, "-")
    set_label_text(APP.ui.banks, "-")
    set_error_text(detail_text or "copy .nes files into /sd/nes")
    refresh_control_hint()
    return
  end

  local current = get_selected_rom()
  local current_index = APP.selected_index or 1
  local count = #APP.roms
  local meta = get_rom_meta(current and current.path or "")
  local error_text = detail_text

  if (not error_text or error_text == "") and
    APP.last_open_error_path and APP.last_open_error_path == (current and current.path) then
    error_text = APP.last_open_error_text
  end
  if (not error_text or error_text == "") and meta and meta.error then
    error_text = meta.error
  end

  local rom_focus_text, rom_focus_color = build_rom_focus_text(true, error_text and error_text ~= "")
  set_status(status_text or "READY")
  set_label_text(APP.ui.rom_index, tostring(current_index) .. " / " .. tostring(count))
  set_label_text(APP.ui.rom_name, display_rom_name(current and current.name or basename_from_path(current and current.path or "")))
  set_label_text(APP.ui.rom_state, rom_focus_text)
  set_label_color(APP.ui.rom_state, rom_focus_color)
  set_label_text(APP.ui.mapper, text_or(meta and meta.mapper_text, "-"))
  set_label_text(APP.ui.mirror, text_or(meta and meta.mirror_text, "-"))
  set_label_text(APP.ui.banks, text_or(meta and (meta.prg_text .. "\n" .. meta.chr_text), "-"))
  if error_text and error_text ~= "" then
    set_error_text(error_text, "ERROR")
  else
    local panel_title, panel_text = build_gamepad_panel_text()
    set_error_text(panel_text, panel_title)
  end
  refresh_control_hint()
end

-- 下面几项会被 gamepad 事件处理提前引用，这里先做前向声明，避免 Lua 把它们当成全局名。
local show_selector_page
local start_selected_rom
local exit_app_to_launcher

-- 当前是否处于 NES 运行态统一从这里判断，避免选择页也误把手柄输入下发到底层。
local function is_nes_running()
  local snapshot = nes.state()
  return snapshot and snapshot.running and true or false
end

-- 真正写 NES 手柄位图时统一收口在这里，避免运行态、返回选择页和清理路径各写各的。
local function apply_input_mask(mask)
  local value = sanitize_mask(mask)
  if not is_nes_running() then
    APP.applied_mask = 0
    return true
  end

  local ok, result = pcall(function()
    return nes.input.set_mask(APP.PLAYER, value)
  end)

  if not ok or not result then
    print("[nes_game] set_mask failed", tostring(value))
    return false
  end

  APP.applied_mask = value
  return true
end

-- 当前脚本只保留一条本地手柄输入链，因此同步时只需要把本地位图下发给 NES。
local function sync_input_mask(force)
  local mask = sanitize_mask(APP.local_mask or 0)
  if not force and mask == APP.applied_mask then
    return true
  end
  return apply_input_mask(mask)
end

-- 脚本切换或退出时主动清空手柄位，避免旧实例残留输入继续影响新会话。
local function clear_input_state()
  APP.local_mask = 0
  APP.applied_mask = -1
  APP.gamepad_prev_buttons = 0
  APP.gamepad_prev_nav = 0

  pcall(function()
    nes.input.clear(APP.PLAYER)
  end)
end

-- 统一判断一个 bit 是否刚从“未按下”变成“按下”，供选择页导航和返回逻辑复用。
local function edge_pressed(prev_mask, current_mask, bit)
  local prev_has = ((prev_mask or 0) & bit) ~= 0
  local current_has = ((current_mask or 0) & bit) ~= 0
  return current_has and (not prev_has)
end

-- 左摇杆也允许充当方向键，阈值保持略高一些，避免轻微漂移导致误触。
local function axis_less_than(value, threshold)
  return type(value) == "number" and value <= -math.abs(threshold or APP.GAMEPAD_AXIS_THRESHOLD)
end

local function axis_greater_than(value, threshold)
  return type(value) == "number" and value >= math.abs(threshold or APP.GAMEPAD_AXIS_THRESHOLD)
end

-- 统一计算 NES 输入位图，便于后续如果要换手柄布局时只改这一处。
local function build_gamepad_nes_mask(state)
  local up = state.dpad_up or axis_greater_than(state.ly)
  local down = state.dpad_down or axis_less_than(state.ly)
  local left = state.dpad_left or axis_less_than(state.lx)
  local right = state.dpad_right or axis_greater_than(state.lx)
  local mask = 0

  if up and down then
    up = false
    down = false
  end
  if left and right then
    left = false
    right = false
  end

  if up then
    mask = mask | nes.BTN_UP
  end
  if down then
    mask = mask | nes.BTN_DOWN
  end
  if left then
    mask = mask | nes.BTN_LEFT
  end
  if right then
    mask = mask | nes.BTN_RIGHT
  end

  if state.a or state.x or state.rb then
    mask = mask | nes.BTN_A
  end
  if state.b or state.y or state.lb then
    mask = mask | nes.BTN_B
  end
  if state.view then
    mask = mask | nes.BTN_SELECT
  end
  if state.menu then
    mask = mask | nes.BTN_START
  end

  return sanitize_mask(mask)
end

-- 选择页和游戏运行页的手柄语义不同，因此这里单独定义一个较小的导航位图。
local function build_gamepad_nav_mask(state)
  local mask = 0

  if state.dpad_left or axis_less_than(state.lx, 0.60) then
    mask = mask | APP.GAMEPAD_NAV_LEFT
  end
  if state.dpad_right or axis_greater_than(state.lx, 0.60) then
    mask = mask | APP.GAMEPAD_NAV_RIGHT
  end
  if state.a or state.menu then
    mask = mask | APP.GAMEPAD_NAV_CONFIRM
  end
  if state.xbox then
    mask = mask | APP.GAMEPAD_NAV_BACK
  end
  if state.y then
    mask = mask | APP.GAMEPAD_NAV_RESCAN
  end

  return mask
end

-- 选择页里优先让手柄直接操作目录，避免用户还要退回设备本体按键。
local function handle_selector_gamepad_nav(state)
  local nav_mask = build_gamepad_nav_mask(state)
  local prev_nav = APP.gamepad_prev_nav or 0
  APP.gamepad_prev_nav = nav_mask

  if APP.screen_mode ~= "select" then
    return
  end

  if edge_pressed(prev_nav, nav_mask, APP.GAMEPAD_NAV_LEFT) then
    move_selected_rom(-1)
    render_selector_ui("SELECT ROM")
    return
  end

  if edge_pressed(prev_nav, nav_mask, APP.GAMEPAD_NAV_RIGHT) then
    move_selected_rom(1)
    render_selector_ui("SELECT ROM")
    return
  end

  if edge_pressed(prev_nav, nav_mask, APP.GAMEPAD_NAV_CONFIRM) then
    start_selected_rom()
    return
  end

  if edge_pressed(prev_nav, nav_mask, APP.GAMEPAD_NAV_RESCAN) and gamepad and gamepad.rescan then
    pcall(function()
      gamepad.rescan()
    end)
    render_selector_ui("RESCAN XBOX")
    return
  end

  if edge_pressed(prev_nav, nav_mask, APP.GAMEPAD_NAV_BACK) then
    exit_app_to_launcher()
  end
end

-- 游戏运行时把 Xbox 输入直接映射成 NES 掩码，并保留 `Xbox` 键返回选择页的入口。
local function apply_gamepad_state(state, force_sync)
  local snapshot = state or get_gamepad_state()
  local current_buttons = tonumber(snapshot.buttons_mask) or 0
  local prev_buttons = APP.gamepad_prev_buttons or 0

  APP.gamepad_started = snapshot.started and true or false
  APP.gamepad_connected = snapshot.connected and true or false

  if APP.screen_mode == "running" and snapshot.connected then
    APP.local_mask = build_gamepad_nes_mask(snapshot)
    sync_input_mask(force_sync and true or false)

    if edge_pressed(prev_buttons, current_buttons, gamepad.BTN_XBOX) then
      APP.gamepad_prev_buttons = current_buttons
      show_selector_page("RETURNED TO SELECTOR", nil, APP.rom_path)
      return
    end
  else
    if APP.local_mask ~= 0 then
      APP.local_mask = 0
      sync_input_mask(true)
    end

    if snapshot.connected then
      handle_selector_gamepad_nav(snapshot)
    else
      APP.gamepad_prev_nav = 0
    end
  end

  APP.gamepad_prev_buttons = current_buttons
end

-- 启动并注册 gamepad 回调；所有 Lua 回调都只做轻逻辑，避免高频输入时卡 UI。
local function start_gamepad_service()
  if not gamepad or not gamepad.start or not gamepad.on or not gamepad.off then
    set_status("CTRL UNAVAILABLE")
    set_error_text("firmware missing gamepad module", "ERROR")
    return false
  end

  pcall(function()
    gamepad.off()
  end)

  local ok_call, ok_start, err = pcall(function()
    return gamepad.start({
      clear_bonds = false,
      debug = false,
    })
  end)

  if not ok_call or not ok_start then
    set_status("CTRL START FAIL")
    set_error_text(tostring(err or ok_start), "ERROR")
    return false
  end

  APP.gamepad_prev_buttons = 0
  APP.gamepad_prev_nav = 0

  gamepad.on(gamepad.EVT_CONNECTING, function()
    APP.gamepad_started = true
    APP.gamepad_connected = false
    if APP.screen_mode == "select" then
      render_selector_ui("PAIRING XBOX")
    end
  end)

  gamepad.on(gamepad.EVT_CONNECT_FAILED, function()
    APP.gamepad_connected = false
    APP.local_mask = 0
    sync_input_mask(true)
    if APP.screen_mode == "select" then
      render_selector_ui("PAIR FAILED")
    end
  end)

  gamepad.on(gamepad.EVT_CONNECTED, function()
    APP.gamepad_prev_buttons = 0
    APP.gamepad_prev_nav = 0
    apply_gamepad_state(get_gamepad_state(), true)
    if APP.screen_mode == "select" then
      render_selector_ui("XBOX READY")
    end
  end)

  gamepad.on(gamepad.EVT_DISCONNECTED, function()
    APP.gamepad_connected = false
    APP.gamepad_prev_buttons = 0
    APP.gamepad_prev_nav = 0
    APP.local_mask = 0
    sync_input_mask(true)
    if APP.screen_mode == "select" then
      render_selector_ui("LINK LOST")
    else
      set_status("controller disconnected")
    end
  end)

  gamepad.on(gamepad.EVT_UPDATE, function()
    apply_gamepad_state(get_gamepad_state(), false)
  end)

  apply_gamepad_state(get_gamepad_state(), true)
  return true
end

-- 只传当前运行时允许 Lua 调整的参数；画面尺寸、直出模式和音频等使用底层默认值。
local function build_runtime_options()
  return {
    target_fps = 60,
    -- 每次提交 48 行，兼顾 DMA 流水和单块传输内存占用。
    transfer_rows = 48,
  }
end

-- 启动前若已有旧会话，先停掉再重开，避免 ROM 重载时和上一次运行态互相干扰。
local function stop_previous_session()
  local snapshot = nes.state()
  if snapshot and snapshot.running then
    pcall(function()
      nes.stop()
    end)
  end
  clear_input_state()
end

-- 选择页和运行态切换都收口到这里，保证 direct render 退出后 UI 能完整重建。
show_selector_page = function(status_text, detail_text, preferred_path)
  stop_previous_session()
  build_ui()
  refresh_rom_catalog(preferred_path)
  render_selector_ui(status_text, detail_text)
  return #APP.roms > 0
end

-- 启动前显式关闭系统默认的 HOME 短按退出，让这个 app 自己接管长按返回逻辑。
local function configure_home_exit_behavior()
  if not app or not app.set_home_exit then
    print("[nes_game] app.set_home_exit unavailable")
    return false
  end

  local ok, result, err = pcall(function()
    return app.set_home_exit(false)
  end)
  if not ok or not result then
    print("[nes_game] disable HOME short exit failed", tostring(err or result))
    return false
  end

  print("[nes_game] HOME short exit disabled for ROM selector flow")
  return true
end

-- 进入游戏前统一从当前选中项取 ROM；纯手柄版只需要确认当前目录里有可用 ROM。
start_selected_rom = function()
  local rom = get_selected_rom()
  if not rom or not rom.path or rom.path == "" then
    render_selector_ui("NO ROM", APP.ROM_DIR)
    return false
  end

  stop_previous_session()

  APP.last_open_error_path = nil
  APP.last_open_error_text = nil
  APP.rom_path = rom.path
  set_tip("running")
  set_status("OPENING")

  local ok, err = nes.start(rom.path, build_runtime_options())
  if not ok then
    APP.last_open_error_path = rom.path
    APP.last_open_error_text = text_or(err, "unknown error")
    show_selector_page("OPEN FAILED", APP.last_open_error_text, rom.path)
    return false
  end

  APP.last_open_error_path = nil
  APP.last_open_error_text = nil
  APP.screen_mode = "running"
  set_status("RUNNING")
  sync_input_mask(true)
  return true
end

-- 业务 app 内仍然保留一个明确的“退出到 launcher”入口，避免关闭短按 HOME 后被困在本 app。
exit_app_to_launcher = function()
  if APP.is_shutting_down then
    return
  end

  set_status("Leaving NES app ...")
  APP.shutdown()
  pcall(function()
    app.exit()
  end)
end

-- 左右键只在选择页生效；进入游戏后，输入优先交给 Xbox BLE 手柄。
local function register_key_handlers()
  if not key or not key.on then
    print("[nes_game] key module unavailable")
    return
  end

  pcall(function()
    key.off()
  end)

  key.on(key.LEFT, function(evt_type, ts_ms)
    if APP.screen_mode ~= "select" then
      return
    end

    if evt_type == key.START then
      move_selected_rom(-1)
      render_selector_ui()
    elseif evt_type == key.LONG_START then
      APP.repeat_left = 0
      move_selected_rom(-1)
      render_selector_ui()
    elseif evt_type == key.LONG_REPEAT then
      APP.repeat_left = (APP.repeat_left or 0) + 1
      if (APP.repeat_left % 3) == 0 then
        move_selected_rom(-1)
        render_selector_ui()
      end
    elseif evt_type == key.LONG_END then
      APP.repeat_left = 0
    end
  end)

  key.on(key.RIGHT, function(evt_type, ts_ms)
    if APP.screen_mode ~= "select" then
      return
    end

    if evt_type == key.START then
      move_selected_rom(1)
      render_selector_ui()
    elseif evt_type == key.LONG_START then
      APP.repeat_right = 0
      move_selected_rom(1)
      render_selector_ui()
    elseif evt_type == key.LONG_REPEAT then
      APP.repeat_right = (APP.repeat_right or 0) + 1
      if (APP.repeat_right % 3) == 0 then
        move_selected_rom(1)
        render_selector_ui()
      end
    elseif evt_type == key.LONG_END then
      APP.repeat_right = 0
    end
  end)

  key.on(key.UP, function(evt_type, ts_ms)
    if APP.screen_mode ~= "select" then
      return
    end

    if evt_type == key.LONG_START then
      start_selected_rom()
    end
  end)

  key.on(key.HOME, function(evt_type, ts_ms)
    if evt_type ~= key.LONG_START then
      return
    end

    if APP.screen_mode == "running" then
      show_selector_page("Returned to ROM selector", nil, APP.rom_path)
    elseif APP.screen_mode == "select" then
      exit_app_to_launcher()
    end
  end)
end

-- 启动流程固定拉起 Xbox BLE 手柄，并把当前连接状态同步到选择页。
local function boot()
  show_selector_page("BOOT", nil, APP.PREFERRED_ROM_PATH)
  clear_input_state()
  configure_home_exit_behavior()
  register_key_handlers()

  if not start_gamepad_service() then
    return false
  end

  local state = get_gamepad_state()
  if state.connected then
    render_selector_ui("XBOX READY")
  elseif state.connecting then
    render_selector_ui("PAIRING XBOX")
  else
    render_selector_ui("PAIR XBOX")
  end

  return true
end

-- 统一收尾，确保切 app 时 NES 后台任务、蓝牙手柄和输入状态都能干净回收。
function APP.shutdown()
  if APP.is_shutting_down then
    return
  end

  APP.is_shutting_down = true
  if gamepad then
    pcall(function()
      gamepad.off()
    end)
    pcall(function()
      gamepad.stop()
    end)
  end
  pcall(function()
    if app and app.set_home_exit then
      app.set_home_exit(true)
    end
  end)
  pcall(function()
    key.off()
  end)
  clear_input_state()
  pcall(function()
    nes.stop()
  end)
  free_rom_font()
end

boot()
