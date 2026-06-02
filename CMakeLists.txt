cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nes)

# esp-elf-loader 1.3.x project_so() only compiles C files directly.
# The C++ NES core is built by the main component and linked from libmain.a.
set(ELF_LIBS "${CMAKE_BINARY_DIR}/esp-idf/main/libmain.a")

include(elf_loader)
project_so(nes)
