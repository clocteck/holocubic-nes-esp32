set(NES_MODULE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MODULE_ABI_DIR "" CACHE PATH "Directory containing module_abi.h")

if(NOT MODULE_ABI_DIR AND DEFINED ENV{CUBICLUA_ROOT} AND EXISTS "$ENV{CUBICLUA_ROOT}/src/dynmod/module_abi.h")
    set(MODULE_ABI_DIR "$ENV{CUBICLUA_ROOT}/src/dynmod")
endif()

if(NOT MODULE_ABI_DIR AND EXISTS "${NES_MODULE_ROOT}/include/module_abi.h")
    set(MODULE_ABI_DIR "${NES_MODULE_ROOT}/include")
endif()

if(NOT MODULE_ABI_DIR AND EXISTS "${NES_MODULE_ROOT}/../../../../src/dynmod/module_abi.h")
    set(MODULE_ABI_DIR "${NES_MODULE_ROOT}/../../../../src/dynmod")
endif()

if(NOT EXISTS "${MODULE_ABI_DIR}/module_abi.h")
    message(FATAL_ERROR "module_abi.h not found. Set -DMODULE_ABI_DIR=/path/to/src/dynmod or CUBICLUA_ROOT.")
endif()

set(NES_CORE_SRCS
    "../core/apu2A03.cpp"
    "../core/bus.cpp"
    "../core/cartridge.cpp"
    "../core/cpu6502.cpp"
    "../core/mapper.cpp"
    "../core/power_on_ram.cpp"
    "../core/ppu2C02.cpp"
    "../core/mappers/mapper000.cpp"
    "../core/mappers/mapper001.cpp"
    "../core/mappers/mapper002.cpp"
    "../core/mappers/mapper003.cpp"
    "../core/mappers/mapper004.cpp"
    "../core/mappers/mapper007.cpp"
    "../core/mappers/mapper015.cpp"
    "../core/mappers/mapper069.cpp"
    "../core/mappers/mapper226.cpp"
    "../port/nes_port.cpp"
    "../runtime/nes_core_bridge.cpp"
    "../video/nes_video_out.cpp"
)

idf_component_register(
    SRCS
        "nes_module.c"
        ${NES_CORE_SRCS}
    INCLUDE_DIRS
        "."
        "${NES_MODULE_ROOT}"
        "${NES_MODULE_ROOT}/port"
        "${MODULE_ABI_DIR}"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -fPIC
    -fvisibility=hidden
    -fdata-sections
    -ffunction-sections
)
